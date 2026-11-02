/*
 * Exercise the real asynchronous capture read/cancel implementation while
 * replacing only Pulse's event loop and stream with a deterministic in-memory
 * fixture. The reader must wake when no producer supplies any PCM.
 */
#define _POSIX_C_SOURCE 200809L
#include "../AudioCapture.c"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_changed = PTHREAD_COND_INITIALIZER;
static atomic_int waiting = ATOMIC_VAR_INIT(0);
static atomic_int wakeups = ATOMIC_VAR_INIT(0);
static atomic_int dropped = ATOMIC_VAR_INIT(0);
static atomic_int readable = ATOMIC_VAR_INIT(0);
static unsigned char fragment[8] = {11, 12, 13, 14, 15, 16, 17, 18};

void __wrap_pa_threaded_mainloop_lock(pa_threaded_mainloop *loop)
{
    assert((uintptr_t)loop == 1);
    assert(pthread_mutex_lock(&event_lock) == 0);
}
void __wrap_pa_threaded_mainloop_unlock(pa_threaded_mainloop *loop)
{
    assert((uintptr_t)loop == 1);
    assert(pthread_mutex_unlock(&event_lock) == 0);
}
void __wrap_pa_threaded_mainloop_wait(pa_threaded_mainloop *loop)
{
    assert((uintptr_t)loop == 1);
    atomic_store(&waiting, 1);
    assert(pthread_cond_wait(&event_changed, &event_lock) == 0);
}
void __wrap_pa_threaded_mainloop_signal(pa_threaded_mainloop *loop, int wait_for_accept)
{
    assert((uintptr_t)loop == 1 && !wait_for_accept);
    atomic_fetch_add(&wakeups, 1);
    assert(pthread_cond_broadcast(&event_changed) == 0);
}

pa_context_state_t __wrap_pa_context_get_state(const pa_context *ctx)
{
    assert((uintptr_t)ctx == 2);
    return PA_CONTEXT_READY;
}
pa_stream_state_t __wrap_pa_stream_get_state(const pa_stream *stream)
{
    assert((uintptr_t)stream == 3);
    return PA_STREAM_READY;
}
size_t __wrap_pa_stream_readable_size(const pa_stream *stream)
{
    assert((uintptr_t)stream == 3);
    return (size_t)atomic_load(&readable);
}
int __wrap_pa_stream_peek(pa_stream *stream, const void **data, size_t *nbytes)
{
    assert((uintptr_t)stream == 3);
    assert(atomic_load(&readable) == 8);
    *data = fragment;
    *nbytes = sizeof(fragment);
    return 0;
}
int __wrap_pa_stream_drop(pa_stream *stream)
{
    assert((uintptr_t)stream == 3);
    atomic_store(&readable, 0);
    atomic_fetch_add(&dropped, 1);
    return 0;
}

typedef struct {
    R2AudioCapture *capture;
    ssize_t got;
    int saved_errno;
} BlockedRead;

static void *read_during_silence(void *userdata)
{
    BlockedRead *read = userdata;
    unsigned char buffer[1920];
    read->got = r2_audio_capture_read(read->capture, buffer, sizeof(buffer));
    read->saved_errno = errno;
    return NULL;
}

static double seconds_between(struct timespec first, struct timespec last)
{
    return (double)(last.tv_sec - first.tv_sec) +
           (double)(last.tv_nsec - first.tv_nsec) / 1000000000.0;
}

int main(void)
{
    R2AudioCapture capture = {0};
    capture.mainloop = (pa_threaded_mainloop *)(uintptr_t)1;
    capture.context = (pa_context *)(uintptr_t)2;
    capture.stream = (pa_stream *)(uintptr_t)3;
    capture.mainloop_started = 1;
    atomic_init(&capture.cancelled, 0);

    atomic_store(&readable, 8);
    unsigned char head[3], tail[5];
    assert(r2_audio_capture_read(&capture, head, sizeof(head)) == 3);
    assert(memcmp(head, fragment, sizeof(head)) == 0);
    assert(atomic_load(&dropped) == 0);
    assert(r2_audio_capture_read(&capture, tail, sizeof(tail)) == 5);
    assert(memcmp(tail, fragment + 3, sizeof(tail)) == 0);
    assert(atomic_load(&dropped) == 1);

    BlockedRead reader = {.capture = &capture, .got = 0, .saved_errno = 0};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, read_during_silence, &reader) == 0);
    for (int i = 0; i < 1500 && !atomic_load(&waiting); ++i) {
        struct timespec pause = {0, 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    assert(atomic_load(&waiting));
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    r2_audio_capture_cancel(&capture);
    assert(pthread_join(thread, NULL) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    assert(reader.got == -1 && reader.saved_errno == ECANCELED);
    assert(atomic_load(&wakeups) >= 1);
    assert(seconds_between(before, after) < 1.0);

    puts("Signalable async capture preserves partial chunks and wakes silent PCM reads on cancellation.");
    return 0;
}
