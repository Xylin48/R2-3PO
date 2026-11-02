#ifndef R2_AUDIO_DISCOVERY_H
#define R2_AUDIO_DISCOVERY_H

#include <stdint.h>

/*
 * One coherent read of the current PulseAudio/PipeWire server.
 * Query failures are different from a successful query with no sources.
 * All returned strings are owned by the caller and must be freed.
 */
typedef struct {
    char *microphone_source;
    char *desktop_monitor;
    char *default_sink;
    long playback_pid;       /* -1 if the active playback has no process ID. */
    uint32_t playback_sink;  /* PA_INVALID_INDEX when no playback is active. */
    int desktop_playing;     /* Uncorked sink input exists. */
} R2AudioDiscovery;

int r2_audio_discover(R2AudioDiscovery *result);
void r2_audio_discovery_free(R2AudioDiscovery *result);

#endif
