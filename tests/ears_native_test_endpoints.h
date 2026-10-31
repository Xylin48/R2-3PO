#ifndef R2_EARS_NATIVE_TEST_ENDPOINTS_H
#define R2_EARS_NATIVE_TEST_ENDPOINTS_H
#include <pulse/pulseaudio.h>
#include <stdatomic.h>
#include <assert.h>
typedef struct {
    unsigned char data[2500];
    int paused, hole, failed;
    char *device;
    atomic_int peeks;
} FakePulse;
static int open_count = 0, close_count = 0, contexts = 0;
static int connecting = 0, fake_pause_new = 0;
static FakePulse *last_stream;

/* Actual Ears code and libpulse mainloop, controlled native device endpoints. */
pa_context *pa_context_new(pa_mainloop_api *api, const char *name)
{ (void)api; (void)name; ++contexts; return (pa_context *)calloc(1, 1); }
int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags,
                       const pa_spawn_api *api)
{ (void)c; (void)server; (void)flags; (void)api; return 0; }
pa_context_state_t pa_context_get_state(const pa_context *c)
{ (void)c; return connecting ? PA_CONTEXT_CONNECTING : PA_CONTEXT_READY; }
void pa_context_disconnect(pa_context *c) { (void)c; }
void pa_context_unref(pa_context *c) { --contexts; free(c); }
int pa_context_errno(const pa_context *c) { (void)c; return PA_ERR_IO; }
pa_stream *pa_stream_new(pa_context *c, const char *name,
                         const pa_sample_spec *ss, const pa_channel_map *map)
{
    (void)c; (void)name; (void)map;
    assert(ss->rate == 48000 && ss->channels == 2);
    last_stream = calloc(1, sizeof(*last_stream));
    assert(last_stream); ++open_count;
    atomic_init(&last_stream->peeks, 0);
    last_stream->paused = fake_pause_new;
    return (pa_stream *)last_stream;
}
int pa_stream_connect_record(pa_stream *s, const char *dev,
                              const pa_buffer_attr *attr, pa_stream_flags_t flags)
{
    (void)attr; (void)flags;
    if (!dev) dev = "alsa_input.test_mic";
    FakePulse *p = (FakePulse *)s;
    p->device = strdup(dev); assert(p->device);
    memset(p->data, strstr(dev, "monitor") ? 0x5a : 0xa5, sizeof(p->data));
    return 0;
}
pa_stream_state_t pa_stream_get_state(const pa_stream *s)
{ return ((const FakePulse *)s)->failed ? PA_STREAM_FAILED : PA_STREAM_READY; }
int pa_stream_peek(pa_stream *s, const void **data, size_t *bytes)
{
    FakePulse *p = (FakePulse *)s;
    atomic_fetch_add(&p->peeks, 1);
    *bytes = p->paused ? 0 : sizeof(p->data);
    *data = p->hole || p->paused ? NULL : p->data;
    return 0;
}
const char *pa_stream_get_device_name(const pa_stream *s)
{ return ((const FakePulse *)s)->device; }
int pa_stream_drop(pa_stream *s) { (void)s; return 0; }
int pa_stream_disconnect(pa_stream *s) { (void)s; return 0; }
void pa_stream_unref(pa_stream *s) { ++close_count; free(((FakePulse *)s)->device); free(s); }

/* Native discovery responses: no capture is opened by these queries. */
static const char *fake_source_name = "alsa_input.test_mic";
static const char *fake_default_source = "alsa_input.test_mic";
static const char *fake_alt_source = NULL;
static const char *fake_monitor_name = "alsa_output.test.monitor";
static const char *fake_pid = "3141";
static int fake_playing = 1, fake_sink_missing = 0, fake_first_anonymous = 0;
static int fake_stall_stage = 0, fake_error_stage = 0, fake_last_stage = 0;
static int fake_operations = 0, fake_cancelled_operations = 0;
typedef struct { pa_operation_state_t state; } FakeOperation;
static pa_operation *fake_operation(int stage)
{
    fake_last_stage = stage;
    FakeOperation *op = calloc(1, sizeof(*op));
    assert(op); ++fake_operations;
    op->state = stage == fake_stall_stage ? PA_OPERATION_RUNNING : PA_OPERATION_DONE;
    return (pa_operation *)op;
}
pa_operation_state_t pa_operation_get_state(const pa_operation *op)
{ return ((const FakeOperation *)op)->state; }
void pa_operation_cancel(pa_operation *op)
{ ((FakeOperation *)op)->state = PA_OPERATION_CANCELLED; ++fake_cancelled_operations; }
void pa_operation_unref(pa_operation *op) { --fake_operations; free(op); }
pa_operation *pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *data)
{
    pa_server_info info = { .default_source_name = fake_default_source,
                            .default_sink_name = "default-output" };
    if (fake_stall_stage != 1) cb(c, fake_error_stage == 1 ? NULL : &info, data);
    return fake_operation(1);
}
pa_operation *pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *data)
{
    if (fake_stall_stage != 2) {
        pa_source_info info = { .name = "monitor-without-suffix", .monitor_of_sink = 2 };
        cb(c, &info, 0, data);
        info.monitor_of_sink = PA_INVALID_INDEX;
        if (fake_alt_source) { info.name = fake_alt_source; cb(c, &info, 0, data); }
        if (fake_source_name) { info.name = fake_source_name; cb(c, &info, 0, data); }
        cb(c, NULL, fake_error_stage == 2 ? -1 : 1, data);
    }
    return fake_operation(2);
}
pa_operation *pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb, void *data)
{
    if (fake_stall_stage != 3) {
        pa_sink_input_info info = { .sink = 2, .corked = 0, .proplist = NULL };
        if (fake_playing && fake_first_anonymous) cb(c, &info, 0, data);
        info.sink = 7; info.corked = !fake_playing;
        info.proplist = pa_proplist_new(); assert(info.proplist);
        if (fake_pid) assert(pa_proplist_sets(info.proplist, PA_PROP_APPLICATION_PROCESS_ID, fake_pid) == 0);
        cb(c, &info, 0, data);
        info.sink = 9; info.corked = 1;
        assert(pa_proplist_sets(info.proplist, PA_PROP_APPLICATION_PROCESS_ID, "2718") == 0);
        cb(c, &info, 0, data);
        pa_proplist_free(info.proplist);
        cb(c, NULL, fake_error_stage == 3 ? -1 : 1, data);
    }
    return fake_operation(3);
}
pa_operation *pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *data)
{
    if (fake_stall_stage != 4) {
        pa_sink_info info = { .name = "default-output", .index = 2,
                              .monitor_source_name = "default.monitor" };
        cb(c, &info, 0, data);
        info.name = "actual-output"; info.index = 7;
        info.monitor_source_name = fake_monitor_name;
        if (!fake_sink_missing) cb(c, &info, 0, data);
        cb(c, NULL, fake_error_stage == 4 ? -1 : 1, data);
    }
    return fake_operation(4);
}

#endif
