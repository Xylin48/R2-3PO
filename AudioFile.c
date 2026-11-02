#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "AudioFile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct R2AudioFile {
    int pcm_fd;             /* nonblocking read end, owned by this object */
    int cancel_fd;          /* eventfd wakes an indefinitely silent decoder */
    pid_t child;            /* direct FFmpeg child, NOT a shell PID */
    atomic_int cancelled;
};

static void r2_file_close_fd(int *fd)
{
    if (*fd < 0) return;
    int old = *fd;
    *fd = -1;
    while (close(old) < 0 && errno == EINTR) {}
}

static uint64_t r2_file_now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

R2AudioFile *r2_audio_file_open(const char *path, uint32_t sample_rate,
                                unsigned channels)
{
    if (!path || !*path || !sample_rate || !channels) {
        errno = EINVAL;
        return NULL;
    }
    R2AudioFile *file = calloc(1, sizeof(*file));
    if (!file) return NULL;
    file->pcm_fd = file->cancel_fd = -1;
    file->child = -1;
    atomic_init(&file->cancelled, 0);

    int pipes[2] = {-1, -1};
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) < 0) goto error;
    file->cancel_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (file->cancel_fd < 0) goto error;

    char rate[32], channel_count[32];
    snprintf(rate, sizeof(rate), "%u", sample_rate);
    snprintf(channel_count, sizeof(channel_count), "%u", channels);
    pid_t child = fork();
    if (child < 0) goto error;
    if (child == 0) {
        /* The child never inherits the reader or cancellation channel.
         * No shell interpolation: the entire filename is one argv entry. */
        close(pipes[0]);
        close(file->cancel_fd);
        if (dup2(pipes[1], STDOUT_FILENO) < 0) _exit(127);
        close(pipes[1]);
        char *const argv[] = {
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-i", (char *)path, "-vn", "-f", "s16le", "-ar", rate,
            "-ac", channel_count, "-acodec", "pcm_s16le", "pipe:1", NULL
        };
        execvp(argv[0], argv);
        _exit(127);
    }
    file->child = child;
    file->pcm_fd = pipes[0];
    pipes[0] = -1;
    r2_file_close_fd(&pipes[1]);
    return file;

error:
    {
        int saved = errno;
        r2_file_close_fd(&pipes[0]);
        r2_file_close_fd(&pipes[1]);
        r2_file_close_fd(&file->cancel_fd);
        free(file);
        errno = saved;
        return NULL;
    }
}

ssize_t r2_audio_file_read(R2AudioFile *file, void *buffer, size_t bytes)
{
    if (!file || !buffer || !bytes || bytes > SSIZE_MAX) {
        errno = EINVAL;
        return -1;
    }
    size_t copied = 0;
    for (;;) {
        if (atomic_load(&file->cancelled)) {
            errno = ECANCELED;
            return -1;
        }
        /* Do not return a partial stereo 16-bit frame. The caller's ring
         * history relies on complete four-byte frames. */
        if (copied > 0 && (copied % 4U == 0 || copied == bytes))
            return (ssize_t)copied;
        struct pollfd pfd[2] = {
            {.fd = file->cancel_fd, .events = POLLIN},
            {.fd = file->pcm_fd, .events = POLLIN}
        };
        int ready;
        do { ready = poll(pfd, 2, -1); }
        while (ready < 0 && errno == EINTR && !atomic_load(&file->cancelled));
        if (atomic_load(&file->cancelled)) {
            errno = ECANCELED;
            return -1;
        }
        if (ready < 0) return -1;
        if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECANCELED;
            return -1;
        }
        if (pfd[1].revents & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
        if (!(pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        ssize_t got = read(file->pcm_fd, (unsigned char *)buffer + copied,
                           bytes - copied);
        if (got > 0) {
            copied += (size_t)got;
            continue;
        }
        if (got == 0) {
            if (copied % 4U != 0) { errno = EPROTO; return -1; }
            return (ssize_t)copied;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return -1;
    }
}

void r2_audio_file_cancel(R2AudioFile *file)
{
    if (!file) return;
    atomic_store(&file->cancelled, 1);
    const uint64_t wake = 1;
    ssize_t written;
    do { written = write(file->cancel_fd, &wake, sizeof(wake)); }
    while (written < 0 && errno == EINTR);
    /* EAGAIN means the event is already signalled, which is sufficient. */
    (void)written;
}

void r2_audio_file_close(R2AudioFile *file)
{
    if (!file) return;
    /* Never close the poll descriptors until the sole reader has exited. */
    r2_audio_file_cancel(file);
    r2_file_close_fd(&file->pcm_fd);
    r2_file_close_fd(&file->cancel_fd);

    if (file->child > 0) {
        int status;
        pid_t result;
        do { result = waitpid(file->child, &status, WNOHANG); }
        while (result < 0 && errno == EINTR);
        if (result == 0) {
            (void)kill(file->child, SIGTERM);
            uint64_t start = r2_file_now_ms();
            for (;;) {
                do { result = waitpid(file->child, &status, WNOHANG); }
                while (result < 0 && errno == EINTR);
                if (result != 0) break;
                uint64_t now = r2_file_now_ms();
                if (start == UINT64_MAX || now == UINT64_MAX ||
                    now < start || now - start >= 250U) break;
                struct timespec pause = {0, 10000000L};
                while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
            }
            if (result == 0) {
                (void)kill(file->child, SIGKILL);
                /* Direct child is now guaranteed to receive a hard stop.
                 * Reap it before releasing the decoder's ownership. */
                do { result = waitpid(file->child, &status, 0); }
                while (result < 0 && errno == EINTR);
            }
        }
    }
    free(file);
}
