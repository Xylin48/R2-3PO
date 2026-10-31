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


/* Discovery owns a native connection but never opens a recording stream. */
typedef struct {
    R2EarsDiscovery result;
    char *default_source, *default_sink;
    uint32_t playback_sink;
    int done, failed;
} R2EarsDiscoveryQuery;

static int r2_ears_pulse_step(R2Ears *ears);

void r2_ears_discovery_clear(R2EarsDiscovery *result)
{
    if (!result) return;
    free(result->microphone_source);
    free(result->desktop_monitor);
    memset(result, 0, sizeof(*result));
    result->playback_pid = -1;
}

static void r2_ears_discovery_copy(R2EarsDiscoveryQuery *q, char **out, const char *value)
{
    if (!value || !*value) return;
    char *copy = strdup(value);
    if (!copy) { q->failed = ENOMEM; return; }
    free(*out);
    *out = copy;
}

static void r2_ears_server_info(pa_context *context, const pa_server_info *info, void *userdata)
{
    (void)context;
    R2EarsDiscoveryQuery *q = userdata;
    q->done = 1;
    if (!info) { q->failed = EIO; return; }
    r2_ears_discovery_copy(q, &q->default_source, info->default_source_name);
    r2_ears_discovery_copy(q, &q->default_sink, info->default_sink_name);
}

static void r2_ears_source_info(pa_context *context, const pa_source_info *info,
                                int eol, void *userdata)
{
    (void)context;
    R2EarsDiscoveryQuery *q = userdata;
    if (eol) { q->done = 1; if (eol < 0) q->failed = EIO; return; }
    if (!info || !info->name || info->monitor_of_sink != PA_INVALID_INDEX) return;
    if (!q->result.microphone_source ||
        (q->default_source && !strcmp(info->name, q->default_source)))
        r2_ears_discovery_copy(q, &q->result.microphone_source, info->name);
}

static void r2_ears_sink_input_info(pa_context *context, const pa_sink_input_info *info,
                                    int eol, void *userdata)
{
    (void)context;
    R2EarsDiscoveryQuery *q = userdata;
    if (eol) { q->done = 1; if (eol < 0) q->failed = EIO; return; }
    if (!info || info->corked || info->sink == PA_INVALID_INDEX) return;
    const char *pid_text = info->proplist ?
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID) : NULL;
    long pid = -1;
    if (pid_text && *pid_text && strspn(pid_text, "0123456789") == strlen(pid_text)) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(pid_text, &end, 10);
        if (!errno && end && !*end && parsed > 1 && parsed <= INT_MAX) pid = parsed;
    }
    /* Select one actual output and its producer together. Prefer a producer
       identity when the first active stream did not supply one. */
    if (!q->result.desktop_playing || (q->result.playback_pid < 0 && pid > 1)) {
        q->playback_sink = info->sink;
        q->result.playback_pid = pid;
    }
    q->result.desktop_playing = 1;
}

static void r2_ears_sink_info(pa_context *context, const pa_sink_info *info,
                              int eol, void *userdata)
{
    (void)context;
    R2EarsDiscoveryQuery *q = userdata;
    if (eol) { q->done = 1; if (eol < 0) q->failed = EIO; return; }
    if (!info) return;
    int selected = q->result.desktop_playing ? info->index == q->playback_sink :
        (q->default_sink && info->name && !strcmp(info->name, q->default_sink));
    if (selected)
        r2_ears_discovery_copy(q, &q->result.desktop_monitor, info->monitor_source_name);
}

