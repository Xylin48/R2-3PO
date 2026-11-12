#define _POSIX_C_SOURCE 200809L
#include "AudioCapture.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/error.h>
#include <pulse/rtclock.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>

/* Only the connection handshake is timed. Real-time capture is allowed to
 * wait for silent sources forever *until* an explicit cancel signals it. */
#define R2_AUDIO_CONNECT_TIMEOUT_US (5000000ULL)

struct R2AudioCapture {
    pa_threaded_mainloop *mainloop;
    pa_context *context;
    pa_stream *stream;
    pa_time_event *connect_timeout;
    atomic_int mainloop_started;
    int connect_timed_out;
    int stream_moved; /* Protected by the threaded mainloop lock. */
    atomic_int cancelled;
    /* A peek fragment stays valid until dropped. Retain its offset so
     * arbitrarily large producer chunks are never silently discarded. */
    const void *fragment;
    size_t fragment_size;
    size_t fragment_offset;
};

static void r2_capture_wake(pa_threaded_mainloop *loop)
{
    pa_threaded_mainloop_signal(loop, 0);
}

static void r2_capture_context_state(pa_context *ctx, void *userdata)
{
    (void)ctx;
    R2AudioCapture *capture = userdata;
    r2_capture_wake(capture->mainloop);
}

static void r2_capture_stream_state(pa_stream *stream, void *userdata)
{
    (void)stream;
    R2AudioCapture *capture = userdata;
    r2_capture_wake(capture->mainloop);
}

static void r2_capture_stream_readable(pa_stream *stream, size_t size, void *userdata)
{
    (void)stream;
    (void)size;
    R2AudioCapture *capture = userdata;
    r2_capture_wake(capture->mainloop);
}

static void r2_capture_stream_moved(pa_stream *stream, void *userdata)
{
    (void)stream;
    R2AudioCapture *capture = userdata;
    capture->stream_moved = 1;
    r2_capture_wake(capture->mainloop);
}

static void r2_capture_connect_timeout(pa_mainloop_api *api,
                                       pa_time_event *event,
                                       const struct timeval *tv, void *userdata)
{
    (void)api;
    (void)event;
    (void)tv;
    R2AudioCapture *capture = userdata;
    capture->connect_timed_out = 1;
    r2_capture_wake(capture->mainloop);
}

R2AudioCapture *r2_audio_capture_new(void)
{
    R2AudioCapture *capture = calloc(1, sizeof(*capture));
    if (!capture) return NULL;
    atomic_init(&capture->cancelled, 0);
    atomic_init(&capture->mainloop_started, 0);
    capture->mainloop = pa_threaded_mainloop_new();
    if (!capture->mainloop) {
        free(capture);
        return NULL;
    }
    return capture;
}

