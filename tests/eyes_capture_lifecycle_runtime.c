/* Real core workers, pipes, cancellation, and child reaping; synthetic pixels. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

int __wrap_r2_visual_is_initialized(void) { return 1; }

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

int main(void)
{
    char root[] = "/tmp/r2-visual-lifecycle-XXXXXX";
    assert(mkdtemp(root));
    char probe[PATH_MAX], decoder[PATH_MAX], ready[PATH_MAX], path[PATH_MAX * 2];
    assert(snprintf(probe, sizeof(probe), "%s/ffprobe", root) < (int)sizeof(probe));
    assert(snprintf(decoder, sizeof(decoder), "%s/ffmpeg", root) < (int)sizeof(decoder));
    assert(snprintf(ready, sizeof(ready), "%s/ready", root) < (int)sizeof(ready));
    write_executable(probe, "#!/bin/sh\nprintf '2x2\\n'\n");
    write_executable(decoder,
        "#!/usr/bin/python3\n"
        "import os, signal, sys\n"
        "mode = sys.argv[sys.argv.index('-i') + 1]\n"
        "if mode == 'finite':\n"
        "    os.write(1, bytes(range(24)))\n"
        "elif mode == 'partial':\n"
        "    os.write(1, b'12345')\n"
        "elif mode == 'failed':\n"
        "    sys.exit(7)\n"
        "else:\n"
        "    signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
        "    if mode == 'stalled-partial': os.write(1, b'12345')\n"
        "    with open(os.environ['R2_EYES_TEST_PID_FILE'], 'w') as f:\n"
        "        f.write(str(os.getpid()))\n"
        "    while True: signal.pause()\n");
    const char *original = getenv("PATH");
    assert(snprintf(path, sizeof(path), "%s:%s", root,
                    original ? original : "/usr/bin") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("R2_EYES_TEST_PID_FILE", ready, 1) == 0);
    assert(unsetenv("DISPLAY") == 0);
    core_initialized = 1;
    shutting_down = 0;
    assert(r2_eyes_init(&eyes) == 0);

    /* Stop must wake a worker blocked before its first complete frame. */
    assert(r2_eyes_open_file(eyes, "stalled") == 0);
    assert(r2_vision_watch_start() == 0);
    pid_t child = wait_for_child(ready);
    double start = monotonic_seconds();
    assert(r2_vision_watch_stop() == 0);
    assert(monotonic_seconds() - start < 2.0);
    assert(!r2_vision_watch_active() && !r2_eyes_is_open(eyes));
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

    r2_eyes_shutdown(media_eyes);
    media_eyes = NULL;
    r2_eyes_shutdown(eyes);
    eyes = NULL;
    core_initialized = 0;
    assert(unlink(probe) == 0 && unlink(decoder) == 0 && rmdir(root) == 0);
    puts("Stalled camera/media stop, partial-frame rejection, decoder exit failure, restart and child reaping passed.");
    return 0;
}
