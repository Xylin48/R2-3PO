#ifndef R2_EARS_NATIVE_TEST_ENDPOINTS_H
#define R2_EARS_NATIVE_TEST_ENDPOINTS_H
#include <pulse/pulseaudio.h>
#include <stdatomic.h>
#include <assert.h>
typedef struct {
    unsigned char data[2500];
    int paused, hole, failed;
    atomic_int peeks;
} FakePulse;
static int open_count = 0, close_count = 0, contexts = 0;
static int connecting = 0;
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
    return (pa_stream *)last_stream;
}
int pa_stream_connect_record(pa_stream *s, const char *dev,
                              const pa_buffer_attr *attr, pa_stream_flags_t flags)
{
    (void)attr; (void)flags;
    assert(dev && (strstr(dev, "test_mic") || strstr(dev, "monitor")));
    FakePulse *p = (FakePulse *)s;
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
int pa_stream_drop(pa_stream *s) { (void)s; return 0; }
int pa_stream_disconnect(pa_stream *s) { (void)s; return 0; }
void pa_stream_unref(pa_stream *s) { ++close_count; free(s); }

#endif