int r2_audio_capture_connect(R2AudioCapture *capture, const char *source,
                             const pa_sample_spec *format, size_t fragment_bytes)
{
    if (!capture || !source || !*source || !format ||
        !pa_sample_spec_valid(format) || fragment_bytes == 0 ||
        fragment_bytes > UINT32_MAX || capture->mainloop_started) {
        errno = EINVAL;
        return -1;
    }
    if (atomic_load(&capture->cancelled)) {
        errno = ECANCELED;
        return -1;
    }
    if (pa_threaded_mainloop_start(capture->mainloop) < 0) {
        errno = EIO;
        return -1;
    }
    capture->mainloop_started = 1;
    pa_threaded_mainloop_lock(capture->mainloop);

    capture->context = pa_context_new(
        pa_threaded_mainloop_get_api(capture->mainloop), "R2-3PO raw hearing");
    if (!capture->context) goto failed;
    pa_context_set_state_callback(capture->context, r2_capture_context_state, capture);
    if (pa_context_connect(capture->context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto failed;
    const pa_usec_t now = pa_rtclock_now();
    if (now > UINT64_MAX - R2_AUDIO_CONNECT_TIMEOUT_US) goto failed;
    capture->connect_timeout = pa_context_rttime_new(
        capture->context, now + R2_AUDIO_CONNECT_TIMEOUT_US,
        r2_capture_connect_timeout, capture);
    if (!capture->connect_timeout) goto failed;

    for (;;) {
        pa_context_state_t state = pa_context_get_state(capture->context);
        if (state == PA_CONTEXT_READY) break;
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto failed;
        if (capture->connect_timed_out || atomic_load(&capture->cancelled)) {
            errno = capture->connect_timed_out ? ETIMEDOUT : ECANCELED;
            goto failed;
        }
        pa_threaded_mainloop_wait(capture->mainloop);
    }
    if (atomic_load(&capture->cancelled)) {
        errno = ECANCELED;
        goto failed;
    }

    capture->stream = pa_stream_new(capture->context, "R2 Hearing", format, NULL);
    if (!capture->stream) goto failed;
    pa_stream_set_state_callback(capture->stream, r2_capture_stream_state, capture);
    pa_stream_set_read_callback(capture->stream, r2_capture_stream_readable, capture);
    /* A stream moved at any point in this connection must be reopened. */
    pa_stream_set_moved_callback(capture->stream, r2_capture_stream_moved, capture);
    pa_buffer_attr attr = {
        .maxlength = UINT32_MAX, .tlength = UINT32_MAX,
        .prebuf = UINT32_MAX, .minreq = UINT32_MAX,
        .fragsize = (uint32_t)fragment_bytes
    };
    if (pa_stream_connect_record(capture->stream, source, &attr, PA_STREAM_DONT_MOVE) < 0)
        goto failed;
    for (;;) {
        pa_stream_state_t state = pa_stream_get_state(capture->stream);
        if (state == PA_STREAM_READY) break;
        if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED ||
            pa_context_get_state(capture->context) != PA_CONTEXT_READY)
            goto failed;
        if (capture->connect_timed_out || atomic_load(&capture->cancelled)) {
            errno = capture->connect_timed_out ? ETIMEDOUT : ECANCELED;
            goto failed;
        }
        pa_threaded_mainloop_wait(capture->mainloop);
    }
    if (capture->connect_timed_out || atomic_load(&capture->cancelled)) {
        errno = atomic_load(&capture->cancelled) ? ECANCELED : ETIMEDOUT;
        goto failed;
    }
    const char *selected = pa_stream_get_device_name(capture->stream);
    if (capture->stream_moved || !selected || strcmp(selected, source) != 0) {
        errno = ENODEV;
        goto failed;
    }
    pa_threaded_mainloop_get_api(capture->mainloop)->time_free(capture->connect_timeout);
    capture->connect_timeout = NULL;
    pa_threaded_mainloop_unlock(capture->mainloop);
    return 0;

failed:
    if (!errno) errno = EIO;
    /* The lifecycle owner will free/disconnect the partially initialized
     * context; it may still be referenced by a concurrent cancel. */
    if (capture->connect_timeout) {
        pa_threaded_mainloop_get_api(capture->mainloop)->time_free(capture->connect_timeout);
        capture->connect_timeout = NULL;
    }
    pa_threaded_mainloop_unlock(capture->mainloop);
    return -1;
}

/* Never hold the threaded-mainloop lock over the caller's main lifecycle
 * lock. The wait releases it, so a stop can signal even if no PCM arrives. */
ssize_t r2_audio_capture_read(R2AudioCapture *capture, void *buffer, size_t bytes)
{
    if (!capture || !buffer || !bytes || bytes > SSIZE_MAX ||
        !capture->stream || !capture->mainloop_started) {
        errno = EINVAL;
        return -1;
    }
    pa_threaded_mainloop_lock(capture->mainloop);
    size_t copied = 0;
    int error = EIO;
    while (copied < bytes) {
        if (atomic_load(&capture->cancelled)) { error = ECANCELED; goto failed; }
        if (capture->stream_moved) { error = ENODEV; goto failed; }
        if (pa_context_get_state(capture->context) != PA_CONTEXT_READY ||
            pa_stream_get_state(capture->stream) != PA_STREAM_READY)
            goto failed;
        if (capture->fragment_offset == capture->fragment_size) {
            capture->fragment = NULL;
            capture->fragment_size = capture->fragment_offset = 0;
            size_t readable = pa_stream_readable_size(capture->stream);
            if (readable == (size_t)-1) goto failed;
            if (!readable) {
                pa_threaded_mainloop_wait(capture->mainloop);
                continue;
            }
            if (pa_stream_peek(capture->stream, &capture->fragment,
                               &capture->fragment_size) < 0) goto failed;
            if (!capture->fragment_size) {
                pa_threaded_mainloop_wait(capture->mainloop);
                continue;
            }
        }
        size_t available = capture->fragment_size - capture->fragment_offset;
        size_t take = bytes - copied < available ? bytes - copied : available;
        if (capture->fragment)
            memcpy((unsigned char *)buffer + copied,
                   (const unsigned char *)capture->fragment + capture->fragment_offset, take);
        else
            memset((unsigned char *)buffer + copied, 0, take); /* PulseAudio hole */
        capture->fragment_offset += take;
        copied += take;
        if (capture->fragment_offset == capture->fragment_size) {
            if (pa_stream_drop(capture->stream) < 0) goto failed;
            capture->fragment = NULL;
            capture->fragment_offset = capture->fragment_size = 0;
        }
    }
    pa_threaded_mainloop_unlock(capture->mainloop);
    return (ssize_t)copied;

failed:
    pa_threaded_mainloop_unlock(capture->mainloop);
    errno = error;
    return -1;
}

void r2_audio_capture_cancel(R2AudioCapture *capture)
{
    if (!capture) return;
    atomic_store(&capture->cancelled, 1);
    if (!capture->mainloop_started) return;
    pa_threaded_mainloop_lock(capture->mainloop);
    pa_threaded_mainloop_signal(capture->mainloop, 0);
    pa_threaded_mainloop_unlock(capture->mainloop);
}

void r2_audio_capture_free(R2AudioCapture *capture)
{
    if (!capture) return;
    r2_audio_capture_cancel(capture);
    if (capture->mainloop_started)
        pa_threaded_mainloop_lock(capture->mainloop);
    if (capture->stream) {
        pa_stream_set_state_callback(capture->stream, NULL, NULL);
        pa_stream_set_read_callback(capture->stream, NULL, NULL);
        pa_stream_set_moved_callback(capture->stream, NULL, NULL);
        pa_stream_disconnect(capture->stream);
        pa_stream_unref(capture->stream);
    }
    if (capture->connect_timeout) {
        pa_threaded_mainloop_get_api(capture->mainloop)->time_free(capture->connect_timeout);
    }
    if (capture->context) {
        pa_context_set_state_callback(capture->context, NULL, NULL);
        pa_context_disconnect(capture->context);
        pa_context_unref(capture->context);
    }
    if (capture->mainloop_started) {
        pa_threaded_mainloop_unlock(capture->mainloop);
        pa_threaded_mainloop_stop(capture->mainloop);
    }
    pa_threaded_mainloop_free(capture->mainloop);
    free(capture);
}
