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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/simple.h>


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
    pa_simple *pulse_stream;

    /*
     * FFmpeg process for file/media decoding.
     *
     * FFmpeg outputs canonical raw PCM through this pipe.
     */
    FILE *file_stream;

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


static int r2_ears_set_source_name(R2Ears *ears, const char *name)
{
    if (!ears) return -1;
    char *copy = strdup(name ? name : "");
    if (!copy) return -1;
    free(ears->event.source_name);
    ears->event.source_name = copy;
    return 0;
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
static char *r2_ears_read_command_line(const char *command)
{
    FILE *pipe = popen(command, "r");
    if (!pipe) return NULL;
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&line, &capacity, pipe);
    int status = pclose(pipe);
    if (length < 0 || status != 0) { free(line); return NULL; }
    line[strcspn(line, "\r\n")] = '\0';
    if (!*line) { free(line); return NULL; }
    return line;
}

static char *r2_ears_get_default_sink(void)
{
    return r2_ears_read_command_line("pactl get-default-sink 2>/dev/null");
}

static char *r2_ears_get_default_source(void)
{
    return r2_ears_read_command_line("pactl get-default-source 2>/dev/null");
}


/* Let pa_simple_new determine actual source availability without substring grep. */

/* Availability discovery is separate from sound capture. Both USB and built-in
 * microphone names come from the active Pulse/PipeWire server, not an app list.
 * A monitor is desktop playback, not a physical microphone. */
static int r2_ears_is_monitor(const char *name)
{
    if (!name) return 1;
    size_t n = strlen(name);
    return n >= 8 && strcmp(name + n - 8, ".monitor") == 0;
}

static char *r2_ears_find_microphone(void)
{
    char *default_name = r2_ears_get_default_source();
    FILE *pipe = popen("pactl list short sources 2>/dev/null", "r");
    if (!pipe) { free(default_name); return NULL; }
    char *line = NULL, *fallback = NULL, *preferred = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, pipe) >= 0) {
        char *cursor = line;
        while (*cursor >= '0' && *cursor <= '9') ++cursor;
        if (cursor == line || (*cursor != ' ' && *cursor != '\t')) continue;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        char *end = cursor + strcspn(cursor, " \t\r\n");
        if (end == cursor) continue;
        *end = '\0';
        if (r2_ears_is_monitor(cursor)) continue;
        if (!fallback) fallback = strdup(cursor);
        if (default_name && strcmp(cursor, default_name) == 0) {
            preferred = strdup(cursor);
            break;
        }
    }
    free(line);
    int status = pclose(pipe);
    free(default_name);
    if (status != 0) { free(preferred); free(fallback); return NULL; }
    if (preferred) { free(fallback); return preferred; }
    return fallback;
}

