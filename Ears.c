/*
 * R2-3PO Ears
 *
 * General-purpose raw audio input system for R2.
 *
 * Ears provides R2 with PCM audio and tells him where
 * that audio originated:
 *
 *     FILE     -> supplied audio/media file
 *     WORLD    -> microphone / physical environment
 *     DESKTOP  -> computer/system audio
 *     SOURCE   -> arbitrary PulseAudio/PipeWire source
 *
 * Ears does NOT interpret the audio.
 *
 * It does not:
 *     - transcribe speech
 *     - identify songs
 *     - determine emotions
 *     - recognize speakers
 *     - create memories
 *
 * Those responsibilities belong to the hearing/cognition layer.
 *
 * Ears is the input layer only.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Ears.h"
#include "Log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/pulseaudio.h>

extern char **environ;


/* ---------------------------------------------------------
 * Internal constants
 * --------------------------------------------------------- */

/*
 * Canonical PCM format used by the hearing pipeline.
 *
 * Every source is converted to:
 *
 *     48000 Hz
 *     stereo
 *     signed 16-bit little-endian PCM
 *
 * This gives the downstream hearing system one consistent
 * audio format regardless of whether the source was:
 *
 *     microphone
 *     desktop audio
 *     MP3
 *     WAV
 *     FLAC
 *     OGG
 *     movie audio
 *     game audio
 *     browser audio
 *     etc.
 */
#define R2_EARS_SAMPLE_RATE       48000
#define R2_EARS_CHANNELS          2
#define R2_EARS_BITS_PER_SAMPLE   16

#define R2_EARS_SOURCE_NAME_MAX   512

/*
 * Ten milliseconds of audio.
 *
 * This keeps reads reasonably small for real-time hearing
 * while avoiding excessive system calls.
 */
#define R2_EARS_FRAME_MS          10

#define R2_EARS_FRAME_BYTES \
    ((size_t)R2_EARS_SAMPLE_RATE * \
     (size_t)R2_EARS_CHANNELS * \
     (size_t)(R2_EARS_BITS_PER_SAMPLE / 8) * \
     R2_EARS_FRAME_MS / 1000)


/* ---------------------------------------------------------
 * Internal Ears structure
 * --------------------------------------------------------- */

struct R2Ears
{
    R2HearingEvent event;

    int open;

    /*
     * PulseAudio/PipeWire live capture.
     *
     * PipeWire normally exposes its PulseAudio compatibility
     * layer through libpulse, so this works for both systems.
     */
    pa_mainloop *pulse_loop;
    pa_context *pulse_context;
    pa_stream *pulse_stream;
    const unsigned char *pulse_data;
    size_t pulse_remaining;
    pthread_mutex_t event_lock;

    /*
     * FFmpeg process for file/media decoding.
     *
     * FFmpeg outputs canonical raw PCM through this pipe.
     */
    int file_fd;

    pid_t file_pid;

    /*
     * Whether the current source is a file.
     */
    int using_file;

    /*
     * PCM format used internally.
     */
    R2AudioFormat format;

    /* Aggregated sensory journal counters; never log every 10 ms read. */
    uint64_t bytes_since_log;
    uint64_t last_log_timestamp_us;
};


/* ---------------------------------------------------------
 * Utility functions
 * --------------------------------------------------------- */

static uint64_t r2_ears_timestamp(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return ((uint64_t)ts.tv_sec * 1000000ULL) +
           ((uint64_t)ts.tv_nsec / 1000ULL);
}


/*
 * Safely quote a string for use as one shell argument.
 *
 * This matters for:
 *
 *     /home/x/My Music/song.mp3
 *     /home/x/Movie's Audio/movie.mkv
 *
 * and also prevents a filename from becoming shell syntax.
 */
