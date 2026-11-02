/*
 * Runtime: multi-output Watch captures actual independent Ears objects
 * with synthetic Pulse monitors. No physical audio server required.
 */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { R2Ears *receiver; char *source; unsigned char marker; } FakeOutput;
static FakeOutput opened[32];
static size_t opened_count = 0;
static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;

int __wrap_r2_ears_open_desktop_source(R2Ears *receiver, const char *source)
{
    assert(receiver && source && *source);
    pthread_mutex_lock(&fake_lock);
    assert(opened_count < sizeof(opened) / sizeof(opened[0]));
    opened[opened_count].receiver = receiver;
    opened[opened_count].source = strdup(source);
    opened[opened_count].marker = (unsigned char)(opened_count + 17);
    assert(opened[opened_count].source);
    ++opened_count;
    pthread_mutex_unlock(&fake_lock);
    return 0;
}

ssize_t __wrap_r2_ears_read(R2Ears *receiver, void *buffer, size_t size)
{
    pthread_mutex_lock(&fake_lock);
    unsigned char marker = 0;
    for (size_t i = 0; i < opened_count; ++i)
        if (opened[i].receiver == receiver)
            marker = opened[i].marker;
    pthread_mutex_unlock(&fake_lock);
    assert(marker && size == 1920);
    memset(buffer, marker, size);
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 2000000L};
    nanosleep(&pause, NULL);
    return (ssize_t)size;
}

static void wait_for_pcm(const char *monitor, unsigned char expected)
{
    unsigned char samples[1920];
    for (int attempt = 0; attempt < 800; ++attempt) {
        if (r2_ears_extra_copy_recent(monitor, sizeof(samples), samples) == 0) {
            for (size_t i = 0; i < sizeof(samples); ++i)
                assert(samples[i] == expected);
            return;
        }
        struct timespec pause = {.tv_sec = 0, .tv_nsec = 2000000L};
        nanosleep(&pause, NULL);
    }
    assert(!"independent source produced no PCM");
}

int main(void)
{
    R2AudioDiscovery audio = {0};
    R2AudioPlaybackOutput outputs[12] = {0};
    char names[12][1024];
    for (size_t i = 0; i < 12; ++i) {
        int written = snprintf(names[i], sizeof(names[i]),
                               "alsa_output.independent_%zu.", i);
        assert(written > 0);
        size_t prefix = (size_t)written;
        memset(names[i] + prefix, (int)('a' + i), 850);
        strcpy(names[i] + prefix + 850, ".monitor");
        outputs[i].monitor = names[i];
        outputs[i].sink_index = (uint32_t)(100 + i);
    }
    audio.playback_outputs = outputs;
    audio.playback_output_count = 12;
    audio.desktop_monitor = names[0];

    assert(r2_ears_extra_reconcile(&audio, names[0]) == 0);
    assert(r2_ears_extra_source_count() == 11);
    /* No hardcoded 2/4/8 source quota; each has exact complete identity. */
    assert(opened_count == 11);
    for (size_t i = 1; i < 12; ++i)
        wait_for_pcm(names[i], (unsigned char)(i + 16));

    /* A physical reroute makes the formerly primary monitor eligible,
     * while the new primary is no longer duplicated by the extra list. */
    assert(r2_ears_extra_reconcile(&audio, names[5]) == 0);
    assert(r2_ears_extra_source_count() == 11);
    assert(r2_ears_extra_copy_recent(names[5], 1920, (unsigned char[1920]){0}) < 0);
    wait_for_pcm(names[0], 28);
    assert(r2_ears_extra_reconcile(&audio, names[5]) == 0);
    assert(r2_ears_extra_source_count() == 11);

    /* Successful snapshot without sources removes only now-absent outputs. */
    audio.playback_output_count = 0;
    assert(r2_ears_extra_reconcile(&audio, NULL) == 0);
    assert(r2_ears_extra_source_count() == 0);
    assert(r2_ears_extra_copy_recent(names[1], 1920,
                                     (unsigned char[1920]){0}) < 0);

    audio.playback_output_count = 12;
    assert(r2_ears_extra_reconcile(&audio, names[0]) == 0);
    assert(r2_ears_extra_source_count() == 11);
    r2_ears_extra_stop_all();
    assert(r2_ears_extra_source_count() == 0);

    for (size_t i = 0; i < opened_count; ++i) free(opened[i].source);
    puts("All independently routed playback outputs retain full identity, isolated PCM and safe teardown.");
    return 0;
}
