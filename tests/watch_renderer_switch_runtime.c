/* Exercise the real Watch reconciliation function at the device API boundary. */
#include "../shell.c"
#include <assert.h>

static uint64_t active_token, next_token = 1;
static unsigned long active_window, vlc_window, pid_window;
static unsigned opens, stops;
static int active_vlc, eyes_allowed = 1, manual_active;
static unsigned long publish_on_open;

int r2_is_shutting_down(void) { return 0; }
int r2_sense_auto_allowed(int which) { assert(which == 1); return eyes_allowed; }
int r2_vision_available(void) { return 1; }
int r2_vision_media_status(void) { return active_token || manual_active; }
int r2_vision_window_available(unsigned long window) { return window != 0; }
unsigned long r2_vision_window_for_pid(long pid) { return pid > 1 ? pid_window : 0; }
unsigned long r2_vision_vlc_window(void) { return vlc_window; }
int r2_vision_media_watch_owns(uint64_t token) { return token && token == active_token; }

static uint64_t open_window(unsigned long window, int vlc)
{
    assert(!r2_vision_media_status() && window);
    active_window = window;
    active_vlc = vlc;
    ++opens;
    if (publish_on_open) atomic_store(&tv_live_video_window, publish_on_open);
    return active_token = next_token++;
}

uint64_t r2_vision_media_open_window_for_watch(unsigned long window)
{
    return open_window(window, 0);
}

uint64_t r2_vision_media_open_vlc_window_for_watch(unsigned long window)
{
    return open_window(window, 1);
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

int main(void)
{
    ShellWatchMedia managed = {0};
    atomic_store(&shell_watch_stop_requested, 0);
    vlc_window = 101;
    shell_watch_media_poll(&managed, 1, 1, 1, -1);
    assert(managed.token && active_vlc && active_window == 101 && opens == 1);

    vlc_window = 102;
    uint64_t previous = managed.token;
    shell_watch_media_poll(&managed, 1, 1, 1, -1);
    assert(managed.token && managed.token != previous && active_vlc && active_window == 102);
    shell_watch_media_poll(&managed, 1, 1, 1, -1);
    assert(opens == 2 && stops == 1);

    /* Higher priority CRT publication and its explicit withdrawal do not
     * depend on the success of PulseAudio discovery. */
    atomic_store(&tv_live_video_window, 201);
    shell_watch_media_poll(&managed, 1, 0, 0, -1);
    assert(managed.token && !active_vlc && active_window == 201);
    atomic_store(&tv_live_video_window, 0);
    shell_watch_media_poll(&managed, 1, 0, 0, -1);
    assert(!managed.token && !active_token && stops == 3);

    /* If the publication changes while FFmpeg opens, abandon our old token. */
    atomic_store(&tv_live_video_window, 202);
    publish_on_open = 203;
    shell_watch_media_poll(&managed, 1, 0, 0, -1);
    assert(!managed.token && !active_token && stops == 4);
    publish_on_open = 0;
    shell_watch_media_poll(&managed, 1, 0, 0, -1);
    assert(managed.token && active_window == 203);

    /* A manual takeover revokes Watch's token and survives a new candidate. */
    active_token = 0;
    manual_active = 1;
    atomic_store(&tv_live_video_window, 204);
    shell_watch_media_poll(&managed, 1, 1, 1, -1);
    assert(!managed.token && manual_active && opens == 5 && stops == 4);
    manual_active = 0;

    /* Producer window priority and a transient audio discovery failure. */
    atomic_store(&tv_live_video_window, 0);
    pid_window = 301;
    shell_watch_media_poll(&managed, 1, 1, 1, 4321);
    assert(managed.token && !active_vlc && active_window == 301);
    shell_watch_media_poll(&managed, 1, 0, 1, -1);
    assert(managed.token && active_window == 301);
    eyes_allowed = 0;
    shell_watch_media_poll(&managed, 0, 1, 1, 4321);
    assert(!managed.token && !active_token);
    puts("Watch switches exact renderers, respects CRT withdrawal and preserves manual custody.");
    return 0;
}