static char *r2_ears_shell_quote(const char *input)
{
    size_t length;
    size_t max_size;
    char *output;
    char *p;

    if (!input)
        return NULL;

    length = strlen(input);

    /*
     * Worst case:
     *
     * every character is a single quote and therefore becomes
     *
     *     '\''
     *
     * plus surrounding quotes.
     */
    if (length > (SIZE_MAX - 3) / 4)
        return NULL;

    max_size = (length * 4) + 3;

    output = malloc(max_size);

    if (!output)
        return NULL;

    p = output;

    *p++ = '\'';

    for (size_t i = 0; i < length; ++i)
    {
        if (input[i] == '\'')
        {
            memcpy(p, "'\\''", 4);
            p += 4;
        }
        else
        {
            *p++ = input[i];
        }
    }

    *p++ = '\'';
    *p = '\0';

    return output;
}


static void r2_ears_reset_event(R2Ears *ears)
{
    if (!ears)
        return;

    memset(
        &ears->event,
        0,
        sizeof(ears->event)
    );

    ears->event.origin = R2_HEARING_NONE;
    ears->event.type = R2_AUDIO_UNKNOWN;

    ears->event.format = ears->format;
    ears->event.timestamp =
        r2_ears_timestamp();
}


static void r2_ears_set_source_name(
    R2Ears *ears,
    const char *name
)
{
    if (!ears)
        return;

    if (!name)
    {
        ears->event.source_name[0] = '\0';
        return;
    }

    snprintf(
        ears->event.source_name,
        sizeof(ears->event.source_name),
        "%s",
        name
    );
}


/* ---------------------------------------------------------
 * PulseAudio / PipeWire helpers
 * --------------------------------------------------------- */

static pa_sample_spec r2_ears_sample_spec(void)
{
    pa_sample_spec spec;

    memset(
        &spec,
        0,
        sizeof(spec)
    );

    spec.format = PA_SAMPLE_S16LE;
    spec.rate = R2_EARS_SAMPLE_RATE;
    spec.channels = R2_EARS_CHANNELS;

    return spec;
}


/*
 * Get the system's default Pulse/PipeWire sink.
 *
 * Example:
 *
 *     alsa_output.pci-0000_00_1f.3.analog-stereo
 *
 * The corresponding monitor source is:
 *
 *     alsa_output.pci-0000_00_1f.3.analog-stereo.monitor
 *
 * This is what allows R2 to hear what the desktop itself
 * is playing.
 */
static char *r2_ears_get_default_sink(void)
{
    FILE *pipe;
    char buffer[1024];

    pipe = popen(
        "pactl get-default-sink 2>/dev/null",
        "r"
    );

    if (!pipe)
        return NULL;

    if (!fgets(
            buffer,
            sizeof(buffer),
            pipe
        ))
    {
        pclose(pipe);
        return NULL;
    }

    pclose(pipe);

    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (buffer[0] == '\0')
        return NULL;

    return strdup(buffer);
}


/*
 * Get the default Pulse/PipeWire recording source.
 *
 * This is more robust than simply assuming the microphone
 * has a particular device name.
 */
static char *r2_ears_get_default_source(void)
{
    FILE *pipe;
    char buffer[1024];

    pipe = popen(
        "pactl get-default-source 2>/dev/null",
        "r"
    );

    if (!pipe)
        return NULL;

    if (!fgets(
            buffer,
            sizeof(buffer),
            pipe
        ))
    {
        pclose(pipe);
        return NULL;
    }

    pclose(pipe);

    buffer[strcspn(buffer, "\r\n")] = '\0';

    if (buffer[0] == '\0')
        return NULL;

    return strdup(buffer);
}


/*
 * Verify that a Pulse/PipeWire source exists.
 *
 * This is deliberately lightweight. The actual opening is still
 * performed by pa_simple_new().
 */
static int r2_ears_source_exists(const char *source)
{
    FILE *pipe;
    char *quoted_source;
    char command[2048];
    int found = 0;

    if (!source || source[0] == '\0')
        return 0;

    quoted_source =
        r2_ears_shell_quote(source);

    if (!quoted_source)
        return 0;

    int command_length = snprintf(
        command,
        sizeof(command),
        "pactl list short sources 2>/dev/null | "
        "grep -F -- %s >/dev/null 2>&1",
        quoted_source
    );

    free(quoted_source);

    if (command_length < 0 || (size_t)command_length >= sizeof(command))
        return 0;

    pipe = popen(command, "r");

    if (!pipe)
        return 0;

    /*
     * grep's exit status is not directly portable through the
     * stdio API, but pclose() gives us enough information here.
     */
    if (pclose(pipe) == 0)
        found = 1;

    return found;
}