static int r2_ears_discovery_wait(R2Ears *connection, pa_operation *operation,
                                  R2EarsDiscoveryQuery *q,
                                  int (*cancelled)(void *), void *userdata)
{
    if (!operation) { errno = EIO; return -1; }
    int error = 0;
    for (;;) {
        if (cancelled && cancelled(userdata)) { error = ECANCELED; break; }
        if (q->failed) { error = q->failed; break; }
        pa_operation_state_t state = pa_operation_get_state(operation);
        if (state != PA_OPERATION_RUNNING) {
            if (state != PA_OPERATION_DONE || !q->done) error = EIO;
            break;
        }
        if (pa_context_get_state(connection->pulse_context) != PA_CONTEXT_READY ||
            r2_ears_pulse_step(connection) < 0) { error = EIO; break; }
    }
    if (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        pa_operation_cancel(operation);
    pa_operation_unref(operation);
    if (error) { errno = error; return -1; }
    return 0;
}

int r2_ears_discover(R2EarsDiscovery *out, int (*cancelled)(void *), void *userdata)
{
    if (!out) { errno = EINVAL; return -1; }
    /* Caller supplies an empty result and owns its strings after success. */
    memset(out, 0, sizeof(*out)); out->playback_pid = -1;
    R2EarsDiscoveryQuery q = { .result.playback_pid = -1,
                               .playback_sink = PA_INVALID_INDEX };
    R2Ears *connection = NULL;
    int rc = -1, error = EIO;
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
    if (r2_ears_init(&connection) != 0) { errno = ENOMEM; return -1; }
    connection->pulse_loop = pa_mainloop_new();
    if (!connection->pulse_loop) { error = ENOMEM; goto cleanup; }
    connection->pulse_context = pa_context_new(pa_mainloop_get_api(connection->pulse_loop),
                                               "R2 source discovery");
    if (!connection->pulse_context) { error = ENOMEM; goto cleanup; }
    if (pa_context_connect(connection->pulse_context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto cleanup;
    while (pa_context_get_state(connection->pulse_context) != PA_CONTEXT_READY) {
        if (cancelled && cancelled(userdata)) { error = ECANCELED; goto cleanup; }
        if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(connection->pulse_context)) ||
            r2_ears_pulse_step(connection) < 0) goto cleanup;
    }
    for (int query = 0; query < 4; ++query) {
        pa_operation *operation = NULL;
        q.done = 0;
        if (query == 0) operation = pa_context_get_server_info(connection->pulse_context, r2_ears_server_info, &q);
        if (query == 1) operation = pa_context_get_source_info_list(connection->pulse_context, r2_ears_source_info, &q);
        if (query == 2) operation = pa_context_get_sink_input_info_list(connection->pulse_context, r2_ears_sink_input_info, &q);
        if (query == 3) operation = pa_context_get_sink_info_list(connection->pulse_context, r2_ears_sink_info, &q);
        if (r2_ears_discovery_wait(connection, operation, &q, cancelled, userdata) != 0) {
            error = errno; goto cleanup;
        }
    }
    /* A routed sink disappearing during enumeration is an incomplete query,
       not evidence that there was no playback. Retry on the next Watch scan. */
    if (q.result.desktop_playing && !q.result.desktop_monitor) goto cleanup;
    *out = q.result;
    memset(&q.result, 0, sizeof(q.result));
    rc = 0;
cleanup:
    free(q.default_source); free(q.default_sink);
    r2_ears_discovery_clear(&q.result);
    r2_ears_shutdown(connection);
    if (rc != 0) errno = error;
    return rc;
}

int r2_ears_microphone_available(void)
{
    R2EarsDiscovery result = {0};
    if (r2_ears_discover(&result, NULL, NULL) != 0) return 0;
    int available = result.microphone_source != NULL;
    r2_ears_discovery_clear(&result);
    return available;
}

int r2_ears_desktop_playing(void)
{
    R2EarsDiscovery result = {0};
    if (r2_ears_discover(&result, NULL, NULL) != 0) return 0;
    int playing = result.desktop_playing;
    r2_ears_discovery_clear(&result);
    return playing;
}

long r2_ears_playback_pid(void)
{
    R2EarsDiscovery result = {0};
    if (r2_ears_discover(&result, NULL, NULL) != 0) return -1;
    long pid = result.playback_pid;
    r2_ears_discovery_clear(&result);
    return pid;
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
    const char *actual_source = source_name && *source_name ? source_name : NULL;
    if (!ears) return -1;
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }

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
        pa_stream_get_device_name(ears->pulse_stream)
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
    return 0;
pulse_cancelled:
    r2_ears_close(ears);
    errno = ECANCELED;
    return -1;
pulse_failed:
    error = ears->pulse_context ? pa_context_errno(ears->pulse_context) : PA_ERR_INTERNAL;
    fprintf(stderr, "R2 Ears: could not open audio source '%s': %s\n",
            actual_source ? actual_source : "default source", pa_strerror(error));
    r2_log_sensory("audio_source_open_failed", "R2 Ears could not open the live audio source.",
                   pa_strerror(error), actual_source);
    r2_ears_close(ears);
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
    R2EarsDiscovery result = {0};
    if (r2_ears_discover(&result, cancelled, userdata) != 0) return -1;
    int rc = -1;
    if (result.microphone_source)
        rc = r2_ears_open_source_cancelable(ears, result.microphone_source, cancelled, userdata);
    else errno = ENODEV;
    int error = errno;
    r2_ears_discovery_clear(&result);
    errno = error;
    return rc;
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
    if (!ears) return -1;
    R2EarsDiscovery result = {0};
    if (r2_ears_discover(&result, cancelled, userdata) != 0) return -1;
    int rc = -1;
    if (result.desktop_monitor)
        rc = r2_ears_open_source_cancelable(ears, result.desktop_monitor, cancelled, userdata);
    else errno = ENODEV;
    int error = errno;
    r2_ears_discovery_clear(&result);
    if (rc == 0) {
        pthread_mutex_lock(&ears->event_lock);
        r2_ears_set_source_name(ears, "desktop-audio");
        pthread_mutex_unlock(&ears->event_lock);
    }
    errno = error;
    return rc;
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
