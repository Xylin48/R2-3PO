#define _POSIX_C_SOURCE 200809L
#include "Eyes.h"
#include "Log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t r2_log_sensory(const char *type, const char *text,
                       const char *details, const char *source)
{
    (void)type; (void)text; (void)details; (void)source;
    return 1;
}

static void check_file(const char *path, uint32_t width, uint32_t height)
{
    R2Eyes *eyes = NULL;
    assert(r2_eyes_init(&eyes) == 0);
    r2_eyes_set_auxiliary(eyes, 1);
    assert(r2_eyes_open_file(eyes, path) == 0);
    R2VideoFormat format = {0};
    assert(r2_eyes_get_format(eyes, &format) == 0);
    assert(format.width == width && format.height == height &&
           format.stride == width * 3 && format.pixel_format == R2_PIXEL_RGB24);
    char *source = r2_eyes_copy_source(eyes);
    assert(source && !strcmp(source, path));
    free(source);

    for (uint64_t i = 1; i <= 3; ++i) {
        R2VisionFrame frame = {0};
        assert(r2_eyes_capture(eyes) == 1);
        assert(r2_eyes_get_frame(eyes, &frame) == 0);
        assert(frame.frame_number == i && frame.size == (size_t)width * height * 3);
        assert(frame.format.width == width && frame.format.height == height);
        /* First track is solid red; the larger second track is solid blue. */
        for (size_t at = 0; at < frame.size; at += 3)
            assert(frame.data[at] > 200 && frame.data[at + 1] < 30 &&
                   frame.data[at + 2] < 30);
    }
    assert(r2_eyes_capture(eyes) == 0 && !r2_eyes_is_open(eyes));
    r2_eyes_shutdown(eyes);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(unsetenv("DISPLAY") == 0);
    check_file(argv[1], 16, 32);
    check_file(argv[2], 32, 16);
    puts("Real FFmpeg decoded geometry, track, RGB bytes and frame boundaries agree.");
    return 0;
}