/* Availability discovery is separate from sound capture. Both USB and built-in
 * microphone names come from the active Pulse/PipeWire server, not an app list.
 * A monitor is desktop playback, not a physical microphone. */
static int r2_ears_is_monitor(const char *name)
{
    if (!name) return 1;
    size_t n = strlen(name);
    return n >= 8 && strcmp(name + n - 8, ".monitor") == 0;
}

static int r2_ears_find_microphone(char *out, size_t capacity)
{
    char preferred[512] = {0}, fallback[512] = {0}, line[2048];
    char *default_name = r2_ears_get_default_source();
    FILE *pipe = popen("pactl list short sources 2>/dev/null", "r");
    if (!out || capacity == 0) {
        free(default_name);
        if (pipe) pclose(pipe);
        return -1;
    }
    out[0] = '\0';
    if (!pipe) {
        free(default_name);
        return -1;
    }
    while (fgets(line, sizeof(line), pipe)) {
        char name[512] = {0};
        if (sscanf(line, "%*u %511s", name) != 1 ||
            r2_ears_is_monitor(name))
            continue;
        if (!fallback[0]) snprintf(fallback, sizeof(fallback), "%s", name);
        if (default_name && strcmp(name, default_name) == 0)
            snprintf(preferred, sizeof(preferred), "%s", name);
    }
    int closed = pclose(pipe);
    free(default_name);
    if (closed != 0) return -1;
    const char *selected = preferred[0] ? preferred : fallback;
    if (!*selected || snprintf(out, capacity, "%s", selected) >= (int)capacity)
        return -1;
    return 0;
}

int r2_ears_microphone_available(void)
{
    char source[512];
    return r2_ears_find_microphone(source, sizeof(source)) == 0;
}

/* A live, uncorked playback stream is an audio source regardless of whether
 * it belongs to a browser, movie player, audio player or game. Paused/corked
 * streams do not keep the continuous desktop listener alive. */
int r2_ears_desktop_playing(void)
{
    FILE *pipe = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (!pipe) return 0;
    char line[2048];
    int playing = 0, have_input = 0;
    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, "Sink Input #")) have_input = 1;
        if (have_input && strstr(line, "Corked: no"))
            playing = 1;
    }
    if (pclose(pipe) != 0) return 0;
    return playing;
}

/* Resolve the real process responsible for an uncorked output stream.
 * It is evidence of desktop playback, not evidence that a video exists.
 * The optional Eyes bridge can look for a visible window owned by this PID. */
long r2_ears_playback_pid(void)
{
    FILE *pipe = popen("pactl list sink-inputs 2>/dev/null", "r");
    if (!pipe) return -1;
    char line[2048];
    long current_pid = -1, selected = -1;
    int active = 0, seen_input = 0;
    while (fgets(line, sizeof(line), pipe)) {
        if (strstr(line, "Sink Input #")) {
            if (seen_input && active && current_pid > 1 && selected < 0)
                selected = current_pid;
            seen_input = 1;
            active = 0;
            current_pid = -1;
        } else if (seen_input && strstr(line, "Corked: ")) {
            active = strstr(line, "Corked: no") != NULL;
        } else if (seen_input) {
            char *key = strstr(line, "application.process.id = ");
            if (key) {
                key += strlen("application.process.id = ");
                if (*key == '"') ++key;
                char *end = NULL;
                errno = 0;
                long value = strtol(key, &end, 10);
                if (!errno && end != key && value > 1 && value <= INT_MAX &&
                    (*end == '"' || *end == '\n' || *end == '\0'))
                    current_pid = value;
            }
        }
    }
    if (seen_input && active && current_pid > 1 && selected < 0)
        selected = current_pid;
    if (pclose(pipe) != 0) return -1;
    return selected;
}

/* ---------------------------------------------------------
 * File process handling
 * --------------------------------------------------------- */