int r2_ears_microphone_available(void)
{
    char *name = r2_ears_find_microphone();
    int available = name != NULL;
    free(name);
    return available;
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

static int r2_ears_start_ffmpeg(R2Ears *ears, const char *path)
{
    if (!ears || !path || !*path) return -1;
    char *quoted_path = r2_ears_shell_quote(path);
    if (!quoted_path) return -1;
    char *command = NULL;
    int written = asprintf(&command,
        "ffmpeg -nostdin -hide_banner -loglevel error "
        "-i %s -vn -f s16le -ar %u -ac %u -acodec pcm_s16le pipe:1",
        quoted_path, R2_EARS_SAMPLE_RATE, R2_EARS_CHANNELS);
    free(quoted_path);
    if (written < 0 || !command) return -1;
    ears->file_stream = popen(command, "r");
    free(command);
    if (!ears->file_stream) return -1;
    ears->file_pid = -1;
    return 0;
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
    new_ears->file_stream = NULL;
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

    if (ears->pulse_stream)
    {
        pa_simple_free(
            ears->pulse_stream
        );

        ears->pulse_stream = NULL;
    }

    if (ears->file_stream)
    {
        pclose(
            ears->file_stream
        );

        ears->file_stream = NULL;
    }

    if (ears->open) {
        char details[768];
        snprintf(details, sizeof(details),
                 "origin=%d; bytes_since_last_journal_entry=%llu",
                 (int)ears->event.origin,
                 (unsigned long long)ears->bytes_since_log);
        r2_log_sensory("audio_source_closed",
                       "R2 Ears stopped receiving audio samples.",
                       details, ears->event.source_name ? ears->event.source_name : "Ears.c");
    }

    free(ears->event.source_name);
    ears->event.source_name = NULL;
    ears->file_pid = -1;
    ears->using_file = 0;
    ears->open = 0;
    ears->bytes_since_log = 0;
    ears->last_log_timestamp_us = 0;

    r2_ears_reset_event(ears);
}


/* ---------------------------------------------------------
 * Shutdown
 * --------------------------------------------------------- */

void r2_ears_shutdown(R2Ears *ears)
{
    if (!ears)
        return;

    r2_ears_close(ears);

    free(ears);
}


/* ---------------------------------------------------------
 * Open generic Pulse/PipeWire source
 * --------------------------------------------------------- */

int r2_ears_open_source(
    R2Ears *ears,
    const char *source_name
)
{
    pa_sample_spec spec;
    pa_buffer_attr buffer_attr;
    int error = 0;
    char *resolved_source = NULL;
    const char *actual_source;

    if (!ears)
        return -1;

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

    ears->pulse_stream =
        pa_simple_new(
            NULL,
            "R2-3PO",
            PA_STREAM_RECORD,
            actual_source,
            "R2 Hearing",
            &spec,
            NULL,
            &buffer_attr,
            &error
        );

    if (!ears->pulse_stream)
    {
        fprintf(
            stderr,
            "R2 Ears: could not open audio source '%s': %s\n",
            actual_source,
            pa_strerror(error)
        );

        r2_log_sensory("audio_source_open_failed",
                       "R2 Ears could not open the live audio source.",
                       pa_strerror(error), actual_source);
        free(resolved_source);

        return -1;
    }

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

    if (r2_ears_set_source_name(ears, actual_source) != 0) {
        free(resolved_source);
        r2_ears_close(ears);
        return -1;
    }

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

    free(resolved_source);

    return 0;
}


/* ---------------------------------------------------------
 * Open default microphone / recording source
 * --------------------------------------------------------- */

int r2_ears_open_microphone(R2Ears *ears)
{
    if (!ears) return -1;
    char *source = r2_ears_find_microphone();
    if (!source) return -1;
    int result = r2_ears_open_source(ears, source);
    free(source);
    return result;
}


/* ---------------------------------------------------------
 * Open desktop/system audio
 * --------------------------------------------------------- */

int r2_ears_open_desktop(R2Ears *ears)
{
    if (!ears) return -1;
    char *sink = r2_ears_get_default_sink();
    if (!sink) {
        fprintf(stderr, "R2 Ears: could not determine the default desktop sink.\n");
        return -1;
    }
    char *monitor = NULL;
    int length = asprintf(&monitor, "%s.monitor", sink);
    free(sink);
    if (length < 0 || !monitor) return -1;
    int result = r2_ears_open_source(ears, monitor);
    free(monitor);
    return result;
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

    if (r2_ears_set_source_name(ears, path) != 0) {
        r2_ears_close(ears);
        return -1;
    }

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

    return 0;
}


/* ---------------------------------------------------------
 * Read PCM audio
 * --------------------------------------------------------- */

ssize_t r2_ears_read(
    R2Ears *ears,
    void *buffer,
    size_t buffer_size
)
{
    if (!ears ||
        !buffer ||
        buffer_size == 0)
    {
        errno = EINVAL;
        return -1;
    }

    if (!ears->open)
    {
        errno = EBADF;
        return -1;
    }


    /* -----------------------------------------------------
     * File source
     * ----------------------------------------------------- */

    if (ears->using_file)
    {
        size_t bytes_read;

        if (!ears->file_stream)
        {
            errno = EBADF;
            return -1;
        }

        bytes_read =
            fread(
                buffer,
                1,
                buffer_size,
                ears->file_stream
            );

        if (bytes_read > 0)
        {
            ears->event.timestamp =
                r2_ears_timestamp();
            ears->bytes_since_log += bytes_read;

            if (ears->last_log_timestamp_us == 0 ||
                ears->event.timestamp - ears->last_log_timestamp_us >= 5000000ULL) {
                char details[768];
                snprintf(details, sizeof(details),
                         "bytes_received_since_last_entry=%llu; "
                         "sample_rate=%u; channels=%u; bits_per_sample=%u; "
                         "audio_interpretation=not_performed_by_Ears",
                         (unsigned long long)ears->bytes_since_log,
                         ears->format.sample_rate, ears->format.channels,
                         ears->format.bits_per_sample);
                r2_log_sensory("audio_samples_received",
                               "R2 Ears received real PCM audio samples.",
                               details, ears->event.source_name);
                ears->bytes_since_log = 0;
                ears->last_log_timestamp_us = ears->event.timestamp;
            }

            return (ssize_t)bytes_read;
        }

        if (feof(ears->file_stream))
        {
            r2_ears_close(ears);

            return 0;
        }

        if (ferror(ears->file_stream))
        {
            r2_log_sensory("audio_read_failed",
                           "R2 Ears encountered an error reading audio-file samples.",
                           "The decoded PCM stream reported a read error.",
                           ears->event.source_name);
            r2_ears_close(ears);

            return -1;
        }

        return 0;
    }


    /* -----------------------------------------------------
     * PulseAudio / PipeWire source
     * ----------------------------------------------------- */

    if (ears->pulse_stream)
    {
        int error = 0;

        if (pa_simple_read(
                ears->pulse_stream,
                buffer,
                buffer_size,
                &error
            ) < 0)
        {
            fprintf(
                stderr,
                "R2 Ears: audio read failed: %s\n",
                pa_strerror(error)
            );
            r2_log_sensory("audio_read_failed",
                           "R2 Ears encountered a live audio capture error.",
                           pa_strerror(error), ears->event.source_name);
            /* A failed Pulse/PipeWire read invalidates this stream. Close it
               now so callers cannot accidentally keep treating it as usable. */
            r2_ears_close(ears);
            return -1;
        }

        ears->event.timestamp =
            r2_ears_timestamp();
        ears->bytes_since_log += buffer_size;

        if (ears->last_log_timestamp_us == 0 ||
            ears->event.timestamp - ears->last_log_timestamp_us >= 5000000ULL) {
            char details[768];
            snprintf(details, sizeof(details),
                     "bytes_received_since_last_entry=%llu; "
                     "sample_rate=%u; channels=%u; bits_per_sample=%u; "
                     "audio_interpretation=not_performed_by_Ears",
                     (unsigned long long)ears->bytes_since_log,
                     ears->format.sample_rate, ears->format.channels,
                     ears->format.bits_per_sample);
            r2_log_sensory("audio_samples_received",
                           "R2 Ears received real PCM audio samples.",
                           details, ears->event.source_name);
            ears->bytes_since_log = 0;
            ears->last_log_timestamp_us = ears->event.timestamp;
        }

        return (ssize_t)buffer_size;
    }


    errno = EBADF;

    return -1;
}


/* ---------------------------------------------------------
 * Event information
 * --------------------------------------------------------- */

int r2_ears_get_event(R2Ears *ears, R2HearingEvent *event)
{
    if (!ears || !event) return -1;
    char *source = strdup(ears->event.source_name ? ears->event.source_name : "");
    if (!source) return -1;
    *event = ears->event;
    event->source_name = source;
    return 0;
}

void r2_ears_release_event(R2HearingEvent *event)
{
    if (!event) return;
    free(event->source_name);
    event->source_name = NULL;
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

    memcpy(
        format,
        &ears->format,
        sizeof(R2AudioFormat)
    );

    return 0;
}


/* ---------------------------------------------------------
 * State
 * --------------------------------------------------------- */

int r2_ears_is_open(R2Ears *ears)
{
    if (!ears)
        return 0;

    return ears->open;
}


R2HearingOrigin r2_ears_get_origin(R2Ears *ears)
{
    if (!ears)
        return R2_HEARING_NONE;

    return ears->event.origin;
}
