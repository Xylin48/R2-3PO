#define _POSIX_C_SOURCE 200809L
#include "Eyes.h"
#include "Log.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int64_t r2_log_sensory(const char *type, const char *text,
                       const char *details, const char *source)
{
    (void)type; (void)text; (void)details; (void)source;
    return 0;
}
static int cancel_when_child_ready(void *path)
{
    return access(path, F_OK) == 0;
}

int main(void)
{
    char dir[] = "/tmp/r2-window-discovery-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char script[PATH_MAX], path[PATH_MAX * 2], ready[PATH_MAX];
    assert(snprintf(script, sizeof(script), "%s/xdotool", dir) < (int)sizeof(script));
    assert(snprintf(ready, sizeof(ready), "%s/ready", dir) < (int)sizeof(ready));
    FILE *fp = fopen(script, "w");
    assert(fp);
    assert(fputs(
        "#!/bin/sh\n"
        "if [ \"$1\" = \"search\" ] && [ \"$2\" = \"--onlyvisible\" ] && [ \"$3\" = \"--pid\" ]; then\n"
        "  if [ \"$4\" = \"9000\" ]; then exit 1; fi\n"
        "  if [ \"$4\" = \"9001\" ]; then echo 'no display' >&2; exit 1; fi\n"
        "  if [ \"$4\" = \"9002\" ]; then printf '103\\n'; fi\n"
        "  if [ \"$4\" = \"3141\" ]; then printf '101\\n102\\n'; fi\n"
        "  if [ \"$4\" = \"3142\" ]; then printf '101\\n102\\n'; exit 7; fi\n"
        "  if [ \"$4\" = \"3143\" ]; then printf '101\\nnot-a-window\\n'; fi\n"
        "  if [ \"$4\" = \"3146\" ]; then printf '101\\n-2\\n'; fi\n"
        "  if [ \"$4\" = \"3147\" ]; then printf '101\\n4294967296\\n'; fi\n"
        "  if [ \"$4\" = \"3144\" ]; then\n"
        "    exec python3 -c 'import os, signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); open(os.environ[\"R2_EYES_QUERY_PID\"], \"w\").write(str(os.getpid())); signal.pause()'\n"
        "  fi\n"
        "  if [ \"$4\" = \"3145\" ]; then sleep 3; printf '101\\n205\\n'; fi\n"
        "elif [ \"$1\" = \"search\" ] && [ \"$3\" = \"--class\" ]; then\n"
        "  printf '101\\n102\\n'\n"
        "elif [ \"$1\" = \"getwindowgeometry\" ] && [ \"$2\" = \"--shell\" ]; then\n"
        "  if [ \"$3\" = \"101\" ]; then printf 'WIDTH=640\\nHEIGHT=480\\n'; fi\n"
        "  if [ \"$3\" = \"102\" ]; then printf 'WIDTH=1280\\nHEIGHT=720\\n'; fi\n"
        "  if [ \"$3\" = \"103\" ]; then printf 'WIDTH=1280\\nHEIGHT=720\\n'; fi\n"
        "  if [ \"$3\" = \"200\" ]; then printf 'WIDTH=640junk\\nHEIGHT=480\\n'; fi\n"
        "  if [ \"$3\" = \"201\" ]; then printf 'WIDTH=42949672960\\nHEIGHT=480\\n'; fi\n"
        "  if [ \"$3\" = \"202\" ]; then printf 'WIDTH=640\\nHEIGHT=480\\n'; exit 7; fi\n"
        "  if [ \"$3\" = \"203\" ]; then\n"
        "    i=0; while [ \"$i\" -lt 3000 ]; do printf 'UNRELATED_GEOMETRY_FIELD=ignored\\n'; i=$((i+1)); done\n"
        "    printf 'WIDTH=640\\nHEIGHT=480\\n'\n"
        "  fi\n"
        "  if [ \"$3\" = \"204\" ]; then printf 'WIDTH=640\\000junk\\nHEIGHT=480\\n'; fi\n"
        "  if [ \"$3\" = \"205\" ]; then\n"
        "    exec python3 -c 'import os, signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); open(os.environ[\"R2_EYES_QUERY_PID\"], \"w\").write(str(os.getpid())); signal.pause()'\n"
        "  fi\n"
        "fi\n", fp) >= 0);
    assert(fclose(fp) == 0 && chmod(script, 0700) == 0);
    const char *existing = getenv("PATH");
    assert(snprintf(path, sizeof(path), "%s:%s", dir,
                    existing ? existing : "/usr/bin") < (int)sizeof(path));
    assert(setenv("PATH", path, 1) == 0);
    assert(setenv("R2_EYES_QUERY_PID", ready, 1) == 0);
    assert(r2_eyes_window_for_pid(-1) == 0);
    assert(r2_eyes_window_for_pid(9999) == 0);
    assert(r2_eyes_window_for_pid(3141) == 102);
    assert(r2_eyes_window_available(101));
    assert(r2_eyes_window_available(102));
    assert(!r2_eyes_window_available(9999));
    assert(!r2_eyes_window_available(200));
    assert(!r2_eyes_window_available(201));
    assert(!r2_eyes_window_available(202));
    assert(r2_eyes_window_available(203)); /* No small output/line quota. */
    assert(!r2_eyes_window_available(204));
    assert(r2_eyes_window_for_pid(3142) == 0);
    assert(r2_eyes_window_for_pid(3143) == 0);
    assert(r2_eyes_window_for_pid(3146) == 0);
    assert(r2_eyes_window_for_pid(3147) == 0);
    assert(r2_eyes_vlc_window() == 102);
    long producers[] = {9000, 3141, 9002};
    long pid = -1;
    unsigned long window = 0;
    assert(r2_eyes_select_playback_window(producers, 3, &pid, &window, NULL, NULL) == 0);
    assert(pid == 3141 && window == 102);
    /* Keep the exact smaller window of the existing producer when another
     * application is listed first, including another eligible renderer. */
    long reordered[] = {9002, 9000, 3141};
    window = 101;
    assert(r2_eyes_select_playback_window(reordered, 3, &pid, &window, NULL, NULL) == 0);
    assert(pid == 3141 && window == 101);
    assert(r2_eyes_select_playback_window(reordered, 2, &pid, &window, NULL, NULL) == 0);
    assert(pid == 9002 && window == 103);
    long empty = 9000;
    assert(r2_eyes_select_playback_window(&empty, 1, &pid, &window, NULL, NULL) == 0);
    assert(pid == -1 && window == 102);
    long failure[] = {9001, 3141};
    assert(r2_eyes_select_playback_window(failure, 2, &pid, &window, NULL, NULL) == -1);
    assert(errno == EIO && !window);

    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(r2_eyes_window_for_pid(3144) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    double elapsed = after.tv_sec - before.tv_sec +
        (after.tv_nsec - before.tv_nsec) / 1000000000.0;
    assert(elapsed < 7.0);
    fp = fopen(ready, "r");
    long child;
    assert(fp && fscanf(fp, "%ld", &child) == 1 && child > 1);
    assert(fclose(fp) == 0);
    int status;
    assert(waitpid((pid_t)child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    assert(unlink(ready) == 0);
    /* Search and every candidate share the same budget. A valid early result
     * cannot turn an incomplete later geometry query into a known snapshot. */
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(r2_eyes_window_for_pid(3145) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    elapsed = after.tv_sec - before.tv_sec +
        (after.tv_nsec - before.tv_nsec) / 1000000000.0;
    assert(elapsed < 7.0);
    fp = fopen(ready, "r");
    assert(fp && fscanf(fp, "%ld", &child) == 1 && fclose(fp) == 0);
    assert(waitpid((pid_t)child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    assert(unlink(ready) == 0);
    long delayed[] = {9000, 3144, 9002};
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    assert(r2_eyes_select_playback_window(delayed, 3, &pid, &window,
                                         cancel_when_child_ready, ready) == -1);
    assert(errno == ECANCELED && !window);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    elapsed = after.tv_sec - before.tv_sec + (after.tv_nsec - before.tv_nsec) / 1000000000.0;
    assert(elapsed < 2.0);
    fp = fopen(ready, "r");
    assert(fp && fscanf(fp, "%ld", &child) == 1 && fclose(fp) == 0);
    assert(waitpid((pid_t)child, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(kill((pid_t)child, 0) == -1 && errno == ESRCH);
    assert(unlink(ready) == 0);
    assert(unlink(script) == 0 && rmdir(dir) == 0);
    puts("X11 discovery verifies complete successful geometry, rejects malformed/failed snapshots, bounds stalled queries and reaps children.");
    return 0;
}
