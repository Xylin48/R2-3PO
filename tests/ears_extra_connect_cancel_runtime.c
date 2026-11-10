/* Real core + Ears ownership with a deliberately stalled transport handshake. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include "../AudioCapture.h"
#include <assert.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int pause_before_open, open_entered, release_open;
static int connect_entered, transport_cancelled, connect_calls;
static int connect_succeeds, read_entered;
static atomic_int owner_cancelled = ATOMIC_VAR_INIT(0);
static R2AudioPlaybackOutput outputs[] = {
    {.monitor = "test.first.monitor"}, {.monitor = "test.second.monitor"},
    {.monitor = "test.third.monitor"}
};
static R2AudioDiscovery snapshot = {
    .playback_outputs = outputs, .playback_output_count = 3
};

static struct timespec deadline(void)
{
    struct timespec until;
    assert(clock_gettime(CLOCK_REALTIME, &until) == 0);
    until.tv_sec += 3;
    return until;
}

static void await_flag(int *flag)
{
    struct timespec until = deadline();
    while (!*flag)
        assert(pthread_cond_timedwait(&changed, &gate, &until) == 0);
}

R2AudioCapture *__real_r2_audio_capture_new(void);
R2AudioCapture *__wrap_r2_audio_capture_new(void)
{
    pthread_mutex_lock(&gate);
    transport_cancelled = 0;
    pthread_mutex_unlock(&gate);
    return __real_r2_audio_capture_new();
}

void __real_r2_audio_capture_cancel(R2AudioCapture *capture);
void __wrap_r2_audio_capture_cancel(R2AudioCapture *capture)
{
    __real_r2_audio_capture_cancel(capture);
    pthread_mutex_lock(&gate);
    transport_cancelled = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
}

int __wrap_r2_audio_capture_connect(R2AudioCapture *capture, const char *source,
                                    const pa_sample_spec *format, size_t bytes)
{
    assert(capture && source && format && bytes == 1920);
    pthread_mutex_lock(&gate);
    ++connect_calls;
    connect_entered = 1;
    pthread_cond_broadcast(&changed);
    if (connect_succeeds) {
        pthread_mutex_unlock(&gate);
        return 0;
    }
    /* Never finishes on its own: stop must reach the real Ears transport. */
    await_flag(&transport_cancelled);
    pthread_mutex_unlock(&gate);
    errno = ECANCELED;
    return -1;
}

ssize_t __wrap_r2_audio_capture_read(R2AudioCapture *capture, void *pcm, size_t bytes)
{
    assert(capture && pcm && bytes);
    pthread_mutex_lock(&gate);
    read_entered = 1;
    pthread_cond_broadcast(&changed);
    await_flag(&transport_cancelled);
    pthread_mutex_unlock(&gate);
    errno = ECANCELED;
    return -1;
}
int __wrap_r2_audio_discover_cancelable(R2AudioDiscovery *audio,
                                       int (*cancelled)(void *), void *userdata)
{
    memset(audio, 0, sizeof(*audio));
    if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
    audio->microphone_source = strdup("test.direct.mic");
    assert(audio->microphone_source);
    return 0;
}
static void *direct_listen(void *unused)
{
    (void)unused;
    assert(r2_ears_listen_cancelable(15, NULL, NULL) == NULL);
    return NULL;
}

int __real_r2_ears_open_desktop_source_cancelable(R2Ears *, const char *,
                                                 int (*)(void *), void *);
int __wrap_r2_ears_open_desktop_source_cancelable(R2Ears *ears, const char *source,
                                                  int (*cancelled)(void *), void *userdata)
{
    pthread_mutex_lock(&gate);
    open_entered = 1;
    pthread_cond_broadcast(&changed);
    if (pause_before_open) await_flag(&release_open);
    pthread_mutex_unlock(&gate);
    return __real_r2_ears_open_desktop_source_cancelable(ears, source, cancelled, userdata);
}

int __real_r2_ears_open_source_cancelable(R2Ears *, const char *, int (*)(void *), void *);
int __wrap_r2_ears_open_source_cancelable(R2Ears *receiver, const char *source,
                                         int (*cancelled)(void *), void *userdata)
{
    pthread_mutex_lock(&gate);
    open_entered = 1;
    pthread_cond_broadcast(&changed);
    if (pause_before_open) await_flag(&release_open);
    pthread_mutex_unlock(&gate);
    return __real_r2_ears_open_source_cancelable(receiver, source, cancelled, userdata);
}

static int owner_stopped(void *unused)
{
    (void)unused;
    return atomic_load(&owner_cancelled);
}

