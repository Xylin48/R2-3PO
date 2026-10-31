#define _POSIX_C_SOURCE 200809L
#include "Ears.h"
#include "Log.h"
#include <pulse/pulseaudio.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <pulse/error.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ears_native_test_endpoints.h"

static int cancel_after_poll(void *userdata)
{
    int *calls = userdata;
    return ++*calls >= 3;
}

static atomic_int snapshots_done, snapshots_count;
static void *read_snapshots(void *userdata)
{
    R2Ears *ears = userdata;
    while (!atomic_load(&snapshots_done)) {
        R2HearingEvent event;
        assert(r2_ears_get_event(ears, &event) == 0);
        assert(event.origin == R2_HEARING_WORLD);
        assert(event.format.sample_rate == 48000 && event.format.channels == 2);
        assert(strstr(event.source_name, "test_mic"));
        assert(r2_ears_is_open(ears));
        assert(r2_ears_get_origin(ears) == R2_HEARING_WORLD);
        atomic_fetch_add(&snapshots_count, 1);
    }
    return NULL;
}

int64_t r2_log_sensory(const char *type, const char *text,
                       const char *details, const char *source)
{
    (void)type; (void)text; (void)details; (void)source;
    return 1;
}

static void check_pcm(R2Ears *stream, unsigned char expected)
{
    unsigned char samples[1920];
    assert(r2_ears_read(stream, samples, sizeof(samples)) == (ssize_t)sizeof(samples));
    for (size_t i = 0; i < sizeof(samples); ++i)
        assert(samples[i] == expected);
}

int main(void)
{
    char temp[] = "/tmp/r2-dual-ear-XXXXXX";
    assert(mkdtemp(temp));
    char path[PATH_MAX], environment[PATH_MAX * 2];
    assert(snprintf(path, sizeof(path), "%s/pactl", temp) < (int)sizeof(path));
    FILE *fp = fopen(path, "w");
    assert(fp);
    assert(fputs(
        "#!/bin/sh\n"
        "if [ \"$1\" = \"get-default-source\" ]; then echo alsa_input.test_mic\n"
        "elif [ \"$1\" = \"get-default-sink\" ]; then echo alsa_output.test\n"
        "elif [ \"$1\" = \"list\" ]; then\n"
        "  echo '1 alsa_input.test_mic x'\n"
        "  echo '2 alsa_output.test.monitor x'\n"
        "fi\n", fp) >= 0);
    assert(fclose(fp) == 0 && chmod(path, 0700) == 0);
    const char *old_path = getenv("PATH");
    assert(snprintf(environment, sizeof(environment), "%s:%s", temp,
                    old_path ? old_path : "/usr/bin") < (int)sizeof(environment));
    assert(setenv("PATH", environment, 1) == 0);

    R2Ears *mic = NULL, *desktop = NULL;
    assert(r2_ears_init(&mic) == 0 && r2_ears_init(&desktop) == 0);
    assert(r2_ears_open_microphone(mic) == 0);
    assert(r2_ears_open_desktop(desktop) == 0);
    assert(r2_ears_is_open(mic) && r2_ears_is_open(desktop));
    R2HearingEvent event_mic = {0}, event_desktop = {0};
    assert(r2_ears_get_event(mic, &event_mic) == 0);
    assert(r2_ears_get_event(desktop, &event_desktop) == 0);
    assert(strstr(event_mic.source_name, "test_mic"));
    assert(!strcmp(event_desktop.source_name, "desktop-audio"));
    check_pcm(mic, 0xa5);
    check_pcm(desktop, 0x5a);
    r2_ears_close(mic);
    assert(!r2_ears_is_open(mic) && r2_ears_is_open(desktop));
    check_pcm(desktop, 0x5a);
    assert(r2_ears_open_microphone(mic) == 0);
    r2_ears_close(desktop);
    assert(r2_ears_is_open(mic) && !r2_ears_is_open(desktop));
    check_pcm(mic, 0xa5);
    /* The 2500-byte native fragments straddle 1920-byte caller reads. */
    pthread_t snapshots;
    assert(pthread_create(&snapshots, NULL, read_snapshots, mic) == 0);
    while (!atomic_load(&snapshots_count)) sched_yield();
    for (int i = 0; i < 10000; ++i) check_pcm(mic, 0xa5);
    atomic_store(&snapshots_done, 1);
    assert(pthread_join(snapshots, NULL) == 0);
    r2_ears_close(mic);
    assert(r2_ears_open_microphone(mic) == 0);
    last_stream->paused = 1;
    unsigned char pcm[1920];
    int calls = 0;
    assert(r2_ears_read_cancelable(mic, pcm, sizeof(pcm), cancel_after_poll, &calls) == -1);
    assert(errno == ECANCELED && calls == 3);
    assert(r2_ears_is_open(mic));
    /* Cancelling this reader has no effect on the independent desktop. */
    assert(r2_ears_open_desktop(desktop) == 0);
    check_pcm(desktop, 0x5a);
    r2_ears_close(mic);
    assert(r2_ears_open_microphone(mic) == 0);
    last_stream->hole = 1;
    assert(r2_ears_read(mic, pcm, sizeof(pcm)) == -1 && errno == EPIPE);
    assert(!r2_ears_is_open(mic));
    assert(r2_ears_open_microphone(mic) == 0);
    last_stream->failed = 1;
    assert(r2_ears_read(mic, pcm, sizeof(pcm)) == -1 && errno == EIO);
    assert(!r2_ears_is_open(mic));
    connecting = 1; calls = 0;
    assert(r2_ears_open_source_cancelable(mic, "test_mic", cancel_after_poll, &calls) == -1);
    assert(errno == ECANCELED && !r2_ears_is_open(mic));
    connecting = 0;
    r2_ears_shutdown(desktop);
    r2_ears_shutdown(mic);
    assert(open_count == close_count && open_count == 7 && contexts == 0);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
    puts("Native fragmented PCM, dual sources, cancellation, loss/error cleanup and concurrent metadata passed.");
    return 0;
}
