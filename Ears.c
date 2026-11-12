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
#include "AudioDiscovery.h"
#include "AudioCapture.h"
#include "AudioFile.h"
#include "Log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <pulse/error.h>
#include <pulse/sample.h>


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

    atomic_int open; /* read failures may be signaled from a worker thread */

    /*
     * PulseAudio/PipeWire live capture.
     *
     * PipeWire normally exposes its PulseAudio compatibility
     * layer through libpulse, so this works for both systems.
     */
    R2AudioCapture *pulse_stream;
    /* Serializes publication, cancellation and release of the transport.
     * The transport's own Pulse mainloop mutex never blocks on PCM delivery. */
    pthread_mutex_t transport_lock;
    atomic_int cancel_requested;

    /*
     * FFmpeg process for file/media decoding.
     *
     * FFmpeg outputs canonical raw PCM through this pipe.
     */
    R2AudioFile *file_stream;

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
 * Native read-only PulseAudio/PipeWire enumeration is shared with Watch.
 * Query errors are distinct from a valid snapshot with zero microphones.
 */
static char *r2_ears_find_microphone(void)
{
    R2AudioDiscovery sources;
    if (r2_audio_discover(&sources) != 0) return NULL;
    char *selected = sources.microphone_source;
    sources.microphone_source = NULL;
    r2_audio_discovery_free(&sources);
    return selected;
}

int r2_ears_microphone_available(void)
{
    char *selected = r2_ears_find_microphone();
    int available = selected != NULL;
    free(selected);
    return available;
}

int r2_ears_desktop_playing(void)
{
    R2AudioDiscovery sources;
    if (r2_audio_discover(&sources) != 0) return 0;
    int playing = sources.desktop_playing;
    r2_audio_discovery_free(&sources);
    return playing;
}