static int r2_ears_start_ffmpeg(
    R2Ears *ears,
    const char *path
)
{
    int descriptors[2];
    pid_t child = -1;
    posix_spawn_file_actions_t actions;
    if (!ears || !path || !*path) return -1;
    if (pipe2(descriptors, O_CLOEXEC) != 0) return -1;
    /* Keep child actions correct even when stdin/stdout were closed by host. */
    for (int i = 0; i < 2; ++i) {
        if (descriptors[i] <= STDERR_FILENO) {
            int fd = fcntl(descriptors[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (fd < 0) {
                close(descriptors[0]); close(descriptors[1]); return -1;
            }
            close(descriptors[i]); descriptors[i] = fd;
        }
    }
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) goto fail;
    rc = posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, descriptors[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, descriptors[1]);
    char *argv[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
                    "-i", (char *)path, "-vn", "-f", "s16le", "-ar", "48000",
                    "-ac", "2", "-acodec", "pcm_s16le", "pipe:1", NULL};
    if (rc == 0) rc = posix_spawnp(&child, "ffmpeg", &actions,
                                   NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) goto fail;
    close(descriptors[1]);
    ears->file_fd = descriptors[0];
    ears->file_pid = child;
    return 0;
fail:
    close(descriptors[0]); close(descriptors[1]);
    errno = rc;
    return -1;
}


/* ---------------------------------------------------------
 * Initialization
 * --------------------------------------------------------- */

int r2_ears_init(R2Ears **ears)
{
    R2Ears *new_ears;

    if (!ears)
        return -1;

    *ears = NULL;

    new_ears =
        calloc(1, sizeof(R2Ears));

    if (!new_ears)
        return -1;

    new_ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    new_ears->format.channels =
        R2_EARS_CHANNELS;

    new_ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    new_ears->open = 0;
    new_ears->pulse_stream = NULL;
    new_ears->file_fd = -1;
    if (pthread_mutex_init(&new_ears->event_lock, NULL) != 0) {
        free(new_ears); return -1;
    }
    new_ears->file_pid = -1;
    new_ears->using_file = 0;

    r2_ears_reset_event(new_ears);

    *ears = new_ears;

    return 0;
}


/* ---------------------------------------------------------
 * Close current source
 * --------------------------------------------------------- */

void r2_ears_close(R2Ears *ears)
{
    if (!ears)
        return;

    /* Caller joins its reader before close/reopen; read errors close themselves. */
    if (ears->pulse_stream) {
        pa_stream_disconnect(ears->pulse_stream);
        pa_stream_unref(ears->pulse_stream);
        ears->pulse_stream = NULL;
    }
    ears->pulse_data = NULL;
    ears->pulse_remaining = 0;
    if (ears->pulse_context) {
        pa_context_disconnect(ears->pulse_context);
        pa_context_unref(ears->pulse_context);
        ears->pulse_context = NULL;
    }
    if (ears->pulse_loop) {
        pa_mainloop_free(ears->pulse_loop);
        ears->pulse_loop = NULL;
    }
    if (ears->file_fd >= 0) {
        close(ears->file_fd);
        ears->file_fd = -1;
    }
    if (ears->file_pid > 0) {
        /* Explicit source close cancels the owned decoder, then reaps it. */
        kill(ears->file_pid, SIGKILL);
        while (waitpid(ears->file_pid, NULL, 0) < 0 && errno == EINTR) {}
        ears->file_pid = -1;
    }
    pthread_mutex_lock(&ears->event_lock);
    if (ears->open) {
        char details[768];
        snprintf(details, sizeof(details),
                 "source=%s; origin=%d; bytes_since_last_journal_entry=%llu",
                 ears->event.source_name[0] ? ears->event.source_name : "(unknown)",
                 (int)ears->event.origin,
                 (unsigned long long)ears->bytes_since_log);
        r2_log_sensory("audio_source_closed",
                       "R2 Ears stopped receiving audio samples.",
                       details, "Ears.c");
    }

    ears->file_pid = -1;
    ears->using_file = 0;
    ears->open = 0;
    ears->bytes_since_log = 0;
    ears->last_log_timestamp_us = 0;

    r2_ears_reset_event(ears);
    pthread_mutex_unlock(&ears->event_lock);
}


/* ---------------------------------------------------------
 * Shutdown
 * --------------------------------------------------------- */

void r2_ears_shutdown(R2Ears *ears)
{
    if (!ears)
        return;

    r2_ears_close(ears);

    pthread_mutex_destroy(&ears->event_lock);
    free(ears);
}


/* ---------------------------------------------------------
 * Open generic Pulse/PipeWire source
 * --------------------------------------------------------- */

/* A poll cycle is bounded only to observe caller cancellation. There is no
 * source, silence, clip-length, or inference deadline. */
static int r2_ears_pulse_step(R2Ears *ears)
{
    if (pa_mainloop_prepare(ears->pulse_loop, 100) < 0) return -1;
    if (pa_mainloop_poll(ears->pulse_loop) < 0) return -1;
    return pa_mainloop_dispatch(ears->pulse_loop);
}

int r2_ears_open_source_cancelable(
    R2Ears *ears, const char *source_name,
    int (*cancelled)(void *), void *userdata
)
{
    pa_sample_spec spec;
    pa_buffer_attr buffer_attr;
    int error = 0;
    char *resolved_source = NULL;
    const char *actual_source;

    if (!ears)
        return -1;
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }

    /*
     * NULL means:
     *
     *     "use the system's default recording source."
     *
     * Usually this is the microphone.
     */
    if (!source_name ||
        source_name[0] == '\0')
    {
        resolved_source =
            r2_ears_get_default_source();

        actual_source =
            resolved_source;
    }
    else
    {
        actual_source = source_name;
    }

    if (!actual_source ||
        actual_source[0] == '\0')
    {
        free(resolved_source);

        fprintf(
            stderr,
            "R2 Ears: no default audio source available.\n"
        );
        r2_log_sensory("audio_source_open_failed",
                       "R2 Ears could not resolve the requested audio source.",
                       "No explicit source was supplied and no default source was available.",
                       "Ears.c");
        return -1;
    }

    r2_ears_close(ears);

    spec =
        r2_ears_sample_spec();

    memset(
        &buffer_attr,
        0,
        sizeof(buffer_attr)
    );

    /*
     * Pulse/PipeWire can choose the actual internal buffer
     * layout, while fragsize gives R2 roughly 10 ms reads.
     */
    buffer_attr.maxlength =
        (uint32_t)-1;

    buffer_attr.fragsize =
        (uint32_t)R2_EARS_FRAME_BYTES;

    ears->pulse_loop = pa_mainloop_new();
    if (!ears->pulse_loop) goto pulse_failed;
    ears->pulse_context = pa_context_new(pa_mainloop_get_api(ears->pulse_loop), "R2-3PO");
    if (!ears->pulse_context ||
        pa_context_connect(ears->pulse_context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto pulse_failed;
    while (pa_context_get_state(ears->pulse_context) != PA_CONTEXT_READY) {
        if (cancelled && cancelled(userdata)) goto pulse_cancelled;
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(ears->pulse_context)) ||
            r2_ears_pulse_step(ears) < 0) goto pulse_failed;
    }
    ears->pulse_stream = pa_stream_new(ears->pulse_context, "R2 Hearing", &spec, NULL);
    if (!ears->pulse_stream ||
        pa_stream_connect_record(ears->pulse_stream, actual_source, &buffer_attr,
                                 PA_STREAM_NOFLAGS) < 0) goto pulse_failed;
    while (pa_stream_get_state(ears->pulse_stream) != PA_STREAM_READY) {
        if (cancelled && cancelled(userdata)) goto pulse_cancelled;
        if (!PA_STREAM_IS_GOOD(pa_stream_get_state(ears->pulse_stream)) ||
            !PA_CONTEXT_IS_GOOD(pa_context_get_state(ears->pulse_context)) ||
            r2_ears_pulse_step(ears) < 0) goto pulse_failed;
    }

    pthread_mutex_lock(&ears->event_lock);
    ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    ears->format.channels =
        R2_EARS_CHANNELS;

    ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    ears->event.origin =
        R2_HEARING_WORLD;

    ears->event.type =
        R2_AUDIO_UNKNOWN;

    ears->event.format =
        ears->format;

    ears->event.timestamp =
        r2_ears_timestamp();

    r2_ears_set_source_name(
        ears,
        actual_source
    );

    ears->using_file = 0;
    ears->open = 1;
    ears->bytes_since_log = 0;
    ears->last_log_timestamp_us = ears->event.timestamp;

    {
        char details[768];
        snprintf(details, sizeof(details),
                 "origin=world; sample_rate=%u; channels=%u; bits_per_sample=%u",
                 ears->format.sample_rate, ears->format.channels,
                 ears->format.bits_per_sample);
        r2_log_sensory("audio_source_opened",
                       "R2 Ears opened a live audio input.",
                       details, ears->event.source_name);
    }

    pthread_mutex_unlock(&ears->event_lock);
    free(resolved_source);
    return 0;
pulse_cancelled:
    r2_ears_close(ears);
    free(resolved_source);
    errno = ECANCELED;
    return -1;
pulse_failed:
    error = ears->pulse_context ? pa_context_errno(ears->pulse_context) : PA_ERR_INTERNAL;
    fprintf(stderr, "R2 Ears: could not open audio source '%s': %s\n",
            actual_source, pa_strerror(error));
    r2_log_sensory("audio_source_open_failed", "R2 Ears could not open the live audio source.",
                   pa_strerror(error), actual_source);
    r2_ears_close(ears);
    free(resolved_source);
    return -1;
}