static void *reconcile(void *unused)
{
    (void)unused;
    assert(r2_ears_extra_reconcile_cancelable(&snapshot, NULL, owner_stopped, NULL) < 0);
    return NULL;
}

static void *stop_all(void *unused)
{
    (void)unused;
    r2_ears_extra_stop_all();
    return NULL;
}

static int primary_desktop, primary_watch, stop_mode;
static void *open_primary(void *unused)
{
    (void)unused;
    if (primary_watch) {
        uint64_t token = primary_desktop
            ? r2_ears_start_desktop_for_watch_cancelable("test.primary.monitor", owner_stopped, NULL)
            : r2_ears_start_for_watch_cancelable("test.primary.mic", owner_stopped, NULL);
        assert(token == 0);
    } else {
        /* Exact source bypasses hardware discovery, but uses the real core. */
        assert((primary_desktop
            ? r2_ears_start_desktop_impl(NULL, "test.primary.monitor", NULL, NULL)
            : r2_ears_start_input(NULL, "test.primary.mic", NULL, NULL)) < 0);
    }
    return NULL;
}

static void *stop_primary(void *unused)
{
    (void)unused;
    if (stop_mode == 1) {
        atomic_store(&owner_cancelled, 1);
        assert(r2_watch_stop() == 0);
    } else if (stop_mode == 2) r2_request_shutdown();
    else assert((primary_desktop ? r2_ears_stop_desktop() : r2_ears_stop()) == 0);
    return NULL;
}

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(void)
{
    /* A stop between a completed handshake and worker publication must
     * invalidate both primary routes before a worker or token can appear. */
    for (int desktop = 0; desktop <= 1; ++desktop) {
        R2EarsOpenRequest completed = r2_ears_open_request(desktop, 1, NULL, NULL);
        r2_ears_cancel_pending_inputs(1);
        uint64_t token = 0;
        assert(r2_ears_publish_capture(&completed, &token) == -1 && errno == ECANCELED);
        assert(!token && !ears_capture_thread_started && !ears_desktop_capture_thread_started);
        assert(!atomic_load(&ears_capture_running) && !atomic_load(&ears_desktop_capture_running));
    }

    /* Both an already-blocked connect and a stop before Ears resets its
     * reusable state must cancel the first open and skip the other outputs. */
    for (int before = 0; before <= 1; ++before) {
        pause_before_open = before;
        open_entered = release_open = connect_entered = transport_cancelled = connect_calls = 0;
        pthread_t opening, stopping;
        uint_fast64_t epoch = atomic_load(&extra_desktop_stop_epoch);
        assert(pthread_create(&opening, NULL, reconcile, NULL) == 0);
        pthread_mutex_lock(&gate);
        await_flag(before ? &open_entered : &connect_entered);
        pthread_mutex_unlock(&gate);
        double start = seconds();
        assert(pthread_create(&stopping, NULL, stop_all, NULL) == 0);
        if (before) {
            /* Wait until stop has cancelled the unpublished transport. */
            struct timespec pause = {0, 1000000L};
            while (atomic_load(&extra_desktop_stop_epoch) == epoch) {
                assert(seconds() - start < 1.0);
                nanosleep(&pause, NULL);
            }
            pthread_mutex_lock(&gate);
            release_open = 1;
            pthread_cond_broadcast(&changed);
            pthread_mutex_unlock(&gate);
        }
        assert(pthread_join(stopping, NULL) == 0);
        assert(pthread_join(opening, NULL) == 0);
        assert(seconds() - start < 1.0);
        assert(connect_calls == 1);
        assert(!extra_desktop_pending && !extra_desktop_head);
        assert(r2_ears_extra_source_count() == 0);
    }

    /* Watch signals cancellation before joining its coordinator. */
    pause_before_open = 0;
    connect_entered = transport_cancelled = connect_calls = 0;
    pthread_t opening;
    assert(pthread_create(&opening, NULL, reconcile, NULL) == 0);
    pthread_mutex_lock(&gate);
    await_flag(&connect_entered);
    pthread_mutex_unlock(&gate);
    atomic_store(&owner_cancelled, 1);
    double start = seconds();
    assert(r2_watch_stop() == 0);
    assert(pthread_join(opening, NULL) == 0);
    assert(seconds() - start < 1.0 && connect_calls == 1);
    /* A stop preceding reconciliation must also prevent a late open. */
    assert(r2_ears_extra_reconcile_cancelable(&snapshot, NULL, owner_stopped, NULL) < 0);
    assert(connect_calls == 1);
    r2_ears_extra_stop_all();
    assert(!extra_desktop_pending && !extra_desktop_head);

    /* Core shutdown also wakes a pending connect before lifecycle joins. */
    atomic_store(&owner_cancelled, 0);
    connect_entered = transport_cancelled = connect_calls = 0;
    assert(pthread_create(&opening, NULL, reconcile, NULL) == 0);
    pthread_mutex_lock(&gate);
    await_flag(&connect_entered);
    pthread_mutex_unlock(&gate);
    start = seconds();
    r2_request_shutdown();
    assert(pthread_join(opening, NULL) == 0);
    assert(seconds() - start < 1.0 && connect_calls == 1);
    assert(!extra_desktop_pending && !extra_desktop_head);
    atomic_store(&shutting_down, 0);
    assert(r2_ears_init(&ears) == 0);
    for (primary_desktop = 0; primary_desktop <= 1; ++primary_desktop) {
        for (stop_mode = 0; stop_mode <= 2; ++stop_mode) {
            for (int before = 0; before <= 1; ++before) {
                primary_watch = stop_mode == 1;
                atomic_store(&owner_cancelled, 0);
                pause_before_open = before;
                open_entered = release_open = connect_entered = transport_cancelled = connect_calls = 0;
                uint_fast64_t epoch = atomic_load(&ears_open_epoch[primary_desktop]);
                assert(pthread_create(&opening, NULL, open_primary, NULL) == 0);
                pthread_mutex_lock(&gate);
                await_flag(before ? &open_entered : &connect_entered);
                pthread_mutex_unlock(&gate);
                start = seconds();
                pthread_t stopping;
                assert(pthread_create(&stopping, NULL, stop_primary, NULL) == 0);
                if (before) {
                    struct timespec pause = {0, 1000000L};
                    while (!(stop_mode == 1 ? atomic_load(&owner_cancelled) :
                             stop_mode == 2 ? atomic_load(&shutting_down) :
                             atomic_load(&ears_open_epoch[primary_desktop]) != epoch)) {
                        assert(seconds() - start < 1.0);
                        nanosleep(&pause, NULL);
                    }
                    pthread_mutex_lock(&gate);
                    release_open = 1;
                    pthread_cond_broadcast(&changed);
                    pthread_mutex_unlock(&gate);
                }
                assert(pthread_join(stopping, NULL) == 0);
                assert(pthread_join(opening, NULL) == 0);
                assert(seconds() - start < 1.0 && connect_calls == 1);
                assert(!ears_pending_open[primary_desktop]);
                assert(!ears_capture_thread_started && !ears_desktop_capture_thread_started);
                atomic_store(&shutting_down, 0);
            }
        }
    }
    /* Watch stop has no custody over an explicit manual pending open. */
    primary_desktop = primary_watch = pause_before_open = 0;
    connect_entered = transport_cancelled = connect_calls = 0;
    assert(pthread_create(&opening, NULL, open_primary, NULL) == 0);
    pthread_mutex_lock(&gate);
    await_flag(&connect_entered);
    pthread_mutex_unlock(&gate);
    assert(r2_watch_stop() == 0);
    pthread_mutex_lock(&gate);
    assert(!transport_cancelled);
    pthread_mutex_unlock(&gate);
    assert(r2_ears_stop() == 0);
    assert(pthread_join(opening, NULL) == 0);
    /* Shutdown reaches an already blocked direct one-shot PCM read. */
    connect_succeeds = 1;
    transport_cancelled = read_entered = 0;
    char model_path[] = "/tmp/r2-shutdown-model-XXXXXX";
    int model_fd = mkstemp(model_path);
    assert(model_fd >= 0 && close(model_fd) == 0);
    assert(setenv("R2_WHISPER_MODEL", model_path, 1) == 0);
    atomic_store(&owner_cancelled, 0);
    assert(pthread_create(&opening, NULL, direct_listen, NULL) == 0);
    pthread_mutex_lock(&gate);
    await_flag(&read_entered);
    pthread_mutex_unlock(&gate);
    start = seconds();
    r2_request_shutdown();
    assert(pthread_join(opening, NULL) == 0);
    assert(seconds() - start < 1.0 && !ears_pending_open[0]);
    assert(unlink(model_path) == 0);
    atomic_store(&shutting_down, 0);
    r2_ears_shutdown(ears);
    ears = NULL;
    r2_ears_shutdown(ears_desktop);
    ears_desktop = NULL;
    puts("Extra/mic/desktop opens cancel across reset, Watch and shutdown; manual custody remains independent.");
    return 0;
}
