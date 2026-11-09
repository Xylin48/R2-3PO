/* Real core workers, pipes, cancellation, and child reaping; synthetic pixels. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

int __wrap_r2_visual_is_initialized(void) { return 1; }

static const char *expected_source;

char *__wrap_r2_visual_analyze_frame(const R2VisionFrame *frame, const char *source,
                                    const char *question)
{
    (void)question;
    assert(expected_source && source && strcmp(source, expected_source) == 0);
    assert(frame->size == 12 && frame->data[11] == 11);
    return strdup("visual fixture result");
}

static double monotonic_seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void write_executable(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0);
    assert(fclose(file) == 0 && chmod(path, 0700) == 0);
}

static pid_t wait_for_child(const char *path)
{
    for (int i = 0; i < 2000; ++i) {
        FILE *file = fopen(path, "r");
        long pid = -1;
        if (file) {
            int got = fscanf(file, "%ld", &pid);
            fclose(file);
            if (got == 1 && pid > 1) return (pid_t)pid;
        }
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    assert(!"decoder never reached its blocked state");
    return -1;
}

static void assert_reaped(pid_t child)
{
    int status;
    errno = 0;
    assert(waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD);
    errno = 0;
    assert(kill(child, 0) == -1 && errno == ESRCH);
}

static int cancel_after_partial_read(void *userdata)
{
    int *checks = userdata;
    /* Before poll, after poll, then after the only available five bytes. */
    return ++*checks >= 3;
}

static void assert_no_complete_frame(R2Eyes *receiver)
{
    R2VisionFrame frame = {0};
    unsigned char output[12];
    memset(output, 0xE7, sizeof(output));
    assert(r2_eyes_get_frame(receiver, &frame) == -1);
    assert(r2_eyes_copy_frame(receiver, output, sizeof(output)) == -1);
    for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xE7);
}