int r2_ears_open_source(R2Ears *ears, const char *source_name)
{
    return r2_ears_open_source_cancelable(ears, source_name, NULL, NULL);
}

/* ---------------------------------------------------------
 * Open default microphone / recording source
 * --------------------------------------------------------- */

int r2_ears_open_microphone_cancelable(R2Ears *ears,
                                      int (*cancelled)(void *), void *userdata)
{
    if (!ears) return -1;
    char source[512];
    if (r2_ears_find_microphone(source, sizeof(source)) != 0)
        return -1;
    return r2_ears_open_source_cancelable(ears, source, cancelled, userdata);
}


int r2_ears_open_microphone(R2Ears *ears)
{
    return r2_ears_open_microphone_cancelable(ears, NULL, NULL);
}

/* ---------------------------------------------------------
 * Open desktop/system audio
 * --------------------------------------------------------- */

int r2_ears_open_desktop_cancelable(R2Ears *ears,
                                   int (*cancelled)(void *), void *userdata)
{
    char *sink;
    char monitor[1024];
    int result;

    if (!ears)
        return -1;

    sink =
        r2_ears_get_default_sink();

    if (!sink)
    {
        fprintf(
            stderr,
            "R2 Ears: could not determine the default "
            "desktop audio sink.\n"
        );

        return -1;
    }

    /*
     * The Pulse/PipeWire monitor source represents the audio
     * being sent through the sink.
     *
     * Therefore:
     *
     *     MP3 playing on desktop
     *     YouTube playing in browser
     *     game audio
     *     movie audio
     *     system sounds
     *
     * all appear here.
     */
    snprintf(
        monitor,
        sizeof(monitor),
        "%s.monitor",
        sink
    );

    free(sink);

    /*
     * Give a useful diagnostic before attempting to open it.
     */
    if (!r2_ears_source_exists(monitor))
    {
        /*
         * Do NOT immediately fail.
         *
         * PipeWire/Pulse implementations can expose sources in
         * ways that make pactl's listing differ from what
         * libpulse can actually open.
         *
         * Let the native Pulse stream make the final determination.
         */
    }

    result =
        r2_ears_open_source_cancelable(ears, monitor, cancelled, userdata);

    if (result != 0)
        return result;

    pthread_mutex_lock(&ears->event_lock);
    ears->event.origin =
        R2_HEARING_WORLD;

    r2_ears_set_source_name(
        ears,
        "desktop-audio"
    );

    pthread_mutex_unlock(&ears->event_lock);
    return 0;
}


