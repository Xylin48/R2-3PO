#define _POSIX_C_SOURCE 200809L
#include "Ears.h"
#include "Log.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ears_native_test_endpoints.h"

int64_t r2_log_sensory(const char *type, const char *summary,
                       const char *details, const char *source)
{
    (void)type; (void)summary; (void)details; (void)source;
    return 0;
}
static int cancel_query(void *data)
{
    if (!connecting && fake_stall_stage != fake_last_stage) return 0;
    return ++*(int *)data >= 4;
}
static void check_empty(const R2EarsDiscovery *d)
{
    assert(!d->microphone_source && !d->desktop_monitor && !d->desktop_playing);
    assert(d->playback_pid == -1 && contexts == 0 && fake_operations == 0);
}
int main(void)
{
    R2EarsDiscovery d = {0};
    fake_source_name = NULL; fake_playing = 0;
    assert(r2_ears_discover(&d, NULL, NULL) == 0);
    assert(!d.microphone_source && !d.desktop_playing && d.playback_pid == -1);
    assert(!strcmp(d.desktop_monitor, "default.monitor"));
    r2_ears_discovery_clear(&d);
    /* Native monitor identity, not a name suffix, excludes this monitor. */
    fake_source_name = "physical.monitor"; fake_default_source = fake_source_name;
    fake_alt_source = "another-input";
    fake_playing = 1;
    assert(r2_ears_discover(&d, NULL, NULL) == 0);
    assert(!strcmp(d.microphone_source, fake_source_name));
    assert(d.desktop_playing && d.playback_pid == 3141);
    assert(!strcmp(d.desktop_monitor, fake_monitor_name));
    r2_ears_discovery_clear(&d);
    fake_default_source = "monitor-without-suffix";
    assert(r2_ears_discover(&d, NULL, NULL) == 0);
    assert(!strcmp(d.microphone_source, fake_alt_source));
    r2_ears_discovery_clear(&d);
    /* Preserve arbitrary full server strings beyond the old 512/1024 caps. */
    char long_source[9000], long_monitor[11000];
    memset(long_source, 's', sizeof(long_source) - 1); long_source[sizeof(long_source) - 1] = 0;
    memset(long_monitor, 'm', sizeof(long_monitor) - 1); long_monitor[sizeof(long_monitor) - 1] = 0;
    fake_source_name = fake_default_source = long_source; fake_monitor_name = long_monitor;
    fake_first_anonymous = 1;
    assert(r2_ears_discover(&d, NULL, NULL) == 0);
    assert(!strcmp(d.microphone_source, long_source));
    assert(!strcmp(d.desktop_monitor, long_monitor) && d.playback_pid == 3141);
    r2_ears_discovery_clear(&d);
    fake_first_anonymous = 0;
    const char *bad_pids[] = {"3141x", "3141 ", "-42", "1", "9999999999999999999999999999", NULL};
    for (size_t i = 0; i < sizeof(bad_pids)/sizeof(*bad_pids); ++i) {
        fake_pid = bad_pids[i];
        assert(r2_ears_discover(&d, NULL, NULL) == 0);
        assert(d.desktop_playing && d.playback_pid == -1);
        assert(!strcmp(d.desktop_monitor, long_monitor));
        r2_ears_discovery_clear(&d);
    }
    fake_pid = "3141";
    for (int stage = 1; stage <= 4; ++stage) {
        fake_error_stage = stage;
        assert(r2_ears_discover(&d, NULL, NULL) == -1 && errno == EIO);
        check_empty(&d);
        fake_error_stage = 0; fake_stall_stage = stage; fake_last_stage = 0;
        int calls = 0;
        assert(r2_ears_discover(&d, cancel_query, &calls) == -1 && errno == ECANCELED);
        check_empty(&d);
        fake_stall_stage = 0;
    }
    assert(fake_cancelled_operations == 4);
    fake_sink_missing = 1;
    assert(r2_ears_discover(&d, NULL, NULL) == -1);
    check_empty(&d); fake_sink_missing = 0;
    connecting = 1;
    int calls = 0;
    assert(r2_ears_discover(&d, cancel_query, &calls) == -1 && errno == ECANCELED);
    check_empty(&d); connecting = 0;
    assert(open_count == 0 && close_count == 0);
    assert(r2_ears_microphone_available() == 1);
    assert(r2_ears_desktop_playing() == 1 && r2_ears_playback_pid() == 3141);
    /* Explicit inputs remain independent of unrelated discovery failures. */
    fake_error_stage = 3;
    assert(r2_ears_microphone_available() == 1);
    fake_error_stage = 2;
    assert(r2_ears_desktop_playing() == 1 && r2_ears_playback_pid() == 3141);
    fake_error_stage = 0;
    assert(contexts == 0 && fake_operations == 0);
    puts("Native discovery: full names, actual routing, PID identity, cancellation and query failure passed.");
    return 0;
}
