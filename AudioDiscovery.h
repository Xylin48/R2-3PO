#ifndef R2_AUDIO_DISCOVERY_H
#define R2_AUDIO_DISCOVERY_H

#include <stdint.h>
#include <stddef.h>

/*
 * One coherent read of the current PulseAudio/PipeWire server.
 * Query failures are different from a successful query with no sources.
 * All returned strings are owned by the caller and must be freed.
 */
/* Every distinct active playback sink, not one entry per application. A sink
 * monitor already mixes the applications routed to that physical output;
 * monitors on different outputs must remain distinct. */
typedef struct {
    char *monitor;
    uint32_t sink_index;
    long playback_pid; /* representative producer PID, or -1 */
} R2AudioPlaybackOutput;

typedef struct {
    char *microphone_source;
    char *desktop_monitor;
    char *default_sink;
    long playback_pid;       /* -1 if the active playback has no process ID. */
    uint32_t playback_sink;  /* PA_INVALID_INDEX when no playback is active. */
    int desktop_playing;     /* Uncorked sink input exists. */
    R2AudioPlaybackOutput *playback_outputs; /* distinct active outputs */
    size_t playback_output_count; /* no arbitrary device count quota */
} R2AudioDiscovery;

int r2_audio_discover(R2AudioDiscovery *result);
/* Predicate is polled during the nonblocking native query; no device opens. */
int r2_audio_discover_cancelable(R2AudioDiscovery *result,
                                  int (*cancelled)(void *), void *userdata);
void r2_audio_discovery_free(R2AudioDiscovery *result);

#endif