int r2_ears_open_desktop(R2Ears *ears)
{
    return r2_ears_open_desktop_cancelable(ears, NULL, NULL);
}

/* ---------------------------------------------------------
 * Open arbitrary audio file
 * --------------------------------------------------------- */

int r2_ears_open_file(
    R2Ears *ears,
    const char *path
)
{
    if (!ears ||
        !path ||
        path[0] == '\0')
    {
        return -1;
    }

    r2_ears_close(ears);

    if (r2_ears_start_ffmpeg(
            ears,
            path
        ) != 0)
    {
        fprintf(
            stderr,
            "R2 Ears: could not open audio file: %s\n",
            path
        );
        r2_log_sensory("audio_file_open_failed",
                       "R2 Ears could not decode the requested audio file.",
                       "FFmpeg did not provide a readable PCM stream.",
                       path);
        return -1;
    }

    pthread_mutex_lock(&ears->event_lock);
    ears->format.sample_rate =
        R2_EARS_SAMPLE_RATE;

    ears->format.channels =
        R2_EARS_CHANNELS;

    ears->format.bits_per_sample =
        R2_EARS_BITS_PER_SAMPLE;

    /*
     * R2 knows that this audio originated as a file.
     *
     * It does NOT know what the audio means.
     */
    ears->event.origin =
        R2_HEARING_FILE;

    ears->event.type =
        R2_AUDIO_UNKNOWN;

    ears->event.format =
        ears->format;

    ears->event.timestamp =
        r2_ears_timestamp();

    r2_ears_set_source_name(
        ears,
        path
    );

    ears->using_file = 1;
    ears->open = 1;
    ears->bytes_since_log = 0;
    ears->last_log_timestamp_us = ears->event.timestamp;

    {
        char details[768];
        snprintf(details, sizeof(details),
                 "origin=file; sample_rate=%u; channels=%u; bits_per_sample=%u; "
                 "audio_meaning=not_interpreted_by_Ears",
                 ears->format.sample_rate, ears->format.channels,
                 ears->format.bits_per_sample);
        r2_log_sensory("audio_source_opened",
                       "R2 Ears opened an audio file for PCM input.",
                       details, path);
    }

    pthread_mutex_unlock(&ears->event_lock);
    return 0;
}


