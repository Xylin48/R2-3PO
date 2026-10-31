#define _GNU_SOURCE
#include "../Ears.c"
#include <assert.h>
#include <limits.h>

int64_t r2_log_sensory(const char *type, const char *summary,
                       const char *details, const char *source)
{
    (void)type; (void)summary; (void)details; (void)source;
    return 1;
}

static int cancel_after_poll(void *userdata)
{
    int *calls = userdata;
    return ++*calls >= 3;
}

/* This binary doubles as a controlled FFmpeg child, using the actual spawn,
 * argument transfer, pipe, read, wait, cancellation and cleanup implementation. */
static int decoder(int argc, char **argv)
{
    assert(argc == 17);
    assert(!strcmp(argv[5], "-i") && !strcmp(argv[6], getenv("R2_TEST_MEDIA")));
    assert(!strcmp(argv[10], "-ar") && !strcmp(argv[11], "48000"));
    assert(!strcmp(argv[12], "-ac") && !strcmp(argv[13], "2"));
    const char *mode = getenv("R2_TEST_DECODER_MODE");
    if (!strcmp(mode, "failed")) return 7;
    if (!strcmp(mode, "paused")) { for (;;) pause(); }
    unsigned char data[5368];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i % 251);
    size_t sent = 0;
    while (sent < sizeof(data)) {
        size_t count = sizeof(data) - sent;
        if (count > 137) count = 137;
        ssize_t n = write(STDOUT_FILENO, data + sent, count);
        assert(n > 0); sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) return decoder(argc, argv);
    char temp[] = "/tmp/r2-decoder-test-XXXXXX";
    assert(mkdtemp(temp));
    char executable[PATH_MAX], path[PATH_MAX], environment[PATH_MAX * 2];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    assert(length > 0); executable[length] = '\0';
    assert(snprintf(path, sizeof(path), "%s/ffmpeg", temp) < (int)sizeof(path));
    assert(symlink(executable, path) == 0);
    assert(snprintf(environment, sizeof(environment), "%s:%s", temp, getenv("PATH")) < (int)sizeof(environment));
    assert(setenv("PATH", environment, 1) == 0);
    /* Longer than the removed 4096-byte shell command, including shell syntax. */
    char media[8000];
    memset(media, 'x', sizeof(media) - 1); media[sizeof(media) - 1] = '\0';
    memcpy(media, "media ' $(literal); ", 19);
    assert(setenv("R2_TEST_MEDIA", media, 1) == 0);
    R2Ears *ears = NULL;
    assert(r2_ears_init(&ears) == 0);
    assert(setenv("R2_TEST_DECODER_MODE", "complete", 1) == 0);
    assert(r2_ears_open_file(ears, media) == 0);
    pid_t child = ears->file_pid;
    int fd = ears->file_fd;
    unsigned char data[4096];
    assert(r2_ears_read(ears, data, sizeof(data)) == 4096);
    for (size_t i = 0; i < sizeof(data); ++i) assert(data[i] == i % 251);
    assert(r2_ears_read(ears, data, sizeof(data)) == 1272);
    for (size_t i = 0; i < 1272; ++i) assert(data[i] == (i + 4096) % 251);
    assert(!r2_ears_is_open(ears));
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);

    assert(setenv("R2_TEST_DECODER_MODE", "failed", 1) == 0);
    assert(r2_ears_open_file(ears, media) == 0);
    assert(r2_ears_read(ears, data, sizeof(data)) == -1 && errno == EIO);
    assert(!r2_ears_is_open(ears));
    assert(setenv("R2_TEST_DECODER_MODE", "paused", 1) == 0);
    assert(r2_ears_open_file(ears, media) == 0);
    child = ears->file_pid; fd = ears->file_fd;
    int calls = 0;
    assert(r2_ears_read_cancelable(ears, data, sizeof(data), cancel_after_poll, &calls) == -1);
    assert(errno == ECANCELED);
    r2_ears_close(ears);
    assert(waitpid(child, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    assert(setenv("R2_TEST_DECODER_MODE", "complete", 1) == 0);
    assert(r2_ears_open_file(ears, media) == 0);
    assert(r2_ears_read(ears, data, sizeof(data)) == 4096);
    r2_ears_shutdown(ears);
    assert(unlink(path) == 0 && rmdir(temp) == 0);
    puts("Full decoder arguments, byte-exact PCM, failed exit, cancellation, reaping and reopen passed.");
    return 0;
}
