/* Exercise the real Watch reconciliation function at the device API boundary. */
#include "../shell.c"
#include <assert.h>

static uint64_t active_token, next_token = 1;
static unsigned long active_window, vlc_window, pid_window;
static int failed_visual_probe;
static unsigned opens, stops;
static int active_vlc, eyes_allowed = 1, manual_active;
static unsigned long publish_on_open;

int r2_is_shutting_down(void) { return 0; }
int r2_sense_auto_allowed(int which) { assert(which == 1); return eyes_allowed; }
int r2_vision_available(void) { return 1; }
int r2_vision_media_status(void) { return active_token || manual_active; }
int r2_vision_window_available_cancelable(unsigned long window,
                                          int (*cancelled)(void *), void *userdata)
{
    return !cancelled(userdata) && window != 0;
}
int r2_vision_select_playback_window(const long *producers, size_t count,
                                    long *pid, unsigned long *window,
                                    int (*cancelled)(void *), void *userdata)
{
    assert(cancelled && !cancelled(userdata));
    if (failed_visual_probe) return -1;
    if (*pid > 1) assert(*pid == 4321 && *window == pid_window);
    *pid = -1;
    *window = vlc_window;
    for (size_t i = 0; i < count; ++i)
        if (producers[i] == 4321) {
            *pid = producers[i];
            *window = pid_window;
        }
    return 0;
}
int r2_vision_media_watch_owns(uint64_t token) { return token && token == active_token; }

static uint64_t open_window(unsigned long window, int vlc)
{
    assert(!r2_vision_media_status() && window);
    active_window = window;
    active_vlc = vlc;
    ++opens;
    if (publish_on_open) tv_publish_video_window(publish_on_open, NULL);
    return active_token = next_token++;
}

uint64_t r2_vision_media_open_x11_for_watch_cancelable(unsigned long window, int vlc,
                                                       int (*cancelled)(void *), void *userdata)
{
    return cancelled(userdata) ? 0 : open_window(window, vlc);
}

int r2_vision_media_stop_if_watch_owned(uint64_t token)
{
    if (r2_vision_media_watch_owns(token)) {
        active_token = 0;
        active_window = 0;
        ++stops;
    }
    return 0;
}

int64_t r2_log_event(R2LogCategory category, const char *type, const char *summary,
                     const char *details, const char *source)
{
    (void)category;
    assert(type && summary && details && source);
    return 1;
}

static const char *expected_media_path;
int64_t r2_log_media_event(const char *action, const char *media_type,
                           const char *path, const char *details)
{
    assert(action && media_type && details);
    assert(expected_media_path && !strcmp(path, expected_media_path));
    return 1;
}

int main(void)
{
    ShellWatchMedia managed = {0};
    long producers[] = {7777, 4321};
    R2AudioDiscovery playback = {.desktop_playing = 1};
    R2AudioDiscovery idle = {0};
    atomic_store(&shell_watch_stop_requested, 0);
    vlc_window = 101;
    shell_watch_media_poll(&managed, 1, &playback);
    assert(managed.token && active_vlc && active_window == 101 && opens == 1);

    vlc_window = 102;
    uint64_t previous = managed.token;
    shell_watch_media_poll(&managed, 1, &playback);
    assert(managed.token && managed.token != previous && active_vlc && active_window == 102);
    shell_watch_media_poll(&managed, 1, &playback);
    assert(opens == 2 && stops == 1);

    /* Higher priority CRT publication and its explicit withdrawal do not
     * depend on the success of PulseAudio discovery. */
    tv_publish_video_window(201, NULL);
    shell_watch_media_poll(&managed, 1, NULL);
    assert(managed.token && !active_vlc && active_window == 201);
    tv_publish_video_window(0, NULL);
    shell_watch_media_poll(&managed, 1, NULL);
    assert(!managed.token && !active_token && stops == 3);

    /* If the publication changes while FFmpeg opens, abandon our old token. */
    tv_publish_video_window(202, NULL);
    publish_on_open = 203;
    shell_watch_media_poll(&managed, 1, NULL);
    assert(!managed.token && !active_token && stops == 4);
    publish_on_open = 0;
    shell_watch_media_poll(&managed, 1, NULL);
    assert(managed.token && active_window == 203);

    /* A manual takeover revokes Watch's token and survives a new candidate. */
    active_token = 0;
    manual_active = 1;
    tv_publish_video_window(204, NULL);
    shell_watch_media_poll(&managed, 1, &playback);
    assert(!managed.token && manual_active && opens == 5 && stops == 4);
    manual_active = 0;

    /* Producer window priority and a transient audio discovery failure. */
    tv_publish_video_window(0, NULL);
    pid_window = 301;
    playback.playback_producers = producers;
    playback.playback_producer_count = 2;
    shell_watch_media_poll(&managed, 1, &playback);
    assert(managed.token && !active_vlc && active_window == 301);
    shell_watch_media_poll(&managed, 1, NULL);
    assert(managed.token && active_window == 301);
    previous = managed.token;
    shell_watch_media_poll(&managed, 1, &playback);
    assert(managed.token == previous);
    failed_visual_probe = 1;
    shell_watch_media_poll(&managed, 1, &playback);
    assert(managed.token == previous && active_window == 301);
    failed_visual_probe = 0;
    eyes_allowed = 0;
    shell_watch_media_poll(&managed, 0, &playback);
    assert(!managed.token && !active_token);
    eyes_allowed = 1;
    char legacy[] = "vcr_playback_started|/tmp/old.mp4|x11=401";
    expected_media_path = "/tmp/old.mp4";
    assert(!strncmp(tv_control_media_event(legacy, 1), "OK", 2));
    assert((uint32_t)atomic_load(&tv_live_renderer) == 401);
    char render[] = "401|/tmp/tape|x11=literal.mp4";
    assert(!strncmp(tv_control_renderer(render), "OK", 2));
    uint64_t publication = atomic_load(&tv_live_renderer);
    char repeat[] = "401|/tmp/tape|x11=literal.mp4";
    assert(!strncmp(tv_control_renderer(repeat), "OK", 2));
    assert(atomic_load(&tv_live_renderer) == publication);
    shell_watch_media_poll(&managed, 1, &idle);
    assert(active_window == 401 && managed.token);
    previous = managed.token;
    char next_tape[] = "401|/tmp/next.mp4";
    assert(!strncmp(tv_control_renderer(next_tape), "OK", 2));
    assert(atomic_load(&tv_live_renderer) != publication);
    shell_watch_media_poll(&managed, 1, &idle);
    assert(active_window == 401 && managed.token && managed.token != previous);

    /* Exact-path events can contain legacy-looking text and do not claim a window. */
    publication = atomic_load(&tv_live_renderer);
    char exact[] = "vcr_playback_stopped|/tmp/tape|x11=literal.mp4";
    expected_media_path = "/tmp/tape|x11=literal.mp4";
    assert(!strncmp(tv_control_media_event(exact, 0), "OK", 2));
    assert(atomic_load(&tv_live_renderer) == publication);
    char invalid[] = "4294967296|/tmp/tape.mp4";
    assert(!strncmp(tv_control_renderer(invalid), "ERROR", 5));
    assert(atomic_load(&tv_live_renderer) == publication);
    char withdraw[] = "0";
    assert(!strncmp(tv_control_renderer(withdraw), "OK", 2));
    shell_watch_media_poll(&managed, 1, NULL);
    assert(!managed.token && !active_token);
    puts("Watch switches exact renderers, respects CRT withdrawal and preserves manual custody.");
    return 0;
}
