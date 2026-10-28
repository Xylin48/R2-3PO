#define _POSIX_C_SOURCE 200809L
#include "Eyes.h"
#include "Log.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int64_t r2_log_sensory(const char *type, const char *text,
                       const char *details, const char *source)
{
    (void)type; (void)text; (void)details; (void)source;
    return 1;
}
int main(void)
{
    char root[] = "/tmp/r2-independent-video-XXXXXX";
    assert(mkdtemp(root));
    char probe[PATH_MAX], decoder[PATH_MAX], path[PATH_MAX * 2];
    assert(snprintf(probe, sizeof(probe), "%s/ffprobe", root) < (int)sizeof(probe));
    assert(snprintf(decoder, sizeof(decoder), "%s/ffmpeg", root) < (int)sizeof(decoder));
    FILE *file = fopen(probe, "w");
    assert(file);
    assert(fputs("#!/bin/sh\necho '32x24'\n", file) >= 0);
    assert(fclose(file) == 0 && chmod(probe, 0700) == 0);
    file = fopen(decoder, "w");
    assert(file);
    assert(fputs(
        "#!/bin/sh\n"
        "case \"$*\" in\n"
        "  *secondary*) python3 -c 'import sys; sys.stdout.buffer.write(bytes([90])*4608)' ;;\n"
        "  *) python3 -c 'import sys; sys.stdout.buffer.write(bytes([65])*4608)' ;;\n"
        "esac\n", file) >= 0);
    assert(fclose(file) == 0 && chmod(decoder, 0700) == 0);
    const char *original = getenv("PATH");
    assert(snprintf(path, sizeof(path), "%s:%s", root,
                    original ? original : "/usr/bin") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(unsetenv("DISPLAY") == 0);

    R2Eyes *primary = NULL, *media = NULL;
    assert(r2_eyes_init(&primary) == 0 && r2_eyes_init(&media) == 0);
    r2_eyes_set_auxiliary(media, 1);
    assert(r2_eyes_open_file(primary, "primary") == 0);
    assert(r2_eyes_open_file(media, "secondary") == 0);
    assert(r2_eyes_capture(primary) == 1);
    assert(r2_eyes_capture(media) == 1);
    R2VisionFrame camera_frame = {0}, media_frame = {0};
    R2VisionEvent camera_event = {0}, media_event = {0};
    assert(r2_eyes_get_frame(primary, &camera_frame) == 0);
    assert(r2_eyes_get_frame(media, &media_frame) == 0);
    assert(r2_eyes_get_event(primary, &camera_event) == 0);
    assert(r2_eyes_get_event(media, &media_event) == 0);
    assert(camera_frame.size == 2304 && media_frame.size == 2304);
    assert(camera_frame.data[0] == 65 && media_frame.data[0] == 90);
    assert(strstr(camera_event.source_name, "primary"));
    assert(strstr(media_event.source_name, "secondary"));
    r2_eyes_close(media);
    assert(r2_eyes_is_open(primary) && !r2_eyes_is_open(media));
    assert(r2_eyes_capture(primary) == 1);
    assert(r2_eyes_get_frame(primary, &camera_frame) == 0);
    assert(camera_frame.data[0] == 65);
    r2_eyes_shutdown(media);
    r2_eyes_shutdown(primary);
    assert(unlink(probe) == 0 && unlink(decoder) == 0 && rmdir(root) == 0);
    puts("Two independent real Eyes streams preserve primary camera frames while media closes.");
    return 0;
}
