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
static char current_microphone[256] = {0}, current_desktop[256] = {0};
static void remember_source(char *destination, size_t capacity, const char *source)
{
    assert(source && strlen(source) < capacity);
    strcpy(destination, source);
}
int __wrap_r2_ears_get_event(R2Ears *receiver, R2HearingEvent *event)
{
    assert(event);
    const char *source = receiver == ears ? current_microphone :
                         receiver == ears_desktop ? current_desktop : NULL;
    assert(source);
    memset(event, 0, sizeof(*event));
    event->source_name = strdup(source);
    assert(event->source_name);
    return 0;
}


static void *direct_listen_worker(void *unused)
{
    (void)unused;
    return r2_ears_listen_cancelable(15U, NULL, NULL);
}

static atomic_int desktop_listen_done = ATOMIC_VAR_INIT(0);
static void *desktop_listen_worker(void *unused)
{
    (void)unused;
    r2_ears_desktop_requested = 1;
    r2_ears_recent_requested = 1;
    char *result = r2_ears_listen_cancelable(1U, NULL, NULL);
    atomic_store(&desktop_listen_done, 1);
    return result;
}

static double duration_seconds(struct timespec before, struct timespec after)
{
    return (double)(after.tv_sec - before.tv_sec) +
           (double)(after.tv_nsec - before.tv_nsec) / 1000000000.0;
}

int __wrap_r2_ears_open_microphone(R2Ears *receiver)
{
    assert(receiver == ears);
    remember_source(current_microphone, sizeof(current_microphone), "alsa_input.default");
    return 0;
}

int __wrap_r2_ears_open_desktop(R2Ears *receiver)
{
    assert(receiver == ears_desktop);
    remember_source(current_desktop, sizeof(current_desktop), "alsa_output.default.monitor");
    return 0;
}

int __wrap_r2_ears_open_microphone_source(R2Ears *receiver, const char *source)
{
    assert(receiver == ears && source && *source);
    remember_source(current_microphone, sizeof(current_microphone), source);
    return 0;
}

int __wrap_r2_ears_open_desktop_source(R2Ears *receiver, const char *source)
{
    assert(receiver == ears_desktop && source && *source);
    remember_source(current_desktop, sizeof(current_desktop), source);
    return 0;
}

int __wrap_r2_ears_open_source_cancelable(R2Ears *receiver, const char *source,
                                         int (*cancelled)(void *), void *userdata)
{
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
    if (receiver == ears)
        return source ? __wrap_r2_ears_open_microphone_source(receiver, source)
                      : __wrap_r2_ears_open_microphone(receiver);
    return __wrap_r2_ears_open_desktop_source(receiver, source);
}

