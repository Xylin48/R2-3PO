#define _POSIX_C_SOURCE 200809L
#include "Ears.h"
#include "AudioCapture.h"
#include <pulse/sample.h>
#include <errno.h>
#include "Log.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
/* White-box only for the borrowed, current event.source_name alias test.
 * Production uses the opaque Ears interface unchanged. */
#include "../Ears.c"

/* Transport injected below: Ears is real, Pulse server is synthetic. */
typedef struct {
    int desktop;
    int cancelled;
} FakeCapture;
static int open_count = 0, close_count = 0;
static int fail_mic_read = 0;

R2AudioCapture *r2_audio_capture_new(void)
{
    FakeCapture *c = calloc(1, sizeof(*c));
    assert(c);
    return (R2AudioCapture *)c;
}

int r2_audio_capture_connect(R2AudioCapture *handle, const char *device,
                             const pa_sample_spec *ss, size_t bytes)
{
    FakeCapture *c = (FakeCapture *)handle;
    assert(c && device && (strstr(device, "test_mic") || strstr(device, "monitor")));
    assert(ss && ss->rate == 48000 && ss->channels == 2 &&
           ss->format == PA_SAMPLE_S16LE);
    assert(bytes == 1920);
    c->desktop = strstr(device, "monitor") != NULL;
    ++open_count;
    return 0;
}

ssize_t r2_audio_capture_read(R2AudioCapture *handle, void *data, size_t bytes)
{
    FakeCapture *c = (FakeCapture *)handle;
    assert(c && bytes == 1920);
    if (c->cancelled) { errno = ECANCELED; return -1; }
    if (fail_mic_read && !c->desktop) { errno = EIO; return -1; }
    memset(data, c->desktop ? 0x5a : 0xa5, bytes);
    return (ssize_t)bytes;
}

void r2_audio_capture_cancel(R2AudioCapture *handle)
{
    if (handle) ((FakeCapture *)handle)->cancelled = 1;
}

void r2_audio_capture_free(R2AudioCapture *handle)
{
    assert(handle);
    ++close_count;
    free(handle);
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
    /* Exercise actual source identities beyond all old 512-byte metadata quotas. */
    char microphone_name[1024] = "alsa_input.test_mic.";
    char sink_name[1024] = "alsa_output.test.";
    size_t m = strlen(microphone_name), n = strlen(sink_name);
    memset(microphone_name + m, 'm', 850);
    microphone_name[m + 850] = '\0';
    memset(sink_name + n, 's', 850);
    sink_name[n + 850] = '\0';
    char monitor_name[1200];
    assert(snprintf(monitor_name, sizeof(monitor_name), "%s.monitor", sink_name) <
           (int)sizeof(monitor_name));
    R2Ears *mic = NULL, *desktop = NULL;
    assert(r2_ears_init(&mic) == 0 && r2_ears_init(&desktop) == 0);
    assert(r2_ears_open_microphone_source(mic, microphone_name) == 0);
    assert(r2_ears_open_desktop_source(desktop, monitor_name) == 0);
    assert(r2_ears_is_open(mic) && r2_ears_is_open(desktop));
    R2HearingEvent event_mic = {0}, event_desktop = {0};
    assert(r2_ears_get_event(mic, &event_mic) == 0);
    assert(r2_ears_get_event(desktop, &event_desktop) == 0);
    assert(!strcmp(event_mic.source_name, microphone_name));
    assert(!strcmp(event_desktop.source_name, monitor_name));
    check_pcm(mic, 0xa5);
    check_pcm(desktop, 0x5a);
    /* A source event describes when this capture opened, not the
     * last sample time; concurrent snapshots must see stable metadata. */
    R2HearingEvent mic_after_read = {0};
    assert(r2_ears_get_event(mic, &mic_after_read) == 0);
    assert(mic_after_read.timestamp == event_mic.timestamp);
    assert(!strcmp(mic_after_read.source_name, event_mic.source_name));
    r2_ears_release_event(&mic_after_read);
    /* The caller is allowed to reopen with a pointer into the current event.
     * The old implementation freed it during close before opening the source. */
    assert(r2_ears_open_source(mic, mic->event.source_name) == 0);
    assert(r2_ears_is_open(mic));
    R2HearingEvent reopened = {0};
    assert(r2_ears_get_event(mic, &reopened) == 0);
    assert(!strcmp(reopened.source_name, microphone_name));
    r2_ears_release_event(&reopened);
    r2_ears_close(mic);
    /* An event snapshot remains owned by its caller after the live source closes. */
    assert(!strcmp(event_mic.source_name, microphone_name));
    assert(!r2_ears_is_open(mic) && r2_ears_is_open(desktop));
    check_pcm(desktop, 0x5a);
    assert(r2_ears_open_microphone_source(mic, microphone_name) == 0);
    r2_ears_close(desktop);
    assert(r2_ears_is_open(mic) && !r2_ears_is_open(desktop));
    check_pcm(mic, 0xa5);
    /* A worker read error invalidates the stream without freeing it under
     * the reader. The lifecycle owner closes it only after the reader exits. */
    fail_mic_read = 1;
    unsigned char failure_samples[1920];
    assert(r2_ears_read(mic, failure_samples, sizeof(failure_samples)) == -1);
    assert(r2_ears_is_open(mic) == 0);
    assert(close_count == 3);
    r2_ears_release_event(&event_mic);
    r2_ears_release_event(&event_desktop);
    r2_ears_shutdown(desktop);
    r2_ears_shutdown(mic);
    assert(open_count == close_count && open_count == 4);
    puts("Independent microphone and desktop PCM sources, metadata and lifecycles passed.");
    return 0;
}
