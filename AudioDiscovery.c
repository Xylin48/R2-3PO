#define _POSIX_C_SOURCE 200809L
/*
 * R2's read-only PulseAudio / PipeWire discovery.
 * Query native introspection rather than parsing pactl output. A single
 * context provides a consistent-enough snapshot of default devices, inputs
 * and the sink monitor attached to the selected playback stream.
 *
 * A transient failure is never translated into "device unplugged".
 */
#include "AudioDiscovery.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pulse/context.h>
#include <pulse/def.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/proplist.h>

#define R2_DISCOVERY_DEADLINE_MS 2500ULL

typedef struct {
    pa_mainloop *loop;
    pa_context *context;
    uint64_t deadline_ms;
    int (*cancelled)(void *);
    void *userdata;
    int done;
    int failed;
    char *default_source;
    char *fallback_microphone;
    R2AudioDiscovery data;
    size_t producer_capacity;
    size_t resolving_output_slot; /* SIZE_MAX for default-only monitor */
} R2PulseScan;

static uint64_t r2_discovery_now_ms(void)
{
    struct timespec stamp;
    if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return UINT64_MAX;
    return (uint64_t)stamp.tv_sec * 1000U +
           (uint64_t)stamp.tv_nsec / 1000000U;
}

static char *r2_discovery_copy(const char *s)
{
    return s && *s ? strdup(s) : NULL;
}

void r2_audio_discovery_free(R2AudioDiscovery *result)
{
    if (!result) return;
    free(result->microphone_source);
    free(result->desktop_monitor);
    free(result->default_sink);
    for (size_t i = 0; i < result->playback_output_count; ++i)
        free(result->playback_outputs[i].monitor);
    free(result->playback_outputs);
    free(result->playback_producers);
    memset(result, 0, sizeof(*result));
    result->playback_pid = -1;
    result->playback_sink = PA_INVALID_INDEX;
}

static void r2_discovery_server_info(pa_context *ctx,
                                      const pa_server_info *info, void *userdata)
{
    (void)ctx;
    R2PulseScan *scan = userdata;
    if (!info) { scan->failed = 1; scan->done = 1; return; }
    scan->default_source = r2_discovery_copy(info->default_source_name);
    scan->data.default_sink = r2_discovery_copy(info->default_sink_name);
    if ((info->default_source_name && *info->default_source_name && !scan->default_source) ||
        (info->default_sink_name && *info->default_sink_name && !scan->data.default_sink))
        scan->failed = 1;
    scan->done = 1;
}

static void r2_discovery_source_info(pa_context *ctx, const pa_source_info *info,
                                      int eol, void *userdata)
{
    (void)ctx;
    R2PulseScan *scan = userdata;
    if (eol != 0) {
        if (eol < 0) scan->failed = 1;
        scan->done = 1;
        return;
    }
    if (!info || !info->name || !*info->name ||
        info->monitor_of_sink != PA_INVALID_INDEX)
        return;
    if (!scan->fallback_microphone) {
        scan->fallback_microphone = strdup(info->name);
        if (!scan->fallback_microphone) scan->failed = 1;
    }
    if (scan->default_source && !strcmp(scan->default_source, info->name)) {
        free(scan->data.microphone_source);
        scan->data.microphone_source = strdup(info->name);
        if (!scan->data.microphone_source) scan->failed = 1;
    }
}

static long r2_discovery_pid(const char *source)
{
    if (!source || !*source) return -1;
    for (const char *p = source; *p; ++p)
        if (*p < '0' || *p > '9') return -1;
    errno = 0;
    char *end = NULL;
    long value = strtol(source, &end, 10);
    return errno == 0 && end && *end == '\0' && value > 1 && value <= INT_MAX
        ? value : -1;
}

static int r2_discovery_add_producer(R2PulseScan *scan, long pid)
{
    if (pid <= 1) return 0;
    R2AudioDiscovery *data = &scan->data;
    for (size_t i = 0; i < data->playback_producer_count; ++i)
        if (data->playback_producers[i] == pid) return 0;
    if (data->playback_producer_count == scan->producer_capacity) {
        const size_t limit = SIZE_MAX / sizeof(*data->playback_producers);
        if (scan->producer_capacity == limit) { errno = EOVERFLOW; return -1; }
        size_t capacity = scan->producer_capacity;
        capacity = !capacity ? 8 : capacity > limit / 2 ? limit : capacity * 2;
        long *grown = realloc(data->playback_producers, capacity * sizeof(*grown));
        if (!grown) return -1;
        data->playback_producers = grown;
        scan->producer_capacity = capacity;
    }
    data->playback_producers[data->playback_producer_count++] = pid;
    return 0;
}

