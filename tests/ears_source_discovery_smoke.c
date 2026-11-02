/*
 * Native PulseAudio/PipeWire introspection regression. Call the actual
 * production callbacks with synthetic metadata; no hardware server needed.
 */
#define _POSIX_C_SOURCE 200809L
#include "../AudioDiscovery.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    R2PulseScan scan = {0};
    scan.data.playback_pid = -1;
    scan.data.playback_sink = PA_INVALID_INDEX;
    char preferred[1024] = "alsa_input.usb_microphone.";
    size_t length = strlen(preferred);
    memset(preferred + length, 'q', 700);
    preferred[length + 700] = '\0';

    pa_server_info server = {0};
    server.default_source_name = preferred;
    server.default_sink_name = "alsa_output.default";
    r2_discovery_server_info(NULL, &server, &scan);
    assert(scan.done && !scan.failed);
    assert(!strcmp(scan.default_source, preferred));
    assert(!strcmp(scan.data.default_sink, "alsa_output.default"));

    scan.done = 0;
    pa_source_info monitor = {0};
    monitor.name = "alsa_output.default.monitor";
    monitor.monitor_of_sink = 25;
    r2_discovery_source_info(NULL, &monitor, 0, &scan);
    assert(!scan.data.microphone_source && !scan.fallback_microphone);
    pa_source_info other = {0};
    other.name = "alsa_input.builtin";
    other.monitor_of_sink = PA_INVALID_INDEX;
    r2_discovery_source_info(NULL, &other, 0, &scan);
    assert(!strcmp(scan.fallback_microphone, "alsa_input.builtin"));
    pa_source_info selected = {0};
    selected.name = preferred;
    selected.monitor_of_sink = PA_INVALID_INDEX;
    r2_discovery_source_info(NULL, &selected, 0, &scan);
    r2_discovery_source_info(NULL, NULL, 1, &scan);
    assert(scan.done && !scan.failed);
    assert(!strcmp(scan.data.microphone_source, preferred));
    assert(strlen(scan.data.microphone_source) > 512);

    pa_proplist *properties = pa_proplist_new();
    assert(properties);
    assert(pa_proplist_sets(properties, PA_PROP_APPLICATION_PROCESS_ID, "3141") == 0);
    pa_sink_input_info paused = {0};
    paused.corked = 1;
    paused.sink = 11;
    paused.proplist = properties;
    scan.done = 0;
    r2_discovery_sink_input(NULL, &paused, 0, &scan);
    assert(!scan.data.desktop_playing && scan.data.playback_pid == -1);

    /* An uncorked stream on a nondefault sink must select that stream's
     * real sink, never append '.monitor' to the global default name. */
    pa_sink_input_info playback = {0};
    playback.corked = 0;
    playback.sink = 88;
    playback.proplist = properties;
    r2_discovery_sink_input(NULL, &playback, 0, &scan);
    /* Same sink, separate producer: one monitor capture, not duplicates. */
    r2_discovery_sink_input(NULL, &playback, 0, &scan);
    pa_sink_input_info secondary = {.sink = 99, .proplist = properties};
    r2_discovery_sink_input(NULL, &secondary, 0, &scan);
    r2_discovery_sink_input(NULL, &paused, 0, &scan);
    r2_discovery_sink_input(NULL, NULL, 1, &scan);
    assert(scan.done && !scan.failed);
    assert(scan.data.desktop_playing);
    assert(scan.data.playback_sink == 88);
    assert(scan.data.playback_pid == 3141);
    assert(scan.data.playback_output_count == 2);
    assert(scan.data.playback_outputs[0].sink_index == 88);
    assert(scan.data.playback_outputs[1].sink_index == 99);

    char routed_monitor[1200] = "alsa_output.routed.";
    size_t monitor_prefix = strlen(routed_monitor);
    memset(routed_monitor + monitor_prefix, 'm', 850);
    strcpy(routed_monitor + monitor_prefix + 850, ".monitor");
    scan.done = 0;
    scan.resolving_output_slot = 0;
    pa_sink_info output = {0};
    output.index = 88;
    output.name = "alsa_output.routed";
    output.monitor_source_name = routed_monitor;
    r2_discovery_sink_info(NULL, &output, 0, &scan);
    r2_discovery_sink_info(NULL, NULL, 1, &scan);
    assert(scan.done && !scan.failed);
    assert(!strcmp(scan.data.desktop_monitor, routed_monitor));
    assert(!strcmp(scan.data.playback_outputs[0].monitor, routed_monitor));

    /* The second sink must retain its own untruncated monitor identity. */
    char other_monitor[1200] = "alsa_output.secondary.";
    size_t other_prefix = strlen(other_monitor);
    memset(other_monitor + other_prefix, 'n', 900);
    strcpy(other_monitor + other_prefix + 900, ".monitor");
    output.index = 99;
    output.monitor_source_name = other_monitor;
    scan.resolving_output_slot = 1;
    scan.done = scan.failed = 0;
    r2_discovery_sink_info(NULL, &output, 0, &scan);
    r2_discovery_sink_info(NULL, NULL, 1, &scan);
    assert(scan.done && !scan.failed);
    assert(!strcmp(scan.data.playback_outputs[1].monitor, other_monitor));
    assert(strcmp(scan.data.playback_outputs[1].monitor,
                  scan.data.playback_outputs[0].monitor));
    assert(!strcmp(scan.data.desktop_monitor, routed_monitor));

    /* A stale sink index must fail discovery rather than silently misroute. */
    scan.done = scan.failed = 0;
    output.index = 100;
    r2_discovery_sink_info(NULL, &output, 0, &scan);
    assert(scan.failed);

    /* An API failure must never be reported as a successful empty list. */
    scan.done = scan.failed = 0;
    r2_discovery_source_info(NULL, NULL, -1, &scan);
    assert(scan.done && scan.failed);

    /* Invalid PID metadata is unknown, not guessed from a paused producer. */
    scan.data.desktop_playing = 0;
    scan.data.playback_pid = -1;
    assert(pa_proplist_sets(properties, PA_PROP_APPLICATION_PROCESS_ID, "3141xyz") == 0);
    scan.done = scan.failed = 0;
    r2_discovery_sink_input(NULL, &playback, 0, &scan);
    assert(scan.data.desktop_playing && scan.data.playback_pid == -1);
    assert(scan.data.playback_output_count == 3);

    pa_proplist_free(properties);
    free(scan.default_source);
    free(scan.fallback_microphone);
    r2_audio_discovery_free(&scan.data);
    puts("Native discovery keeps untruncated microphones and every distinct routed playback sink, with failure propagation.");
    return 0;
}
