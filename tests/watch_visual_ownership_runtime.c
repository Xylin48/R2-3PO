/*
 * Real core Watch ownership helpers with synthetic visual availability.
 * The test intentionally never opens any host camera or video display.
 */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdio.h>

int __wrap_r2_visual_is_initialized(void)
{
    return 1;
}

/* Simulate an already opened physical camera, without needing a device. */
int __wrap_r2_eyes_is_open(R2Eyes *receiver)
{
    assert((uintptr_t)receiver == 1);
    return 1;
}

int main(void)
{
    /* A stale Watch cleanup may not stop a replacement media session. */
    atomic_store(&media_capture_running, 1);
    media_capture_thread_started = 0;
    media_watch_token = 57;
    assert(r2_vision_media_watch_owns(57));
    assert(!r2_vision_media_watch_owns(56));
    assert(r2_vision_media_stop_if_watch_owned(56) == 0);
    assert(r2_vision_media_status() && media_watch_token == 57);

    /* Manual media availability appeared after Watch inspected status.
     * The guarded open must not stop/replace the other owner's display. */
    core_initialized = 1;
    shutting_down = 0;
    atomic_store(&eyes_auto_allowed, 1);
    assert(r2_vision_media_open_window_for_watch(700) == 0);
    assert(r2_vision_media_open_vlc_for_watch() == 0);
    assert(r2_vision_media_status() && media_watch_token == 57);

    /* Manual stop invalidates the old token and closes its own display. */
    assert(r2_vision_media_stop() == 0);
    assert(!r2_vision_media_watch_owns(57));
    assert(!r2_vision_media_status());
    assert(media_watch_token == 0);

    /* A matching Watch token closes its own generation, not future ones. */
    media_watch_token = 81;
    atomic_store(&media_capture_running, 1);
    assert(r2_vision_media_stop_if_watch_owned(81) == 0);
    assert(!r2_vision_media_status() && media_watch_token == 0);

    /* Camera cleanup has the same exact-generation contract.
     * The synthetic test has no camera; it checks that stale cleanup
     * never calls release and that an explicit release revokes ownership. */
    eyes = (R2Eyes *)(uintptr_t)1;
    assert(r2_eyes_start_for_watch() == 0);
    assert(eyes_watch_token == 0); /* no takeover of an existing camera */
    /* The explicit question's stale outer check must neither replace a
     * camera Watch just opened nor revoke its cleanup token. */
    eyes_watch_token = 105;
    assert(r2_eyes_start_impl(NULL, 0) == 0);
    assert(r2_eyes_watch_owns(105));
    r2_sense_set_auto_allowed(1, 0);
    assert(r2_eyes_start_impl(NULL, 0) == -1);
    assert(!r2_sense_auto_allowed(1) && r2_eyes_watch_owns(105));
    r2_sense_set_auto_allowed(1, 1);
    eyes = NULL;
    assert(r2_eyes_watch_owns(105));
    assert(!r2_eyes_watch_owns(104));
    assert(r2_eyes_release_if_watch_owned(104) == 0);
    assert(eyes_watch_token == 105);
    assert(r2_eyes_release_source() == -1); /* no physical camera attached */
    assert(!r2_eyes_watch_owns(105));
    assert(eyes_watch_token == 0);

    core_initialized = 0;
    puts("Watch exact camera/media token checks protect manual and replaced visual sources.");
    return 0;
}