/* ---------------------------------------------------------
 * Read PCM audio
 * --------------------------------------------------------- */

static void r2_ears_record_samples(R2Ears *ears, size_t bytes)
{
    pthread_mutex_lock(&ears->event_lock);
    ears->event.timestamp = r2_ears_timestamp();
    ears->bytes_since_log += bytes;
    if (ears->last_log_timestamp_us == 0 ||
        ears->event.timestamp - ears->last_log_timestamp_us >= 5000000ULL) {
        char details[768];
        snprintf(details, sizeof(details),
                 "source=%s; bytes_received_since_last_entry=%llu; "
                 "sample_rate=%u; channels=%u; bits_per_sample=%u; "
                 "audio_interpretation=not_performed_by_Ears",
                 ears->event.source_name, (unsigned long long)ears->bytes_since_log,
                 ears->format.sample_rate, ears->format.channels, ears->format.bits_per_sample);
        r2_log_sensory("audio_samples_received", "R2 Ears received real PCM audio samples.",
                       details, ears->event.source_name);
        ears->bytes_since_log = 0;
        ears->last_log_timestamp_us = ears->event.timestamp;
    }
    pthread_mutex_unlock(&ears->event_lock);
}

ssize_t r2_ears_read_cancelable(R2Ears *ears, void *buffer, size_t buffer_size,
                               int (*cancelled)(void *), void *userdata)
{
    if (!ears || !buffer || !buffer_size || buffer_size > SSIZE_MAX) {
        errno = EINVAL; return -1;
    }
    if (!r2_ears_is_open(ears)) { errno = EBADF; return -1; }
    size_t copied = 0;
    while (copied < buffer_size) {
        if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
        if (ears->using_file) {
            struct pollfd fd = { .fd = ears->file_fd, .events = POLLIN };
            int ready = poll(&fd, 1, 100);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) goto read_failed;
            if (!ready) continue;
            ssize_t got = read(ears->file_fd, (unsigned char *)buffer + copied,
                               buffer_size - copied);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) goto read_failed;
            if (got == 0) {
                int status = 0;
                for (;;) {
                    pid_t done = waitpid(ears->file_pid, &status, WNOHANG);
                    if (done == ears->file_pid) { ears->file_pid = -1; break; }
                    if (done < 0 && errno == EINTR) continue;
                    if (done < 0) goto read_failed;
                    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
                    (void)poll(NULL, 0, 100);
                }
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    errno = EIO; goto read_failed;
                }
                if (copied) r2_ears_record_samples(ears, copied);
                r2_ears_close(ears);
                return (ssize_t)copied;
            }
            copied += (size_t)got;
        } else {
            if (!ears->pulse_stream ||
                pa_stream_get_state(ears->pulse_stream) != PA_STREAM_READY ||
                pa_context_get_state(ears->pulse_context) != PA_CONTEXT_READY) {
                errno = EIO; goto read_failed;
            }
            if (!ears->pulse_remaining) {
                const void *data = NULL;
                size_t bytes = 0;
                if (pa_stream_peek(ears->pulse_stream, &data, &bytes) < 0) {
                    errno = EIO; goto read_failed;
                }
                if (!bytes) {
                    if (r2_ears_pulse_step(ears) < 0) { errno = EIO; goto read_failed; }
                    continue;
                }
                /* A Pulse hole is lost capture data, not observed silence. */
                if (!data) { errno = EPIPE; goto read_failed; }
                ears->pulse_data = data;
                ears->pulse_remaining = bytes;
            }
            size_t count = buffer_size - copied;
            if (count > ears->pulse_remaining) count = ears->pulse_remaining;
            memcpy((unsigned char *)buffer + copied, ears->pulse_data, count);
            ears->pulse_data += count;
            ears->pulse_remaining -= count;
            copied += count;
            if (!ears->pulse_remaining && pa_stream_drop(ears->pulse_stream) < 0) {
                errno = EIO; goto read_failed;
            }
        }
    }
    r2_ears_record_samples(ears, copied);
    return (ssize_t)copied;
