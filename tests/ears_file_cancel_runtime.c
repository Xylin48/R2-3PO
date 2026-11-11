#define _POSIX_C_SOURCE 200809L
/*
 * End-to-end Ears file-decoder cancellation: run an actual child process but
 * supply a controlled FFmpeg executable through a private test PATH.
 * No physical microphone or media decoder package is needed.
 */
/* Include Ears only here to test a pointer borrowed from its own event;
 * production callers continue using the opaque Ears.h interface. */
#include "../Ears.c"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int source_closes = 0;
int64_t r2_log_sensory(const char *type, const char *text,
                      const char *details, const char *source)
{
    (void)text; (void)details; (void)source;
    if (type && strcmp(type, "audio_source_closed") == 0) ++source_closes;
    return 1;
}

typedef struct {
    R2Ears *ears;
    atomic_int begun;
    ssize_t result;
    int error;
} Reader;

static void *stalled_reader(void *userdata)
{
    Reader *reader = userdata;
    atomic_store(&reader->begun, 1);
    char out[4];
    reader->result = r2_ears_read(reader->ears, out, sizeof(out));
    reader->error = errno;
    return NULL;
}

static double duration(struct timespec a, struct timespec b)
{
    return (double)(b.tv_sec - a.tv_sec) +
           (double)(b.tv_nsec - a.tv_nsec) / 1000000000.0;
}

int main(void)
{
    char root[] = "/tmp/r2-file-cancel-XXXXXX";
    assert(mkdtemp(root));
    char script[512];
    assert(snprintf(script, sizeof(script), "%s/ffmpeg", root) <
           (int)sizeof(script));
    char pid_path[512];
    assert(snprintf(pid_path, sizeof(pid_path), "%s/child.pid", root) <
           (int)sizeof(pid_path));
    assert(setenv("R2_TEST_CHILD_PID_FILE", pid_path, 1) == 0);
    FILE *fp = fopen(script, "w");
    assert(fp);
    /* The input is one literal argv element even when it contains shell
     * metacharacters and quotes. Never interpolate it in a shell command. */
    assert(fputs(
        "#!/bin/sh\n"
        "while [ \"$#\" -gt 0 ]; do\n"
        "  if [ \"$1\" = \"-i\" ]; then\n"
        "    shift\n"
        "    if [ \"$1\" != \"$R2_TEST_FILE_NAME\" ]; then exit 17; fi\n"
        "  fi\n"
        "  shift\n"
        "done\n"
        "printf '%s\\n' \"$$\" > \"$R2_TEST_CHILD_PID_FILE\"\n"
        "if [ \"$R2_TEST_FILE_MODE\" = failed ]; then exit 23; fi\n"
        "if [ \"$R2_TEST_FILE_MODE\" = stalled ]; then\n"
        "  printf 'WXYZ'\n"
        "  exec sleep 60\n"
        "fi\n"
        "printf 'ABCDEFGH'\n", fp) >= 0);
    assert(fclose(fp) == 0 && chmod(script, 0700) == 0);
    const char *old_path = getenv("PATH");
    size_t length = strlen(root) + (old_path ? strlen(old_path) : 8U) + 2U;
    char *path = malloc(length);
    assert(path);
    assert(snprintf(path, length, "%s:%s", root,
                    old_path ? old_path : "/usr/bin") < (int)length);
    assert(setenv("PATH", path, 1) == 0);
    free(path);

    const char *unusual_filename = "/tmp/R2 song '$() ; strange filename.wav";
    assert(setenv("R2_TEST_FILE_NAME", unusual_filename, 1) == 0);
    R2Ears *ears = NULL;
    assert(r2_ears_init(&ears) == 0);
    assert(setenv("R2_TEST_FILE_MODE", "complete", 1) == 0);
    assert(r2_ears_open_file(ears, unusual_filename) == 0);
    char out[4];
    /* Invalid sub-frame requests must fail before consuming any decoder
     * bytes. Ears history and timestamps only accept complete S16LE frames. */
    errno = 0;
    assert(r2_audio_file_read(ears->file_stream, out, 3) == -1);
    assert(errno == EINVAL);
    assert(r2_ears_read(ears, out, sizeof(out)) == 4);
    assert(memcmp(out, "ABCD", 4) == 0);
    assert(r2_ears_read(ears, out, sizeof(out)) == 4);
    assert(memcmp(out, "EFGH", 4) == 0);
    assert(r2_ears_read(ears, out, sizeof(out)) == 0);
    assert(!r2_ears_is_open(ears));

    R2AudioFile *mono = r2_audio_file_open(unusual_filename, 44100, 1);
    assert(mono);
    assert(r2_audio_file_read(mono, out, 3) == -1 && errno == EINVAL);
    assert(r2_audio_file_read(mono, out, 2) == 2);
    assert(memcmp(out, "AB", 2) == 0);
    r2_audio_file_close(mono);

    /* Reopening from the exact event string previously freed it before
     * passing it to FFmpeg. Recreate while the old event is still owned. */
    assert(r2_ears_open_file(ears, ears->event.source_name) == 0);
    assert(r2_ears_read(ears, out, sizeof(out)) == 4);
    assert(memcmp(out, "ABCD", 4) == 0);
    r2_ears_close(ears);

    /* A failed decoder producing zero PCM must report EIO, not clean EOF. */
    assert(setenv("R2_TEST_FILE_MODE", "failed", 1) == 0);
    assert(r2_ears_open_file(ears, unusual_filename) == 0);
    assert(r2_ears_read(ears, out, sizeof(out)) == -1);
    assert(errno == EIO);
    r2_ears_close(ears);

    assert(setenv("R2_TEST_FILE_MODE", "stalled", 1) == 0);
    assert(r2_ears_open_file(ears, unusual_filename) == 0);
    assert(r2_ears_read(ears, out, sizeof(out)) == 4);
    assert(memcmp(out, "WXYZ", 4) == 0);
    Reader reader = {.ears = ears, .result = 0, .error = 0};
    atomic_init(&reader.begun, 0);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, stalled_reader, &reader) == 0);
    for (int i = 0; i < 1000 && !atomic_load(&reader.begun); ++i) {
        struct timespec pause = {0, 1000000L};
        (void)nanosleep(&pause, NULL);
    }
    assert(atomic_load(&reader.begun));
    struct timespec pause = {0, 30000000L};
    (void)nanosleep(&pause, NULL);
    struct timespec before, after;
    assert(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    r2_ears_cancel(ears);
    assert(pthread_join(thread, NULL) == 0);
    r2_ears_close(ears);
    assert(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    assert(reader.result == -1 && reader.error == ECANCELED);
    /* A direct-child FFmpeg PID was reaped, not left running or zombie. */
    FILE *pid_file = fopen(pid_path, "r");
    assert(pid_file);
    long child_pid = -1;
    assert(fscanf(pid_file, "%ld", &child_pid) == 1 && child_pid > 1);
    assert(fclose(pid_file) == 0);
    errno = 0;
    assert(kill((pid_t)child_pid, 0) == -1 && errno == ESRCH);
    assert(duration(before, after) < 2.0);
    r2_ears_shutdown(ears);
    assert(source_closes == 4); /* EOF, reopened file, failed decoder, cancellation */
    assert(unlink(pid_path) == 0);
    assert(unlink(script) == 0);
    assert(rmdir(root) == 0);
    puts("File decoder preserves exact argv, complete PCM, and promptly cancels stalled FFmpeg reads and reaps child.");
    return 0;
}