/* Snapshot every distinct output sink with live, uncorked playback.
 * Multiple applications on the same sink share its monitor; different
 * sinks never collapse into one audio stream. */
static void r2_discovery_sink_input(pa_context *ctx,
                                     const pa_sink_input_info *info,
                                     int eol, void *userdata)
{
    (void)ctx;
    R2PulseScan *scan = userdata;
    if (eol != 0) {
        if (eol < 0) scan->failed = 1;
        scan->done = 1;
        return;
    }
    if (scan->failed || !info || info->corked || info->sink == PA_INVALID_INDEX)
        return;
    const char *pid = info->proplist ?
        pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_ID) : NULL;
    long producer = r2_discovery_pid(pid);
    if (r2_discovery_add_producer(scan, producer) != 0) {
        scan->failed = 1;
        return;
    }
    for (size_t i = 0; i < scan->data.playback_output_count; ++i)
        if (scan->data.playback_outputs[i].sink_index == info->sink)
            return;
    if (scan->data.playback_output_count == SIZE_MAX ||
        scan->data.playback_output_count >= SIZE_MAX / sizeof(R2AudioPlaybackOutput)) {
        scan->failed = 1;
        return;
    }
    size_t next = scan->data.playback_output_count + 1;
    R2AudioPlaybackOutput *grown = realloc(
        scan->data.playback_outputs, next * sizeof(*grown));
    if (!grown) { scan->failed = 1; return; }
    scan->data.playback_outputs = grown;
    R2AudioPlaybackOutput *output = &grown[scan->data.playback_output_count];
    *output = (R2AudioPlaybackOutput){
        .monitor = NULL,
        .sink_index = info->sink,
        .playback_pid = producer
    };
    scan->data.playback_output_count = next;
    if (!scan->data.desktop_playing) {
        scan->data.desktop_playing = 1;
        scan->data.playback_sink = info->sink;
        scan->data.playback_pid = output->playback_pid;
    }
}

static void r2_discovery_sink_info(pa_context *ctx, const pa_sink_info *info,
                                    int eol, void *userdata)
{
    (void)ctx;
    R2PulseScan *scan = userdata;
    if (eol != 0) {
        if (eol < 0) scan->failed = 1;
        scan->done = 1;
        return;
    }
    if (!info || !info->monitor_source_name || !*info->monitor_source_name) {
        scan->failed = 1;
        return;
    }
    if (scan->resolving_output_slot != SIZE_MAX) {
        if (scan->resolving_output_slot >= scan->data.playback_output_count ||
            scan->data.playback_outputs[scan->resolving_output_slot].sink_index != info->index) {
            scan->failed = 1; /* route changed during introspection */
            return;
        }
        R2AudioPlaybackOutput *output =
            &scan->data.playback_outputs[scan->resolving_output_slot];
        if (!output->monitor) {
            output->monitor = strdup(info->monitor_source_name);
            if (!output->monitor) scan->failed = 1;
        }
    }
    if (!scan->data.desktop_monitor && scan->resolving_output_slot == 0) {
        scan->data.desktop_monitor = strdup(info->monitor_source_name);
        if (!scan->data.desktop_monitor) scan->failed = 1;
    } else if (!scan->data.desktop_monitor && scan->resolving_output_slot == SIZE_MAX) {
        scan->data.desktop_monitor = strdup(info->monitor_source_name);
        if (!scan->data.desktop_monitor) scan->failed = 1;
    }
}

/* A finite nonblocking pump: do not let a disconnected or wedged server
 * hold Watch's discovery thread forever. */
static int r2_discovery_pump(R2PulseScan *scan, int *done)
{
    while (!*done) {
        if (scan->cancelled && scan->cancelled(scan->userdata)) {
            errno = ECANCELED;
            return -1;
        }
        if (r2_discovery_now_ms() >= scan->deadline_ms) {
            errno = ETIMEDOUT;
            return -1;
        }
        pa_context_state_t state = pa_context_get_state(scan->context);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            errno = ENOTCONN;
            return -1;
        }
        if (pa_mainloop_iterate(scan->loop, 0, NULL) < 0) {
            errno = EIO;
            return -1;
        }
        if (!*done) {
            struct timespec sleep_time = {0, 5000000L};
            while (nanosleep(&sleep_time, &sleep_time) != 0 && errno == EINTR) {}
        }
    }
    return scan->failed ? -1 : 0;
}

static int r2_discovery_run(R2PulseScan *scan, pa_operation *operation)
{
    if (!operation) return -1;
    int result = r2_discovery_pump(scan, &scan->done);
    pa_operation_unref(operation);
    return result;
}