read_failed: {
    int saved = errno;
    R2HearingEvent event;
    r2_ears_get_event(ears, &event);
    r2_log_sensory("audio_read_failed", "R2 Ears could not complete a real PCM read.",
                   strerror(saved), event.source_name);
    r2_ears_close(ears);
    errno = saved;
    return -1;
}
}

ssize_t r2_ears_read(R2Ears *ears, void *buffer, size_t buffer_size)
{
    return r2_ears_read_cancelable(ears, buffer, buffer_size, NULL, NULL);
}


/* ---------------------------------------------------------
 * Event information
 * --------------------------------------------------------- */

int r2_ears_get_event(
    R2Ears *ears,
    R2HearingEvent *event
)
{
    if (!ears || !event)
        return -1;

    pthread_mutex_lock(&ears->event_lock);
    memcpy(
        event,
        &ears->event,
        sizeof(R2HearingEvent)
    );
    pthread_mutex_unlock(&ears->event_lock);

    return 0;
}


/* ---------------------------------------------------------
 * Audio format
 * --------------------------------------------------------- */

int r2_ears_get_format(
    R2Ears *ears,
    R2AudioFormat *format
)
{
    if (!ears || !format)
        return -1;

    pthread_mutex_lock(&ears->event_lock);
    memcpy(
        format,
        &ears->format,
        sizeof(R2AudioFormat)
    );
    pthread_mutex_unlock(&ears->event_lock);

    return 0;
}


/* ---------------------------------------------------------
 * State
 * --------------------------------------------------------- */

int r2_ears_is_open(R2Ears *ears)
{
    if (!ears)
        return 0;

    pthread_mutex_lock(&ears->event_lock);
    int open = ears->open;
    pthread_mutex_unlock(&ears->event_lock);
    return open;
}


R2HearingOrigin r2_ears_get_origin(R2Ears *ears)
{
    if (!ears)
        return R2_HEARING_NONE;

    pthread_mutex_lock(&ears->event_lock);
    R2HearingOrigin origin = ears->event.origin;
    pthread_mutex_unlock(&ears->event_lock);
    return origin;
}
