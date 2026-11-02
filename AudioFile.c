#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "AudioFile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <spawn.h>
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

extern char **environ;

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
    /* On Linux close(EINTR) still releases this fd. Retrying could close a
     * descriptor opened by a different thread after this call. */
    (void)close(old);
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
    /* Only the reader should be nonblocking: applying O_NONBLOCK to the
     * pipe writer makes FFmpeg fail with EAGAIN under normal backpressure. */
    if (pipe2(pipes, O_CLOEXEC) < 0) goto error;
    int flags = fcntl(pipes[0], F_GETFL);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto error;
    file->cancel_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (file->cancel_fd < 0) goto error;

    char rate[32], channel_count[32];
    snprintf(rate, sizeof(rate), "%u", sample_rate);
    snprintf(channel_count, sizeof(channel_count), "%u", channels);
    posix_spawn_file_actions_t actions;
    int spawn_rc = posix_spawn_file_actions_init(&actions);
    if (spawn_rc != 0) { errno = spawn_rc; goto error; }
    spawn_rc = posix_spawn_file_actions_addclose(&actions, pipes[0]);
    if (spawn_rc == 0)
        spawn_rc = posix_spawn_file_actions_addclose(&actions, file->cancel_fd);
    if (spawn_rc == 0)
        spawn_rc = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (spawn_rc == 0 && pipes[1] != STDOUT_FILENO)
        spawn_rc = posix_spawn_file_actions_addclose(&actions, pipes[1]);
    pid_t child = -1;
    if (spawn_rc == 0) {
        char *const argv[] = {
            "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
            "-i", (char *)path, "-vn", "-f", "s16le", "-ar", rate,
            "-ac", channel_count, "-acodec", "pcm_s16le", "pipe:1", NULL
        };
        /* Safe from a multithreaded program; FFmpeg receives a single
         * filename argument, without a shell or inherited cancel pipe. */
        spawn_rc = posix_spawnp(&child, argv[0], &actions, NULL, argv, environ);
    }
    (void)posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) { errno = spawn_rc; goto error; }
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

/* On pipe EOF, distinguish a decoder that exited successfully from one
 * which failed to decode the file. Its output pipe can close just before
 * exit(2), so allow a short cancellable grace period for the exit status.
 * A process that deliberately continues after closing stdout is left to
 * the lifecycle owner to terminate rather than blocking the reader. */
static int r2_file_check_eof(R2AudioFile *file)
{
    if (file->child <= 0) return 0;
    uint64_t start = r2_file_now_ms();
    for (;;) {
        int status = 0;
        pid_t result;
        do { result = waitpid(file->child, &status, WNOHANG); }
        while (result < 0 && errno == EINTR);
        if (result == file->child) {
            file->child = -1; /* reaped; owner must never signal this PID */
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                errno = EIO;
                return -1;
            }
            return 0;
        }
        if (result < 0) {
            if (errno == ECHILD) { file->child = -1; return 0; }
            return -1;
        }
        if (atomic_load(&file->cancelled)) {
            errno = ECANCELED;
            return -1;
        }
        uint64_t now = r2_file_now_ms();
        if (start == UINT64_MAX || now == UINT64_MAX ||
            now < start || now - start >= 150U)
            return 0; /* EOF certain, child exit status not yet known */
        struct timespec pause = {0, 5000000L};
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
    }
}

ssize_t r2_audio_file_read(R2AudioFile *file, void *buffer, size_t bytes)
{
    if (!file || !buffer || !bytes || bytes > SSIZE_MAX || (bytes % 4U) != 0U) {
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
        if (copied > 0 && copied % 4U == 0)
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
            if (copied == 0 && r2_file_check_eof(file) < 0) return -1;
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