int main(void)
{
    char root[] = "/tmp/r2-visual-lifecycle-XXXXXX";
    assert(mkdtemp(root));
    char probe[PATH_MAX], decoder[PATH_MAX], ready[PATH_MAX], release[PATH_MAX];
    char xdotool[PATH_MAX], path[PATH_MAX * 2];
    assert(snprintf(probe, sizeof(probe), "%s/ffprobe", root) < (int)sizeof(probe));
    assert(snprintf(decoder, sizeof(decoder), "%s/ffmpeg", root) < (int)sizeof(decoder));
    assert(snprintf(ready, sizeof(ready), "%s/ready", root) < (int)sizeof(ready));
    assert(snprintf(release, sizeof(release), "%s/release", root) < (int)sizeof(release));
    assert(snprintf(xdotool, sizeof(xdotool), "%s/xdotool", root) < (int)sizeof(xdotool));
    write_executable(xdotool,
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  search) echo 123 ;;\n"
        "  getwindowgeometry) printf 'WIDTH=2\\nHEIGHT=2\\n' ;;\n"
        "  getwindowname) printf '%s\\n' \"$R2_TEST_VLC_TITLE\" ;;\n"
        "  *) exit 1 ;;\n"
        "esac\n");
    write_executable(probe,
        "#!/usr/bin/python3\n"
        "import os, signal, sys\n"
        "if sys.argv[-1] == 'probe-stalled':\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "    with open(os.environ['R2_EYES_TEST_PID_FILE'], 'w') as f:\n"
        "        f.write(str(os.getpid()))\n"
        "    while True: signal.pause()\n"
        "print('2x2')\n"
        "if sys.argv[-1] == 'probe-failed': sys.exit(7)\n");
    write_executable(decoder,
        "#!/usr/bin/python3\n"
        "import os, signal, sys, time\n"
        "mode = sys.argv[sys.argv.index('-i') + 1]\n"
        "if mode == 'finite':\n"
        "    os.write(1, bytes(range(24)))\n"
        "elif mode == 'partial':\n"
        "    os.write(1, b'12345')\n"
        "elif mode == 'failed':\n"
        "    sys.exit(7)\n"
        "elif mode in ('resumable', 'resumable-after-frame'):\n"
        "    split = 5 if mode == 'resumable' else 17\n"
        "    end = 24 if mode == 'resumable' else 36\n"
        "    os.write(1, bytes(range(split)))\n"
        "    with open(os.environ['R2_EYES_TEST_PID_FILE'], 'w') as f:\n"
        "        f.write(str(os.getpid()))\n"
        "    while not os.path.exists(os.environ['R2_EYES_TEST_RELEASE']): time.sleep(.001)\n"
        "    os.write(1, bytes(range(split, end)))\n"
        "else:\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "    if mode == 'stalled-partial': os.write(1, b'12345')\n"
        "    if mode.startswith('long-frame:') or mode == ':test-display': os.write(1, bytes(range(12)))\n"
        "    with open(os.environ['R2_EYES_TEST_PID_FILE'], 'w') as f:\n"
        "        f.write(str(os.getpid()))\n"
        "    while True: signal.pause()\n");
    const char *original = getenv("PATH");
    assert(snprintf(path, sizeof(path), "%s:%s", root,
                    original ? original : "/usr/bin") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("R2_EYES_TEST_PID_FILE", ready, 1) == 0);
    assert(setenv("R2_EYES_TEST_RELEASE", release, 1) == 0);
    assert(unsetenv("DISPLAY") == 0);
    core_initialized = 1;
    shutting_down = 0;
    assert(r2_eyes_init(&eyes) == 0);

    /* Opening has a bounded metadata budget even before a capture worker can
     * exist. An exit error cannot legitimize plausible dimensions either. */
    double start = monotonic_seconds();
    assert(r2_eyes_open_file(eyes, "probe-stalled") == -1);
    assert(monotonic_seconds() - start < 7.0 && !r2_eyes_is_open(eyes));
    pid_t child = wait_for_child(ready);
    assert_reaped(child);
    assert(unlink(ready) == 0);
    assert(r2_eyes_open_file(eyes, "probe-failed") == -1);

    /* Stop must wake a worker blocked before its first complete frame. */
    assert(r2_eyes_open_file(eyes, "stalled") == 0);
    assert(r2_vision_watch_start() == 0);
    child = wait_for_child(ready);
    start = monotonic_seconds();
    assert(r2_vision_watch_stop() == 0);
    assert(monotonic_seconds() - start < 2.0);
    assert(!r2_vision_watch_active() && r2_eyes_is_open(eyes));
    assert(kill(child, 0) == 0); /* Worker stop alone preserves the source owner. */
    assert(r2_eyes_release_source() == 0);
    assert(!r2_eyes_is_open(eyes));
    assert_reaped(child);
    assert(unlink(ready) == 0);

    /* Cancellation must preserve the manually owned stream and the incomplete
     * frame, so resuming cannot shift all subsequent RGB pixels by five bytes. */
    assert(r2_eyes_open_file(eyes, "resumable") == 0);
    child = wait_for_child(ready);
    assert_no_complete_frame(eyes);
    int checks = 0;
    assert(r2_eyes_capture_cancelable(eyes, cancel_after_partial_read, &checks) == -1);
    assert(errno == ECANCELED && r2_eyes_is_open(eyes));
    assert_no_complete_frame(eyes);
    FILE *release_file = fopen(release, "w");
    assert(release_file && fclose(release_file) == 0);
    for (unsigned frame_index = 0; frame_index < 2; ++frame_index) {
        assert(r2_eyes_capture(eyes) == 1);
        R2VisionFrame frame = {0};
        assert(r2_eyes_get_frame(eyes, &frame) == 0 && frame.size == 12);
        for (size_t i = 0; i < frame.size; ++i)
            assert(frame.data[i] == frame_index * 12 + i);
    }
    assert(r2_eyes_capture(eyes) == 0 && !r2_eyes_is_open(eyes));
    assert_reaped(child);
    assert(unlink(ready) == 0 && unlink(release) == 0);

    /* A partial next frame must not be exposed with the previous frame's
     * valid metadata after it overwrites part of the shared pixel buffer. */
    assert(r2_eyes_open_file(eyes, "resumable-after-frame") == 0);
    child = wait_for_child(ready);
    assert(r2_eyes_capture(eyes) == 1);
    R2VisionFrame prior = {0};
    assert(r2_eyes_get_frame(eyes, &prior) == 0 && prior.frame_number == 1);
    checks = 0;
    assert(r2_eyes_capture_cancelable(eyes, cancel_after_partial_read, &checks) == -1);
    assert(errno == ECANCELED && r2_eyes_is_open(eyes));
    assert_no_complete_frame(eyes);
    release_file = fopen(release, "w");
    assert(release_file && fclose(release_file) == 0);
    for (unsigned frame_index = 1; frame_index < 3; ++frame_index) {
        assert(r2_eyes_capture(eyes) == 1);
        R2VisionFrame frame = {0};
        assert(r2_eyes_get_frame(eyes, &frame) == 0 && frame.size == 12);
        assert(frame.frame_number == frame_index + 1);
        for (size_t i = 0; i < frame.size; ++i)
            assert(frame.data[i] == frame_index * 12 + i);
    }
    assert(r2_eyes_capture(eyes) == 0 && !r2_eyes_is_open(eyes));
    assert_reaped(child);
    assert(unlink(ready) == 0 && unlink(release) == 0);

    /* Discovery can attach to a pre-opened camera, but a manual start adopts
     * the worker and revokes its claim without replacing or stopping it. */
    assert(r2_eyes_open_file(eyes, "stalled") == 0);
    uint64_t old_token = r2_vision_watch_start_for_watch();
    assert(old_token && r2_vision_watch_owns(old_token));
    child = wait_for_child(ready);
    assert(r2_vision_watch_start() == 0);
    assert(!r2_vision_watch_owns(old_token));
    assert(r2_vision_watch_start_for_watch() == 0);
    assert(r2_vision_watch_stop_if_watch_owned(old_token) == 0);
    assert(r2_vision_watch_active() && kill(child, 0) == 0);
    assert(r2_vision_watch_stop() == 0);
    assert(r2_eyes_is_open(eyes));
    assert(r2_eyes_release_source() == 0);
    assert_reaped(child);
    assert(unlink(ready) == 0);

    /* A stale cleanup also cannot stop the next independently opened worker. */
    assert(r2_eyes_open_file(eyes, "stalled") == 0);
    uint64_t new_token = r2_vision_watch_start_for_watch();
    assert(new_token && new_token != old_token && r2_vision_watch_owns(new_token));
    child = wait_for_child(ready);
    assert(r2_vision_watch_stop_if_watch_owned(old_token) == 0);
    assert(r2_vision_watch_active() && kill(child, 0) == 0);
    assert(r2_vision_watch_stop_if_watch_owned(new_token) == 0);
    assert(!r2_vision_watch_active() && !r2_vision_watch_owns(new_token));
    assert(r2_eyes_is_open(eyes));
    assert(r2_eyes_release_source() == 0);
    assert_reaped(child);
    assert(unlink(ready) == 0);

    /* Separate media has the same contract, including a partial frame. */
    assert(r2_vision_open_file("stalled-partial") == 0);
    child = wait_for_child(ready);
    assert(r2_vision_media_set_attention(1) == 0);
    start = monotonic_seconds();
    assert(r2_vision_media_stop() == 0);
    assert(monotonic_seconds() - start < 2.0);
    assert(!r2_vision_media_status() && !r2_vision_media_attention_status());
    assert_reaped(child);
    assert(unlink(ready) == 0);

    /* Restart yields exact complete frames, then clean EOF, without stale data. */
    assert(r2_eyes_open_file(eyes, "finite") == 0);
    for (unsigned frame_index = 0; frame_index < 2; ++frame_index) {
        assert(r2_eyes_capture(eyes) == 1);
        R2VisionFrame frame = {0};
        assert(r2_eyes_get_frame(eyes, &frame) == 0 && frame.size == 12);
        for (size_t i = 0; i < frame.size; ++i)
            assert(frame.data[i] == frame_index * 12 + i);
    }
    assert(r2_eyes_capture(eyes) == 0 && !r2_eyes_is_open(eyes));
    assert(r2_eyes_open_file(eyes, "partial") == 0);
    assert(r2_eyes_capture(eyes) == -1 && !r2_eyes_is_open(eyes));
    assert(r2_eyes_open_file(eyes, "failed") == 0);
    assert(r2_eyes_capture(eyes) == -1 && !r2_eyes_is_open(eyes));

    /* Use the public capture and attention paths with names sharing a long
     * prefix. The matching source must accompany the actual copied pixels. */
    char camera_source[1700], media_source[1700];
    memset(camera_source, 's', sizeof(camera_source));
    memcpy(camera_source, "long-frame:", 11);
    memcpy(camera_source + sizeof(camera_source) - sizeof(" camera"), " camera", sizeof(" camera"));
    memcpy(media_source, camera_source, sizeof(media_source));
    memcpy(media_source + sizeof(media_source) - sizeof(" media\n"), " media\n", sizeof(" media\n"));

    assert(r2_eyes_open_file(eyes, camera_source) == 0);
    R2VisionEvent legacy = {0};
    assert(r2_eyes_get_event(eyes, &legacy) == -1 && errno == EOVERFLOW);
    char *saved_source = r2_eyes_copy_source(eyes);
    assert(saved_source && strcmp(saved_source, camera_source) == 0);
    expected_source = camera_source;
    char *observation = r2_vision_see("direct source");
    assert(observation);
    free(observation);
    child = wait_for_child(ready);
    assert(r2_eyes_release_source() == 0);
    assert_reaped(child);
    assert(unlink(ready) == 0);
    assert(strcmp(saved_source, camera_source) == 0);
    free(saved_source);

    assert(r2_eyes_open_file(eyes, camera_source) == 0);
    assert(r2_vision_watch_start() == 0);
    child = wait_for_child(ready);
    observation = r2_vision_see("watched source");
    assert(observation);
    free(observation);
    assert(r2_eyes_release_source() == 0);
    assert_reaped(child);
    assert(unlink(ready) == 0);

    assert(setenv("DISPLAY", ":test-display", 1) == 0);
    assert(setenv("R2_TEST_VLC_TITLE", media_source, 1) == 0);
    assert(r2_vision_open_vlc() == 0);
    child = wait_for_child(ready);
    expected_source = media_source;
    observation = NULL;
    for (int i = 0; i < 2000 && !observation; ++i) {
        observation = r2_vision_media_see("long VLC source");
        if (!observation) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    assert(observation);
    free(observation);
    assert(r2_vision_media_stop() == 0);
    assert_reaped(child);
    assert(unlink(ready) == 0);
    assert(unsetenv("DISPLAY") == 0);

    r2_eyes_shutdown(media_eyes);
    media_eyes = NULL;
    r2_eyes_shutdown(eyes);
    eyes = NULL;
    core_initialized = 0;
    assert(unlink(probe) == 0 && unlink(decoder) == 0 && unlink(xdotool) == 0 && rmdir(root) == 0);
    puts("Stalled camera/media stop, exact worker ownership, partial-frame rejection, decoder exit failure, restart, child reaping and full capture source identity passed.");
    return 0;
}
