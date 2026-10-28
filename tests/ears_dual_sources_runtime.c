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
    r2_ears_shutdown(desktop);
    r2_ears_shutdown(mic);
    assert(open_count == close_count && open_count == 3);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
    puts("Independent microphone and desktop PCM sources, metadata and lifecycles passed.");
    return 0;
}