int __wrap_r2_ears_open_desktop_cancelable(R2Ears *receiver,
                                          int (*cancelled)(void *), void *userdata)
{
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
    return __wrap_r2_ears_open_desktop(receiver);
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

    uint64_t mic_first = r2_ears_start_for_watch("alsa_input.test_mic");
    assert(mic_first != 0 && r2_ears_status() == 1);
    assert(r2_ears_status_microphone_source("alsa_input.test_mic") == 1);
    assert(r2_ears_status_microphone_source("alsa_input.other") == 0);
    assert(r2_ears_start_for_watch("alsa_input.test_mic") == 0); /* attached, not a new owner */
    assert(r2_ears_stop_if_watch_owned(0) == 0);
    assert(atomic_load(&mic_closes) == 0 && r2_ears_status() == 1);

    /* A manual start takes custody from Watch without closing its stream. */
    assert(r2_ears_start() == 0);
    assert(r2_ears_stop_if_watch_owned(mic_first) == 0);
    assert(atomic_load(&mic_closes) == 0 && r2_ears_status() == 1);
    assert(r2_ears_stop() == 0);
    assert(atomic_load(&mic_closes) == 1 && r2_ears_status() == 0);

    /* A real default-microphone reroute changes the exact active name.
     * The old Watch token can stop only its own capture, then a replacement
     * receives a fresh generation. */
    uint64_t mic_routed = r2_ears_start_for_watch("alsa_input.other");
    assert(mic_routed != 0 && r2_ears_status_microphone_source("alsa_input.other"));
    assert(!r2_ears_status_microphone_source("alsa_input.test_mic"));
    assert(r2_ears_stop_if_watch_owned(mic_routed) == 0);
    assert(atomic_load(&mic_closes) == 2);
    assert(!r2_ears_status_microphone_source("alsa_input.other"));

    uint64_t mic_second = r2_ears_start_for_watch("alsa_input.test_mic");
    assert(mic_second != 0 && mic_second != mic_first);
    assert(r2_ears_stop_if_watch_owned(mic_first) == 0);
    assert(atomic_load(&mic_closes) == 2 && r2_ears_status() == 1);
    assert(r2_ears_stop_if_watch_owned(mic_second) == 0);
    assert(atomic_load(&mic_closes) == 3 && r2_ears_status() == 0);

    uint64_t desktop_first = r2_ears_start_desktop_for_watch("alsa_output.test.monitor");
    assert(desktop_first != 0 && r2_ears_status_desktop() == 1);
    assert(r2_ears_start_desktop_for_watch("alsa_output.test.monitor") == 0);
    assert(r2_ears_start_desktop() == 0); /* manual takeover */
    assert(r2_ears_stop_desktop_if_watch_owned(desktop_first) == 0);
    assert(atomic_load(&desktop_closes) == 0 &&
           r2_ears_status_desktop() == 1);
    assert(r2_ears_stop_desktop() == 0);
    assert(atomic_load(&desktop_closes) == 1 &&
           r2_ears_status_desktop() == 0);

    uint64_t desktop_second = r2_ears_start_desktop_for_watch("alsa_output.test.monitor");
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
    /* A completed recognizer output must grow beyond the old 16 KiB cap. */
    char text_path[] = "/tmp/r2-transcript-full-XXXXXX";
    int text_fd = mkstemp(text_path);
    assert(text_fd >= 0);
    char expected[40001];
    memset(expected, 't', sizeof(expected) - 1);
    expected[sizeof(expected) - 1] = '\0';
    assert(write(text_fd, expected, sizeof(expected) - 1) == (ssize_t)sizeof(expected) - 1);
    assert(close(text_fd) == 0);
    char cli_path[] = "/tmp/r2-whisper-fake-XXXXXX";
    int cli_fd = mkstemp(cli_path);
    assert(cli_fd >= 0);
    char script[1024];
    int script_len = snprintf(script, sizeof(script),
        "#!/bin/sh\nwhile [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = -of ]; then shift; out=$1; fi\n  shift\ndone\n"
        "cp '%s' \"$out.txt\"\n", text_path);
    assert(script_len > 0 && (size_t)script_len < sizeof(script));
    assert(write(cli_fd, script, (size_t)script_len) == script_len);
    assert(fchmod(cli_fd, 0700) == 0 && close(cli_fd) == 0);
    assert(setenv("R2_WHISPER_CLI", cli_path, 1) == 0);
    char *complete = r2_ears_listen_cancelable(1U, NULL, NULL);
    assert(complete && !strcmp(complete, expected));
    free(complete);
    /* Desktop attention must neither wait for the independent microphone's
     * lifecycle mutex nor inherit its explicit-stop cancellation state. */
    pthread_mutex_lock(&ears_desktop_recent_lock);
    memset(ears_desktop_recent_audio, 0, 192000);
    ears_desktop_recent_length = 192000;
    ears_desktop_recent_write_position = 192000;
    pthread_mutex_unlock(&ears_desktop_recent_lock);
    atomic_store(&ears_desktop_capture_running, 1);
    pthread_mutex_lock(&ears_capture_lock);
    atomic_fetch_add(&ears_stop_requesters, 1);
    assert(pthread_create(&listener, NULL, desktop_listen_worker, NULL) == 0);
    for (int i = 0; i < 2000 && !atomic_load(&desktop_listen_done); ++i) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    assert(atomic_load(&desktop_listen_done));
    atomic_fetch_sub(&ears_stop_requesters, 1);
    pthread_mutex_unlock(&ears_capture_lock);
    void *desktop_transcript = NULL;
    assert(pthread_join(listener, &desktop_transcript) == 0);
    assert(desktop_transcript && !strcmp(desktop_transcript, expected));
    free(desktop_transcript);
    atomic_store(&ears_desktop_capture_running, 0);
    assert(unlink(text_path) == 0 && unlink(cli_path) == 0);
    assert(unlink(model_path) == 0);

    r2_ears_shutdown(ears_desktop);
    ears_desktop = NULL;
    r2_ears_shutdown(ears);
    ears = NULL;

    puts("Watch ownership is generation-safe and explicit stop cancels active one-shot PCM capture.");
    return 0;
}