int r2_audio_discover_cancelable(R2AudioDiscovery *result,
                                  int (*cancelled)(void *), void *userdata)
{
    if (!result) { errno = EINVAL; return -1; }
    memset(result, 0, sizeof(*result));
    result->playback_pid = -1;
    result->playback_sink = PA_INVALID_INDEX;

    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
    R2PulseScan scan = {.cancelled = cancelled, .userdata = userdata};
    scan.data.playback_pid = -1;
    scan.data.playback_sink = PA_INVALID_INDEX;
    scan.resolving_output_slot = SIZE_MAX;
    uint64_t started = r2_discovery_now_ms();
    if (started == UINT64_MAX || started > UINT64_MAX - R2_DISCOVERY_DEADLINE_MS)
        return -1;
    scan.deadline_ms = started + R2_DISCOVERY_DEADLINE_MS;
    scan.loop = pa_mainloop_new();
    if (!scan.loop) goto fail;
    scan.context = pa_context_new(pa_mainloop_get_api(scan.loop), "R2-3PO source discovery");
    if (!scan.context || pa_context_connect(scan.context, NULL,
                                            PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto fail;
    int ready = 0;
    while (!ready) {
        if (cancelled && cancelled(userdata)) { errno = ECANCELED; goto fail; }
        pa_context_state_t state = pa_context_get_state(scan.context);
        if (state == PA_CONTEXT_READY) ready = 1;
        else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
            goto fail;
        else if (r2_discovery_now_ms() >= scan.deadline_ms ||
                 pa_mainloop_iterate(scan.loop, 0, NULL) < 0)
            goto fail;
        else {
            struct timespec pause = {0, 5000000L};
            while (nanosleep(&pause, &pause) != 0 && errno == EINTR) {}
        }
    }

    scan.done = scan.failed = 0;
    if (r2_discovery_run(&scan, pa_context_get_server_info(
            scan.context, r2_discovery_server_info, &scan)) < 0) goto fail;
    scan.done = scan.failed = 0;
    if (r2_discovery_run(&scan, pa_context_get_source_info_list(
            scan.context, r2_discovery_source_info, &scan)) < 0) goto fail;
    if (!scan.data.microphone_source) {
        scan.data.microphone_source = scan.fallback_microphone;
        scan.fallback_microphone = NULL;
    }

    scan.done = scan.failed = 0;
    if (r2_discovery_run(&scan, pa_context_get_sink_input_info_list(
            scan.context, r2_discovery_sink_input, &scan)) < 0) goto fail;
    /* Resolve every distinct active sink's actual monitor. Keep the
     * first-selected monitor as the legacy desktop listener for existing
     * explicit attention commands, without hiding other simultaneously
     * audible outputs. No hard device-count cap is imposed. */
    for (size_t i = 0; i < scan.data.playback_output_count; ++i) {
        scan.done = scan.failed = 0;
        scan.resolving_output_slot = i;
        pa_operation *op = pa_context_get_sink_info_by_index(
            scan.context, scan.data.playback_outputs[i].sink_index,
            r2_discovery_sink_info, &scan);
        if (r2_discovery_run(&scan, op) < 0 ||
            !scan.data.playback_outputs[i].monitor) goto fail;
    }
    /* A manually requested desktop listener can still open the default
     * monitor when no app currently plays to an output sink. */
    if (!scan.data.desktop_playing && scan.data.default_sink) {
        scan.done = scan.failed = 0;
        scan.resolving_output_slot = SIZE_MAX;
        if (r2_discovery_run(&scan,
            pa_context_get_sink_info_by_name(scan.context, scan.data.default_sink,
                                              r2_discovery_sink_info, &scan)) < 0 ||
            !scan.data.desktop_monitor) goto fail;
    }

    if (cancelled && cancelled(userdata)) { errno = ECANCELED; goto fail; }
    pa_context_disconnect(scan.context);
    pa_context_unref(scan.context);
    pa_mainloop_free(scan.loop);
    free(scan.default_source);
    free(scan.fallback_microphone);
    *result = scan.data;
    return 0;

fail:
    if (scan.context) {
        pa_context_disconnect(scan.context);
        pa_context_unref(scan.context);
    }
    if (scan.loop) pa_mainloop_free(scan.loop);
    free(scan.default_source);
    free(scan.fallback_microphone);
    r2_audio_discovery_free(&scan.data);
    errno = errno ? errno : EIO;
    return -1;
}

int r2_audio_discover(R2AudioDiscovery *result)
{
    return r2_audio_discover_cancelable(result, NULL, NULL);
}
