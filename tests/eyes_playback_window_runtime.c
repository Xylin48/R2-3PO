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
    return 0;
}
int main(void)
{
    char dir[] = "/tmp/r2-window-discovery-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char script[PATH_MAX], path[PATH_MAX * 2];
    assert(snprintf(script, sizeof(script), "%s/xdotool", dir) < (int)sizeof(script));
    FILE *fp = fopen(script, "w");
    assert(fp);
    assert(fputs(
        "#!/bin/sh\n"
        "if [ \"$1\" = \"search\" ] && [ \"$2\" = \"--onlyvisible\" ] && [ \"$3\" = \"--pid\" ]; then\n"
        "  if [ \"$4\" = \"3141\" ]; then printf '101\\n102\\n'; fi\n"
        "elif [ \"$1\" = \"getwindowgeometry\" ] && [ \"$2\" = \"--shell\" ]; then\n"
        "  if [ \"$3\" = \"101\" ]; then printf 'WIDTH=640\\nHEIGHT=480\\n'; fi\n"
        "  if [ \"$3\" = \"102\" ]; then printf 'WIDTH=1280\\nHEIGHT=720\\n'; fi\n"
        "fi\n", fp) >= 0);
    assert(fclose(fp) == 0 && chmod(script, 0700) == 0);
    const char *existing = getenv("PATH");
    assert(snprintf(path, sizeof(path), "%s:%s", dir,
                    existing ? existing : "/usr/bin") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(r2_eyes_window_for_pid(-1) == 0);
    assert(r2_eyes_window_for_pid(9999) == 0);
    assert(r2_eyes_window_for_pid(3141) == 102);
    assert(r2_eyes_window_available(101));
    assert(r2_eyes_window_available(102));
    assert(!r2_eyes_window_available(9999));
    assert(unlink(script) == 0 && rmdir(dir) == 0);
    puts("Visible X11 window discovery selects the actual largest renderer for an audio producer.");
    return 0;
}
