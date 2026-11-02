#define _POSIX_C_SOURCE 200809L
#include "Ears.h"
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

typedef struct { int desktop; } FakePulse;
static int open_count = 0, close_count = 0;
static int fail_mic_read = 0;

/* Use real Ears.c with controlled Pulse device objects: no hardware needed. */
pa_simple *__wrap_pa_simple_new(const char *server, const char *name,
    pa_stream_direction_t dir, const char *dev, const char *stream_name,
    const pa_sample_spec *ss, const pa_channel_map *map,
    const pa_buffer_attr *attr, int *error)
{
    (void)server; (void)name; (void)stream_name; (void)map; (void)attr;
    assert(dir == PA_STREAM_RECORD);
    assert(ss->rate == 48000 && ss->channels == 2);
    assert(dev && (strstr(dev, "test_mic") || strstr(dev, "monitor")));
    FakePulse *p = calloc(1, sizeof(*p));
    assert(p);
    p->desktop = strstr(dev, "monitor") != NULL;
    if (error) *error = 0;
    ++open_count;
    return (pa_simple *)p;
}
int __wrap_pa_simple_read(pa_simple *s, void *data, size_t bytes, int *error)
{
    FakePulse *p = (FakePulse *)s;
    assert(p && bytes == 1920);
    if (fail_mic_read && !p->desktop) {
        if (error) *error = PA_ERR_IO;
        return -1;
    }
    memset(data, p->desktop ? 0x5a : 0xa5, bytes);
    if (error) *error = 0;
    return 0;
}
void __wrap_pa_simple_free(pa_simple *s)
{
    assert(s);
    ++close_count;
    free(s);
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
    assert(close_count == 2);
    r2_ears_release_event(&event_mic);
    r2_ears_release_event(&event_desktop);
    r2_ears_shutdown(desktop);
    r2_ears_shutdown(mic);
    assert(open_count == close_count && open_count == 3);
    puts("Independent microphone and desktop PCM sources, metadata and lifecycles passed.");
    return 0;
}
