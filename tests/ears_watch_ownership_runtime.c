/*
 * Runtime: Watch may stop only the exact raw audio session it created.
 * Real core start/stop functions and worker threads run with synthetic Ears
 * transport, so no host sound devices or live databases are required.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_int mic_closes = ATOMIC_VAR_INIT(0);
static atomic_int desktop_closes = ATOMIC_VAR_INIT(0);
static atomic_int synthetic_reads = ATOMIC_VAR_INIT(0);

static void *direct_listen_worker(void *unused)
{
    (void)unused;
    return r2_ears_listen_cancelable(15U, NULL, NULL);
}

static double duration_seconds(struct timespec before, struct timespec after)
{
    return (double)(after.tv_sec - before.tv_sec) +
           (double)(after.tv_nsec - before.tv_nsec) / 1000000000.0;
}

int __wrap_r2_ears_open_microphone(R2Ears *receiver)
{
    assert(receiver == ears);
    return 0;
}

int __wrap_r2_ears_open_desktop(R2Ears *receiver)
{
    assert(receiver == ears_desktop);
    return 0;
}

ssize_t __wrap_r2_ears_read(R2Ears *receiver, void *samples, size_t bytes)
{
    assert(receiver == ears || receiver == ears_desktop);
    assert(bytes == 1920U);
    memset(samples, 0, bytes);
    atomic_fetch_add(&synthetic_reads, 1);
    struct timespec short_sample = {.tv_sec = 0, .tv_nsec = 2000000L};
    (void)nanosleep(&short_sample, NULL);
    return (ssize_t)bytes;
}

void __wrap_r2_ears_close(R2Ears *receiver)
{
    if (receiver == ears)
        atomic_fetch_add(&mic_closes, 1);
    else if (receiver == ears_desktop)
        atomic_fetch_add(&desktop_closes, 1);
    else
        assert(!"Unknown Ears receiver closed");
}

int main(void)
{
    assert(r2_ears_init(&ears) == 0);

    uint64_t mic_first = r2_ears_start_for_watch();
    assert(mic_first != 0 && r2_ears_status() == 1);
    assert(r2_ears_start_for_watch() == 0); /* attached, not a new owner */
    assert(r2_ears_stop_if_watch_owned(0) == 0);
    assert(atomic_load(&mic_closes) == 0 && r2_ears_status() == 1);

    /* A manual start takes custody from Watch without closing its stream. */
    assert(r2_ears_start() == 0);
    assert(r2_ears_stop_if_watch_owned(mic_first) == 0);
    assert(atomic_load(&mic_closes) == 0 && r2_ears_status() == 1);
    assert(r2_ears_stop() == 0);
    assert(atomic_load(&mic_closes) == 1 && r2_ears_status() == 0);

    uint64_t mic_second = r2_ears_start_for_watch();
    assert(mic_second != 0 && mic_second != mic_first);
    assert(r2_ears_stop_if_watch_owned(mic_first) == 0);
    assert(atomic_load(&mic_closes) == 1 && r2_ears_status() == 1);
    assert(r2_ears_stop_if_watch_owned(mic_second) == 0);
    assert(atomic_load(&mic_closes) == 2 && r2_ears_status() == 0);

    uint64_t desktop_first = r2_ears_start_desktop_for_watch();
    assert(desktop_first != 0 && r2_ears_status_desktop() == 1);
    assert(r2_ears_start_desktop_for_watch() == 0);
    assert(r2_ears_start_desktop() == 0); /* manual takeover */
    assert(r2_ears_stop_desktop_if_watch_owned(desktop_first) == 0);
    assert(atomic_load(&desktop_closes) == 0 &&
           r2_ears_status_desktop() == 1);
    assert(r2_ears_stop_desktop() == 0);
    assert(atomic_load(&desktop_closes) == 1 &&
           r2_ears_status_desktop() == 0);

    uint64_t desktop_second = r2_ears_start_desktop_for_watch();
    assert(desktop_second != 0 && desktop_second != desktop_first);
    assert(r2_ears_stop_desktop_if_watch_owned(desktop_first) == 0);
    assert(atomic_load(&desktop_closes) == 1 &&
           r2_ears_status_desktop() == 1);
    assert(r2_ears_stop_desktop_if_watch_owned(desktop_second) == 0);
    assert(atomic_load(&desktop_closes) == 2 &&
           r2_ears_status_desktop() == 0);

    /*
     * Explicit stop must announce cancellation before waiting for the lock
     * held across a one-shot microphone capture. A synthetic 15-second
     * listening request must not run to completion after stop is requested.
     */
    char model_path[] = "/tmp/r2-audio-test-model-XXXXXX";
    int model_fd = mkstemp(model_path);
    assert(model_fd >= 0);
    assert(close(model_fd) == 0);
    assert(setenv("R2_WHISPER_MODEL", model_path, 1) == 0);
    atomic_store(&synthetic_reads, 0);
    pthread_t listener;
    assert(pthread_create(&listener, NULL, direct_listen_worker, NULL) == 0);
    for (int i = 0; i < 2000 && atomic_load(&synthetic_reads) == 0; ++i) {
        struct timespec tick = {.tv_sec = 0, .tv_nsec = 1000000L};
        (void)nanosleep(&tick, NULL);
    }
    assert(atomic_load(&synthetic_reads) > 0);
    struct timespec before_stop, after_stop;
    assert(clock_gettime(CLOCK_MONOTONIC, &before_stop) == 0);
    assert(r2_ears_stop() == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after_stop) == 0);
    void *transcript = (void *)1;
    assert(pthread_join(listener, &transcript) == 0);
    assert(transcript == NULL);
    assert(duration_seconds(before_stop, after_stop) < 2.0);
    assert(unlink(model_path) == 0);

    r2_ears_shutdown(ears_desktop);
    ears_desktop = NULL;
    r2_ears_shutdown(ears);
    ears = NULL;

    puts("Watch ownership is generation-safe and explicit stop cancels active one-shot PCM capture.");
    return 0;
}