long r2_ears_playback_pid(void)
{
    R2AudioDiscovery sources;
    if (r2_audio_discover(&sources) != 0) return -1;
    long pid = sources.playback_pid;
    r2_audio_discovery_free(&sources);
    return pid;
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

    atomic_init(&new_ears->open, 0);
    atomic_init(&new_ears->cancel_requested, 0);
    if (pthread_mutex_init(&new_ears->transport_lock, NULL) != 0) {
        free(new_ears);
        return -1;
    }
    new_ears->pulse_stream = NULL;
    new_ears->file_stream = NULL;
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

    pthread_mutex_lock(&ears->transport_lock);
    if (ears->pulse_stream) {
        /* Only the lifecycle owner may close, after its reader has exited. */
        r2_audio_capture_free(ears->pulse_stream);
        ears->pulse_stream = NULL;
    }
    pthread_mutex_unlock(&ears->transport_lock);

    pthread_mutex_lock(&ears->transport_lock);
    if (ears->file_stream) {
        /* The owner only closes after its reader has completed. */
        r2_audio_file_close(ears->file_stream);
        ears->file_stream = NULL;
    }
    pthread_mutex_unlock(&ears->transport_lock);

    /* A reader can mark the source inactive at EOF/failure, but only the
     * lifecycle owner closes it. Preserve the final close journal event
     * regardless of the availability flag; a source name means a session
     * was actually opened. */
    if (ears->event.source_name) {
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
    ears->using_file = 0;
    ears->open = 0;
    ears->bytes_since_log = 0;
    ears->last_log_timestamp_us = 0;

    r2_ears_reset_event(ears);
}


/* Interrupt both an asynchronous connect and a read waiting for PCM.
 * Safe before taking R2's capture lifecycle mutex, so a one-shot listen
 * cannot hide this cancellation behind that mutex. */
void r2_ears_cancel(R2Ears *ears)
{
    if (!ears) return;
    atomic_store(&ears->cancel_requested, 1);
    pthread_mutex_lock(&ears->transport_lock);
    if (ears->pulse_stream)
        r2_audio_capture_cancel(ears->pulse_stream);
    if (ears->file_stream)
        r2_audio_file_cancel(ears->file_stream);
    pthread_mutex_unlock(&ears->transport_lock);
}

/* ---------------------------------------------------------
 * Shutdown
 * --------------------------------------------------------- */

void r2_ears_shutdown(R2Ears *ears)
{
    if (!ears)
        return;

    r2_ears_close(ears);
    pthread_mutex_destroy(&ears->transport_lock);
    free(ears);
}


/* ---------------------------------------------------------
 * Open generic Pulse/PipeWire source
 * --------------------------------------------------------- */

int r2_ears_open_source_cancelable(
    R2Ears *ears,
    const char *source_name,
    int (*cancelled)(void *), void *userdata
)
{
    pa_sample_spec spec;
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
        R2AudioDiscovery sources;
        if (r2_audio_discover_cancelable(&sources, cancelled, userdata) != 0)
            return -1;
        resolved_source = sources.microphone_source;
        sources.microphone_source = NULL;
        r2_audio_discovery_free(&sources);

        actual_source =
            resolved_source;
    }
    else
    {
        /* A caller may pass ears->event.source_name. That string belongs
         * to the source we are about to close; make a stable owned copy
         * BEFORE freeing its event metadata. */
        resolved_source = strdup(source_name);
        if (!resolved_source) return -1;
        actual_source = resolved_source;
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

    atomic_store(&ears->cancel_requested, 0);
    r2_ears_close(ears);

    spec = r2_ears_sample_spec();
    R2AudioCapture *capture = r2_audio_capture_new();
    if (!capture) {
        free(resolved_source);
        return -1;
    }
    /* Publish before attempting the connection: stop requests can now cancel
     * even while the server is connecting, not just after the first sample. */
    pthread_mutex_lock(&ears->transport_lock);
    ears->pulse_stream = capture;
    /* Check the owner's persistent stop predicate after resetting this
     * reusable receiver. A stop before open must not be erased by reset. */
    if (atomic_load(&ears->cancel_requested) ||
        (cancelled && cancelled(userdata)))
        r2_audio_capture_cancel(capture);
    pthread_mutex_unlock(&ears->transport_lock);

    int connected = r2_audio_capture_connect(capture, actual_source, &spec, R2_EARS_FRAME_BYTES);
    int connect_error = connected != 0 ? errno : 0;
    if (connected != 0 || atomic_load(&ears->cancel_requested)) {
        int failed_errno = atomic_load(&ears->cancel_requested) ? ECANCELED :
                           connect_error ? connect_error : EIO;
        const char *details = failed_errno == ECANCELED ? "Audio connection was cancelled." :
                              failed_errno == ENODEV ? "The server did not retain the requested recording source." :
                              "PulseAudio/PipeWire connection failed.";
        r2_log_sensory("audio_source_open_failed",
                       "R2 Ears could not open the live audio source.",
                       details, actual_source);
        r2_ears_close(ears);
        free(resolved_source);
        errno = failed_errno;
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

int r2_ears_open_source(R2Ears *ears, const char *source_name)
{
    return r2_ears_open_source_cancelable(ears, source_name, NULL, NULL);
}

/* ---------------------------------------------------------
 * Open default microphone / recording source
 * --------------------------------------------------------- */

int r2_ears_open_microphone_source(R2Ears *ears, const char *source)
{
    if (!ears || !source || !*source) return -1;
    return r2_ears_open_source(ears, source);
}

int r2_ears_open_microphone(R2Ears *ears)
{
    if (!ears) return -1;
    char *source = r2_ears_find_microphone();
    if (!source) return -1;
    int result = r2_ears_open_microphone_source(ears, source);
    free(source);
    return result;
}


/* ---------------------------------------------------------
 * Open desktop/system audio
 * --------------------------------------------------------- */

int r2_ears_open_desktop_source(R2Ears *ears, const char *monitor)
{
    if (!ears || !monitor || !*monitor) return -1;
    return r2_ears_open_source(ears, monitor);
}

int r2_ears_open_desktop_source_cancelable(R2Ears *ears, const char *monitor,
                                          int (*cancelled)(void *), void *userdata)
{
    if (!ears || !monitor || !*monitor) { errno = EINVAL; return -1; }
    return r2_ears_open_source_cancelable(ears, monitor, cancelled, userdata);
}

int r2_ears_open_desktop_cancelable(R2Ears *ears,
                                    int (*cancelled)(void *), void *userdata)
{
    if (!ears) { errno = EINVAL; return -1; }
    R2AudioDiscovery sources;
    if (r2_audio_discover_cancelable(&sources, cancelled, userdata) != 0) return -1;
    int rc = sources.desktop_monitor ? r2_ears_open_source_cancelable(
        ears, sources.desktop_monitor, cancelled, userdata) : -1;
    r2_audio_discovery_free(&sources);
    return rc;
}

int r2_ears_open_desktop(R2Ears *ears)
{
    if (!ears) return -1;
    R2AudioDiscovery sources;
    if (r2_audio_discover(&sources) != 0) return -1;
    int rc = sources.desktop_monitor
        ? r2_ears_open_desktop_source(ears, sources.desktop_monitor) : -1;
    r2_audio_discovery_free(&sources);
    return rc;
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

    /* A reopen can be requested using the name from its own event.
     * Preserve the complete path before the previous source is closed. */
    char *stable_path = strdup(path);
    if (!stable_path) return -1;
    r2_ears_close(ears);
    atomic_store(&ears->cancel_requested, 0);
    R2AudioFile *decoder = r2_audio_file_open(stable_path, R2_EARS_SAMPLE_RATE,
                                               R2_EARS_CHANNELS);
    if (!decoder)
    {
        fprintf(
            stderr,
            "R2 Ears: could not open audio file: %s\n",
            stable_path
        );
        r2_log_sensory("audio_file_open_failed",
                       "R2 Ears could not decode the requested audio file.",
                       "FFmpeg did not provide a readable PCM stream.",
                       stable_path);
        free(stable_path);
        return -1;
    }

    pthread_mutex_lock(&ears->transport_lock);
    ears->file_stream = decoder;
    if (atomic_load(&ears->cancel_requested))
        r2_audio_file_cancel(decoder);
    pthread_mutex_unlock(&ears->transport_lock);
    if (atomic_load(&ears->cancel_requested)) {
        r2_ears_close(ears);
        errno = ECANCELED;
        free(stable_path);
        return -1;
    }

    ears->format.sample_rate = R2_EARS_SAMPLE_RATE;
    ears->format.channels = R2_EARS_CHANNELS;
    ears->format.bits_per_sample = R2_EARS_BITS_PER_SAMPLE;

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

    if (r2_ears_set_source_name(ears, stable_path) != 0) {
        r2_ears_close(ears);
        free(stable_path);
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
                       details, stable_path);
    }
    free(stable_path);

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
        if (!ears->file_stream) { errno = EBADF; return -1; }
        ssize_t bytes_read = r2_audio_file_read(ears->file_stream, buffer,
                                                 buffer_size);
        if (bytes_read > 0) {
            uint64_t sample_time = r2_ears_timestamp();
            ears->bytes_since_log += (uint64_t)bytes_read;
            if (ears->last_log_timestamp_us == 0 ||
                sample_time < ears->last_log_timestamp_us ||
                sample_time - ears->last_log_timestamp_us >= 5000000ULL) {
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
                ears->last_log_timestamp_us = sample_time;
            }
            return bytes_read;
        }
        int read_error = errno;
        if (bytes_read < 0 && read_error != ECANCELED)
            r2_log_sensory("audio_read_failed",
                           "R2 Ears encountered an error reading audio-file samples.",
                           "The decoded PCM stream ended with an error.",
                           ears->event.source_name);
        /* Do not free the decoder here: the lifecycle owner must first
         * establish that the reader has exited, then close and reap FFmpeg. */
        atomic_store(&ears->open, 0);
        if (bytes_read < 0) errno = read_error;
        return bytes_read;
    }


    /* -----------------------------------------------------
     * PulseAudio / PipeWire source
     * ----------------------------------------------------- */

    if (ears->pulse_stream)
    {
        if (r2_audio_capture_read(ears->pulse_stream, buffer, buffer_size) < 0) {
            int read_errno = errno;
            if (read_errno != ECANCELED) {
                r2_log_sensory("audio_read_failed",
                               "R2 Ears encountered a live audio capture error.",
                               read_errno == ENODEV ? "The recording source changed and needs to be reopened." :
                                                      "Asynchronous PulseAudio recording failed.",
                               ears->event.source_name);
            }
            /* The reader never releases this handle. Stop joins before close. */
            atomic_store(&ears->open, 0);
            errno = read_errno;
            return -1;
        }

        uint64_t sample_time = r2_ears_timestamp();
        ears->bytes_since_log += buffer_size;

        if (ears->last_log_timestamp_us == 0 ||
            sample_time < ears->last_log_timestamp_us ||
            sample_time - ears->last_log_timestamp_us >= 5000000ULL) {
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
            ears->last_log_timestamp_us = sample_time;
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

    return atomic_load(&ears->open);
}


R2HearingOrigin r2_ears_get_origin(R2Ears *ears)
{
    if (!ears)
        return R2_HEARING_NONE;

    return ears->event.origin;
}
