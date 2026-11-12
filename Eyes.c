#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Eyes.h"
#include "Log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <glob.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>

/*
 * R2-3PO Eyes
 *
 * IMPORTANT:
 *
 * R2's EYES are NOT limited to 640x480.
 *
 * 640x480 is the resolution used when R2 chooses to RECORD /
 * retain a visual moment.
 *
 * His visual input remains at the source's native resolution.
 *
 * Example:
 *
 *     1920x1080 movie
 *             |
 *             v
 *          R2 EYES
 *             |
 *       1920x1080 RGB
 *             |
 *       vision/cognition
 *             |
 *       "I want THAT"
 *             |
 *             v
 *        640x480 capture
 *
 * This means R2 never has visual information thrown away before
 * his vision system has an opportunity to examine it.
 *
 * Eyes itself does NOT:
 *
 *     - recognize faces
 *     - identify objects
 *     - read text
 *     - describe scenes
 *     - determine emotions
 *     - choose what R2 should look at
 *     - create memories
 *
 * Eyes only supplies the visual stream.
 */

/* Recording resolution */
#define R2_EYES_RECORD_WIDTH       640
#define R2_EYES_RECORD_HEIGHT      480
#define R2_EYES_RECORD_BPP         24
#define R2_EYES_RECORD_CHANNELS    3
#define R2_EYES_RECORD_SIZE \
    ((size_t)R2_EYES_RECORD_WIDTH * \
     (size_t)R2_EYES_RECORD_HEIGHT * \
     R2_EYES_RECORD_CHANNELS)

/* Maximum native frame size */
#define R2_EYES_MAX_FRAME_BYTES \
    ((size_t)7680 * (size_t)4320 * 3)

/* Sampling rate */
#define R2_EYES_FPS 10

struct R2Eyes
{
    FILE *stream;
    pid_t stream_child; /* Direct FFmpeg child, rather than a popen shell. */
    atomic_int open;
    int source_is_live;
    R2VisionOrigin origin;
    R2VideoFormat format;

    unsigned char *frame_buffer;
    size_t frame_size;
    int frame_ready; /* Pixels and frame metadata describe one complete capture. */
    size_t pending_frame_bytes; /* Retained across a cancelled reader, not across source close. */

    unsigned char *record_buffer;
    size_t record_size;

    uint64_t frame_count;
    uint64_t timestamp;
    uint64_t last_log_timestamp;
    uint64_t preview_session;
    int auxiliary_display; /* No V-Webcam takeover: only real Eyes drives it. */

    R2VisionEvent event;
    R2VisionFrame frame;

    char *source_name;
};

/* Display-only mirror for V-Webcam. It reads the same Eyes frames but never
   feeds pixels back into perception or changes the active source. */
#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"
#define R2_VWEBCAM_MAX_W 960U
#define R2_VWEBCAM_MAX_H 540U
static _Atomic uint64_t active_preview_session;

uint64_t r2_eyes_preview_session(void)
{
    return atomic_load(&active_preview_session);
}

static pid_t r2_vwebcam_pid = -1;
static uint64_t r2_vwebcam_last_publish_ms = 0;
static uint64_t r2_vwebcam_last_status_ms = 0;

static int r2_vwebcam_prepare_dir(void)
{
    struct stat st;
    if (mkdir(R2_VWEBCAM_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(R2_VWEBCAM_DIR, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid())
        return -1;
    if ((st.st_mode & 0777) != 0700 && chmod(R2_VWEBCAM_DIR, 0700) != 0)
        return -1;
    return 0;
}

static void r2_vwebcam_write_status(const R2Eyes *eyes, int active)
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display)
        return;
    uint64_t now_ms = eyes ? eyes->timestamp : 0;
    if (active && eyes && eyes->frame_count == 0) {
        r2_vwebcam_last_publish_ms = 0;
        r2_vwebcam_last_status_ms = 0;
    } else if (active && now_ms && r2_vwebcam_last_status_ms &&
               now_ms >= r2_vwebcam_last_status_ms &&
               now_ms - r2_vwebcam_last_status_ms < 500) {
        return;
    }
    if (r2_vwebcam_prepare_dir() != 0)
        return;
    if (active && now_ms)
        r2_vwebcam_last_status_ms = now_ms;
    char tmp[PATH_MAX], path[PATH_MAX];
    int tn = snprintf(tmp, sizeof(tmp), "%s/status.tmp", R2_VWEBCAM_DIR);
    int pn = snprintf(path, sizeof(path), "%s/status.txt", R2_VWEBCAM_DIR);
    if (tn < 0 || (size_t)tn >= sizeof(tmp) || pn < 0 || (size_t)pn >= sizeof(path))
        return;

    const char *raw = eyes && eyes->source_name ? eyes->source_name : "No visual source";
    FILE *fp = fopen(tmp, "w");
    if (!fp)
        return;
    int failed = fprintf(fp, "%d\n", active ? 1 : 0) < 0;
    /* Retain the legacy one-line display label and append the exact bytes
       after the existing fields. Both forms travel in the same atomic file. */
    for (const unsigned char *c = (const unsigned char *)raw; *c && !failed; ++c)
        failed = fputc(*c == '\n' || *c == '\r' ? ' ' : *c, fp) == EOF;
    if (!failed)
        failed = fprintf(fp, "\n%u\n%u\n%llu\n%llu\n%llu\nsource_hex=",
            eyes ? eyes->format.width : 0, eyes ? eyes->format.height : 0,
            (unsigned long long)(eyes ? eyes->frame_count : 0),
            (unsigned long long)(eyes ? eyes->timestamp : 0),
            (unsigned long long)(eyes ? eyes->preview_session : 0)) < 0;
    for (const unsigned char *c = (const unsigned char *)raw; *c && !failed; ++c)
        failed = fprintf(fp, "%02x", (unsigned)*c) < 0;
    if (!failed)
        failed = fputc('\n', fp) == EOF;
    if (fclose(fp) != 0)
        failed = 1;
    if (!failed) {
        (void)chmod(tmp, 0600);
        if (rename(tmp, path) != 0)
            failed = 1;
    }
    if (failed)
        (void)unlink(tmp);

    if (!active || !eyes || eyes->frame_count == 0) {
        char focus[PATH_MAX];
        int n = snprintf(focus, sizeof(focus), "%s/focus.json", R2_VWEBCAM_DIR);
        if (n > 0 && (size_t)n < sizeof(focus))
            (void)unlink(focus);
    }
}

static void r2_vwebcam_begin_session(R2Eyes *eyes)
{
    uint64_t session = 0;
    ssize_t size;
    do {
        size = getrandom(&session, sizeof(session), GRND_NONBLOCK);
    } while (size < 0 && errno == EINTR);
    /* A failed token allocation leaves the preview unavailable for this open. */
    eyes->preview_session = size == (ssize_t)sizeof(session) ? session : 0;
    atomic_store(&active_preview_session, eyes->preview_session);
    r2_vwebcam_write_status(eyes, 1);
}

static void r2_vwebcam_publish_frame(const R2Eyes *eyes)
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display)
        return;
    if (eyes && eyes->timestamp && r2_vwebcam_last_publish_ms &&
        eyes->timestamp >= r2_vwebcam_last_publish_ms &&
        eyes->timestamp - r2_vwebcam_last_publish_ms < 100)
        return;
    if (!eyes || !eyes->open || !eyes->frame_buffer ||
        !eyes->preview_session || eyes->preview_session != r2_eyes_preview_session() ||
        !eyes->format.width || !eyes->format.height ||
        r2_vwebcam_prepare_dir() != 0)
        return;

    size_t width = eyes->format.width, height = eyes->format.height;
    if (height > SIZE_MAX / width || width * height > SIZE_MAX / 3U ||
        eyes->frame_size < width * height * 3U)
        return;

    double scale = 1.0;
    if (width > R2_VWEBCAM_MAX_W || height > R2_VWEBCAM_MAX_H) {
        double sx = (double)R2_VWEBCAM_MAX_W / (double)width;
        double sy = (double)R2_VWEBCAM_MAX_H / (double)height;
        scale = sx < sy ? sx : sy;
    }
    unsigned int dw = (unsigned int)(width * scale);
    unsigned int dh = (unsigned int)(height * scale);
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    if (dw > R2_VWEBCAM_MAX_W || dh > R2_VWEBCAM_MAX_H)
        return;

    char tmp[PATH_MAX], path[PATH_MAX];
    int tn = snprintf(tmp, sizeof(tmp), "%s/frame.tmp.ppm", R2_VWEBCAM_DIR);
    int pn = snprintf(path, sizeof(path), "%s/frame.ppm", R2_VWEBCAM_DIR);
    if (tn < 0 || (size_t)tn >= sizeof(tmp) || pn < 0 || (size_t)pn >= sizeof(path))
        return;
    FILE *fp = fopen(tmp, "wb");
    if (!fp)
        return;
    int failed = fprintf(fp, "P6\n# r2-preview %llu\n%u %u\n255\n",
                         (unsigned long long)eyes->preview_session, dw, dh) < 0;
    unsigned char row[R2_VWEBCAM_MAX_W * 3U];
    for (unsigned int y = 0; !failed && y < dh; ++y) {
        size_t sy = ((size_t)y * height) / dh;
        const unsigned char *src = eyes->frame_buffer + sy * width * 3U;
        for (unsigned int x = 0; x < dw; ++x) {
            size_t sx = ((size_t)x * width) / dw;
            row[x * 3U] = src[sx * 3U];
            row[x * 3U + 1U] = src[sx * 3U + 1U];
            row[x * 3U + 2U] = src[sx * 3U + 2U];
        }
        if (fwrite(row, 3U, dw, fp) != dw)
            failed = 1;
    }
    if (fclose(fp) != 0)
        failed = 1;
    if (!failed) {
        (void)chmod(tmp, 0600);
        if (rename(tmp, path) != 0)
            failed = 1;
    }
    if (failed)
        (void)unlink(tmp);
    else if (eyes->timestamp)
        r2_vwebcam_last_publish_ms = eyes->timestamp;
}

static void r2_vwebcam_start_viewer(void)
{
    const char *display = getenv("DISPLAY");
    if (!display || !*display || r2_vwebcam_prepare_dir() != 0)
        return;
    if (r2_vwebcam_pid > 0) {
        int status = 0;
        pid_t ended;
        do {
            ended = waitpid(r2_vwebcam_pid, &status, WNOHANG);
        } while (ended < 0 && errno == EINTR);
        if (ended == 0)
            return;
        if (ended == r2_vwebcam_pid || (ended < 0 && errno == ECHILD))
            r2_vwebcam_pid = -1;
        else
            return;
    }

    /* Resolve all environment/path state before fork. The child of this
       multithreaded process then uses only async-signal-safe calls until exec. */
    const char *source_dir = getenv("R2_SOURCE");
    if (!source_dir || !*source_dir)
        source_dir = "/home/x/R2_Home/R2";
    char viewer[PATH_MAX];
    int n = snprintf(viewer, sizeof(viewer), "%s/V-Webcam.py", source_dir);
    if (n < 0 || (size_t)n >= sizeof(viewer))
        return;
    char *const argv[] = {"/usr/bin/python3", viewer, NULL};
    pid_t parent_pid = getpid();

    pid_t child = fork();
    if (child < 0)
        return;
    if (child == 0) {
        /* A detached viewer must not outlive the R2 process if it crashes or
           is terminated before the normal Eyes shutdown path can reap it. */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_pid)
            _exit(126);
        (void)setsid();
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            (void)dup2(fd, STDIN_FILENO);
            (void)dup2(fd, STDOUT_FILENO);
            (void)dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    r2_vwebcam_pid = child;
}

static void r2_eyes_close_stream(FILE *stream, pid_t *child);

static void r2_vwebcam_stop_viewer(void)
{
    r2_eyes_close_stream(NULL, &r2_vwebcam_pid);
}

static uint64_t r2_eyes_timestamp(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return ((uint64_t)ts.tv_sec * 1000ULL) +
           ((uint64_t)ts.tv_nsec / 1000000ULL);
}

/* Read-only discovery must not wait indefinitely for ffprobe or X11. The
 * five-second read budget applies to an entire metadata operation, including
 * all candidates in one window lookup, followed by bounded child cleanup. */
#define R2_EYES_PROBE_MS 5000U

/* A live decoder normally supplies a frame every 100 ms. Allow startup and
 * scheduling delays, but close a live pipe that cannot supply a whole frame.
 * File playback retains its cancellable wait without a live-frame deadline. */
#define R2_EYES_LIVE_FRAME_MS 5000U
static uint64_t r2_eyes_monotonic_ms(void);
typedef struct {
    uint64_t deadline;
    int (*cancelled)(void *);
    void *userdata;
} R2EyesProbe;

static char *r2_eyes_query(char *const argv[], R2EyesProbe probe, int search);
static uint64_t r2_eyes_probe_deadline(void)
{
    uint64_t now = r2_eyes_monotonic_ms();
    return now >= UINT64_MAX - R2_EYES_PROBE_MS ? 0 : now + R2_EYES_PROBE_MS;
}
static int r2_eyes_probe_expired(uint64_t deadline)
{
    uint64_t now = r2_eyes_monotonic_ms();
    return !deadline || now == UINT64_MAX || now >= deadline;
}

static int r2_eyes_probe_check(R2EyesProbe probe)
{
    if (probe.cancelled && probe.cancelled(probe.userdata)) {
        errno = ECANCELED;
        return -1;
    }
    if (r2_eyes_probe_expired(probe.deadline)) {
        errno = ETIMEDOUT;
        return -1;
    }
    return 0;
}

static int r2_eyes_probe_dimensions(
    const char *source,
    int live_device,
    uint32_t *width,
    uint32_t *height)
{
    if (!source || !width || !height)
        return -1;

    if (live_device)
    {
        *width = 640;
        *height = 480;

        return 0;
    }

    char *argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x",
        (char *)source, NULL};
    char *dimensions = r2_eyes_query(argv, (R2EyesProbe){.deadline = r2_eyes_probe_deadline()}, 0);
    if (!dimensions) return -1;

    char *separator = strchr(dimensions, 'x');
    uint32_t w = 0, h = 0;
    int valid = 0;
    if (separator) {
        *separator = '\0';
        errno = 0;
        char *end;
        unsigned long a = strtoul(dimensions, &end, 10);
        while (*end && isspace((unsigned char)*end)) ++end;
        if (!errno && end != dimensions && !*end && a && a <= UINT32_MAX) {
            w = (uint32_t)a;
            const char *height_text = separator + 1;
            unsigned long b = strtoul(height_text, &end, 10);
            while (*end && isspace((unsigned char)*end)) ++end;
            if (!errno && end != height_text && !*end && b && b <= UINT32_MAX)
                h = (uint32_t)b;
        }
        valid = w && h && (size_t)w <= R2_EYES_MAX_FRAME_BYTES / 3U / (size_t)h;
    }
    free(dimensions);
    if (!valid) { errno = EINVAL; return -1; }
    *width = w;
    *height = h;
    return 0;
}

static int r2_eyes_allocate_native_buffer(
    R2Eyes *eyes,
    uint32_t width,
    uint32_t height)
{
    if (!eyes || width == 0 || height == 0)
        return -1;

    if ((size_t)width > R2_EYES_MAX_FRAME_BYTES / 3U / (size_t)height)
        return -1;

    size_t frame_size =
        (size_t)width *
        (size_t)height *
        3U;

    if (frame_size > R2_EYES_MAX_FRAME_BYTES)
        return -1;

    unsigned char *buffer =
        malloc(frame_size);

    if (!buffer)
        return -1;

    free(eyes->frame_buffer);

    eyes->frame_buffer = buffer;
    eyes->frame_size = frame_size;

    eyes->format.width = width;
    eyes->format.height = height;
    eyes->format.stride = width * 3;
    eyes->format.bits_per_pixel = 24;
    eyes->format.pixel_format = R2_PIXEL_RGB24;

    return 0;
}

static int r2_eyes_allocate_record_buffer(R2Eyes *eyes)
{
    if (!eyes)
        return -1;

    if (!eyes->record_buffer)
    {
        eyes->record_buffer =
            malloc(R2_EYES_RECORD_SIZE);

        if (!eyes->record_buffer)
            return -1;
    }

    eyes->record_size =
        R2_EYES_RECORD_SIZE;

    memset(
        eyes->record_buffer,
        0,
        eyes->record_size
    );

    return 0;
}

static uint64_t r2_eyes_monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return UINT64_MAX;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/* Only the reader is nonblocking: the writer retains normal backpressure.
 * A private process group also permits cleanup of a decoder's helper children. */
typedef enum { R2_EYES_STDERR_INHERIT, R2_EYES_STDERR_DISCARD, R2_EYES_STDERR_CAPTURE } R2EyesStderr;

static FILE *r2_eyes_spawn_stream(char *const argv[], pid_t *child_out, R2EyesStderr diagnostics)
{
    extern char **environ;
    int pipes[2];
    *child_out = -1;
    if (pipe2(pipes, O_CLOEXEC) != 0) return NULL;
    int flags = fcntl(pipes[0], F_GETFL);
    if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) < 0)
        goto failed;
    FILE *stream = fdopen(pipes[0], "r");
    if (!stream) goto failed;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) { errno = rc; fclose(stream); close(pipes[1]); return NULL; }
    rc = posix_spawnattr_init(&attr);
    if (rc != 0) {
        posix_spawn_file_actions_destroy(&actions);
        fclose(stream); close(pipes[1]); errno = rc; return NULL;
    }
    rc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (rc == 0) rc = posix_spawnattr_setpgroup(&attr, 0);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&actions, pipes[0]);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (rc == 0 && pipes[1] != STDOUT_FILENO)
        rc = posix_spawn_file_actions_addclose(&actions, pipes[1]);
    if (rc == 0 && diagnostics == R2_EYES_STDERR_DISCARD)
        rc = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    if (rc == 0 && diagnostics == R2_EYES_STDERR_CAPTURE)
        rc = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (rc == 0)
        rc = posix_spawnp(child_out, argv[0], &actions, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (rc != 0) { fclose(stream); errno = rc; return NULL; }
    return stream;
failed:
    {
        int saved = errno;
        close(pipes[0]); close(pipes[1]); errno = saved;
        return NULL;
    }
}

/* Called after the reader ends. Terminate a stalled child with bounded
 * graceful cleanup, then reap it before releasing its PID ownership. */
static void r2_eyes_close_stream(FILE *stream, pid_t *child)
{
    if (stream) fclose(stream);
    if (*child <= 0) return;
    int status;
    pid_t result;
    do { result = waitpid(*child, &status, WNOHANG); }
    while (result < 0 && errno == EINTR);
    if (result == 0) {
        (void)kill(-*child, SIGTERM);
        (void)kill(*child, SIGTERM); /* The owned child may have changed groups. */
        uint64_t start = r2_eyes_monotonic_ms();
        for (;;) {
            do { result = waitpid(*child, &status, WNOHANG); }
            while (result < 0 && errno == EINTR);
            if (result != 0) break;
            uint64_t now = r2_eyes_monotonic_ms();
            if (start == UINT64_MAX || now == UINT64_MAX ||
                now < start || now - start >= 250U) break;
            struct timespec pause = {0, 10000000L};
            while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
        }
        if (result == 0) {
            (void)kill(-*child, SIGKILL);
            (void)kill(*child, SIGKILL);
            do { result = waitpid(*child, &status, 0); }
            while (result < 0 && errno == EINTR);
        }
    }
    *child = -1;
}

static int r2_eyes_stream_eof(R2Eyes *eyes,
                             int (*cancelled)(void *), void *userdata, int *exit_code)
{
    /* Pipe EOF alone does not establish a successful decoder exit. */
    if (eyes->stream_child <= 0) { errno = EIO; return -1; }
    uint64_t start = r2_eyes_monotonic_ms();
    for (;;) {
        if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
        int status;
        pid_t result;
        do { result = waitpid(eyes->stream_child, &status, WNOHANG); }
        while (result < 0 && errno == EINTR);
        if (result == eyes->stream_child) {
            eyes->stream_child = -1;
            if (WIFEXITED(status)) {
                if (exit_code) {
                    *exit_code = WEXITSTATUS(status);
                    return 0;
                }
                if (WEXITSTATUS(status) == 0) return 0;
            }
            errno = EIO; return -1;
        }
        if (result < 0) return -1;
        uint64_t now = r2_eyes_monotonic_ms();
        if (start == UINT64_MAX || now == UINT64_MAX ||
            now < start || now - start >= 250U) { errno = ETIMEDOUT; return -1; }
        struct timespec pause = {0, 5000000L};
        while (nanosleep(&pause, &pause) < 0 && errno == EINTR) {}
    }
}

static int r2_eyes_read_exact(R2Eyes *eyes, unsigned char *buffer, size_t size,
                              int (*cancelled)(void *), void *userdata)
{
    size_t total = eyes->pending_frame_bytes;
    uint64_t start = 0;
    if (eyes->source_is_live) {
        start = r2_eyes_monotonic_ms();
        if (start == UINT64_MAX) return -1;
    }
    while (total < size) {
        if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
        int wait_ms = 100;
        if (eyes->source_is_live) {
            uint64_t now = r2_eyes_monotonic_ms();
            if (now == UINT64_MAX) return -1;
            if (now - start >= R2_EYES_LIVE_FRAME_MS) {
                errno = ETIMEDOUT;
                return -1;
            }
            uint64_t remaining = R2_EYES_LIVE_FRAME_MS - (now - start);
            if (remaining < (uint64_t)wait_ms) wait_ms = (int)remaining;
        }
        struct pollfd input = {.fd = fileno(eyes->stream), .events = POLLIN};
        int ready = poll(&input, 1, wait_ms);
        if (ready < 0) { if (errno == EINTR) continue; return -1; }
        if (!ready) continue;
        if (cancelled && cancelled(userdata)) { errno = ECANCELED; return -1; }
        if (input.revents & POLLNVAL) { errno = EBADF; return -1; }
        ssize_t got = read(input.fd, buffer + total, size - total);
        if (got > 0) {
            total += (size_t)got;
            eyes->pending_frame_bytes = total;
            continue;
        }
        if (got == 0) {
            if (total) { errno = EPROTO; return -1; }
            return r2_eyes_stream_eof(eyes, cancelled, userdata, NULL);
        }
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    eyes->pending_frame_bytes = 0;
    return 1;
}

/* Retain the complete query output, require a successful child exit, and
 * discard the whole snapshot on timeout/read/allocation failure. */
static char *r2_eyes_query(char *const argv[], R2EyesProbe probe, int search)
{
    if (r2_eyes_probe_check(probe) != 0) return NULL;
    R2Eyes process = {0};
    process.stream = r2_eyes_spawn_stream(argv, &process.stream_child,
        search ? R2_EYES_STDERR_CAPTURE : R2_EYES_STDERR_DISCARD);
    if (!process.stream) return NULL;
    char *output = NULL;
    size_t length = 0, capacity = 0;
    for (;;) {
        if (r2_eyes_probe_check(probe) != 0) break;
        struct pollfd input = {.fd = fileno(process.stream), .events = POLLIN};
        int ready = poll(&input, 1, 50);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (!ready) continue;
        if (input.revents & POLLNVAL) { errno = EBADF; break; }
        char block[4096];
        ssize_t got = read(input.fd, block, sizeof(block));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (got == 0) {
            int exit_code;
            if (r2_eyes_stream_eof(&process, probe.cancelled, probe.userdata, &exit_code) < 0 ||
                r2_eyes_probe_check(probe) != 0)
                break;
            /* xdotool search returns 1 for no match. Accept it only if
               both stdout and captured stderr are empty. */
            if (exit_code && !(search && exit_code == 1 && !length)) {
                errno = EIO;
                break;
            }
            if (!output) output = strdup("");
            r2_eyes_close_stream(process.stream, &process.stream_child);
            return output;
        }
        if (memchr(block, 0, (size_t)got)) { errno = EPROTO; break; }
        if ((size_t)got > SIZE_MAX - length - 1U) { errno = EOVERFLOW; break; }
        size_t needed = length + (size_t)got + 1U;
        if (needed > capacity) {
            size_t next = capacity ? capacity : 4096U;
            while (next < needed) {
                if (next > SIZE_MAX / 2U) { next = needed; break; }
                next *= 2U;
            }
            char *grown = realloc(output, next);
            if (!grown) break;
            output = grown;
            capacity = next;
        }
        memcpy(output + length, block, (size_t)got);
        length += (size_t)got;
        output[length] = '\0';
    }
    int saved = errno;
    free(output);
    r2_eyes_close_stream(process.stream, &process.stream_child);
    errno = saved;
    return NULL;
}

static void r2_eyes_reset_frame(R2Eyes *eyes)
{
    if (!eyes)
        return;

    memset(
        &eyes->frame,
        0,
        sizeof(eyes->frame)
    );

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;

    eyes->frame.format =
        eyes->format;

    eyes->frame.frame_number =
        eyes->frame_count;

    eyes->frame.timestamp =
        eyes->timestamp;
}

static void r2_eyes_reset_event(R2Eyes *eyes)
{
    if (!eyes)
        return;

    memset(
        &eyes->event,
        0,
        sizeof(eyes->event)
    );

    eyes->event.origin =
        eyes->origin;

    eyes->event.type =
        R2_VISUAL_UNKNOWN;

    eyes->event.format =
        eyes->format;

    eyes->event.timestamp =
        eyes->timestamp;

    eyes->event.frame_count =
        eyes->frame_count;
}

static FILE *r2_eyes_start_ffmpeg(const char *input, int live_device, pid_t *child_out)
{
    if (!input) return NULL;
    char fps[32], filter[64];
    snprintf(fps, sizeof(fps), "%d", R2_EYES_FPS);
    snprintf(filter, sizeof(filter), "fps=%d,format=rgb24", R2_EYES_FPS);
    char *camera[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-f", "v4l2", "-framerate", fps, "-video_size", "640x480", "-i", (char *)input,
        "-vf", "format=rgb24", "-f", "rawvideo", "-pix_fmt", "rgb24", "-r", fps,
        "pipe:1", NULL};
    char *file[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-re", "-i", (char *)input, "-vf", filter, "-f", "rawvideo", "-pix_fmt",
        "rgb24", "pipe:1", NULL};
    return r2_eyes_spawn_stream(live_device ? camera : file, child_out, R2_EYES_STDERR_INHERIT);
}

/* ============================================================
   VLC SUPPORT
   ============================================================ */

/*
 * Find a visible VLC window through X11.
 *
 * Eyes captures the VLC window itself, NOT the entire desktop.
 */
static int r2_eyes_parse_window_id(const char *cursor, char **end, unsigned long *id)
{
    if (*cursor < '0' || *cursor > '9') return -1;
    errno = 0;
    unsigned long value = strtoul(cursor, end, 10);
    if (errno || !value || value > UINT32_MAX ||
        (**end && !isspace((unsigned char)**end))) return -1;
    *id = value;
    return 0;
}

static int r2_eyes_probe_vlc_dimensions(unsigned long window_id,
                                        uint32_t *width, uint32_t *height,
                                        R2EyesProbe probe)
{
    if (!width || !height || !window_id || window_id > UINT32_MAX) return -1;
    char window[32];
    snprintf(window, sizeof(window), "%lu", window_id);
    char *argv[] = {"xdotool", "getwindowgeometry", "--shell", window, NULL};
    char *output = r2_eyes_query(argv, probe, 0);
    if (!output) return -1;
    uint32_t w = 0, h = 0;
    int valid = 1;
    char *save = NULL;
    for (char *line = strtok_r(output, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        uint32_t *target = !strncmp(line, "WIDTH=", 6) ? &w :
                           !strncmp(line, "HEIGHT=", 7) ? &h : NULL;
        if (!target) continue;
        const char *text = strchr(line, '=') + 1;
        char *end;
        errno = 0;
        unsigned long value = strtoul(text, &end, 10);
        while (*end && isspace((unsigned char)*end)) ++end;
        if (errno || end == text || *end || !value || value > UINT32_MAX) {
            valid = 0; break;
        }
        *target = (uint32_t)value;
    }
    free(output);
    if (!valid || !w || !h || (size_t)w > R2_EYES_MAX_FRAME_BYTES / 3U / (size_t)h) {
        errno = EINVAL; return -1;
    }
    *width = w;
    *height = h;
    return 0;
}

/* A failed or incomplete search must never be interpreted as a withdrawal. */
static int r2_eyes_find_window(char *const argv[], R2EyesProbe probe,
                               unsigned long preferred, unsigned long *window)
{
    *window = 0;
    char *output = r2_eyes_query(argv, probe, 1);
    if (!output) return -1;
    unsigned long best = 0;
    uint64_t best_area = 0;
    int result = 0;
    char *cursor = output;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        char *end;
        unsigned long candidate;
        if (r2_eyes_parse_window_id(cursor, &end, &candidate) != 0) {
            errno = EPROTO;
            result = -1;
            break;
        }
        cursor = end;
        uint32_t width = 0, height = 0;
        if (r2_eyes_probe_vlc_dimensions(candidate, &width, &height, probe) == 0) {
            uint64_t area = (uint64_t)width * height;
            if (candidate == preferred ||
                ((!best || best != preferred) &&
                 (area > best_area || (area == best_area && candidate < best)))) {
                best = candidate;
                best_area = area;
            }
        } else if (errno != EINVAL) {
            result = -1;
            break;
        }
    }
    free(output);
    if (result == 0 && r2_eyes_probe_check(probe) != 0) result = -1;
    if (result == 0) *window = best;
    return result;
}

static int r2_eyes_find_pid_window(long pid, R2EyesProbe probe,
                                  unsigned long preferred, unsigned long *window)
{
    *window = 0;
    if (pid <= 1 || pid > INT_MAX) return 0;
    char process_id[32];
    snprintf(process_id, sizeof(process_id), "%ld", pid);
    char *argv[] = {"xdotool", "search", "--onlyvisible", "--pid", process_id, NULL};
    return r2_eyes_find_window(argv, probe, preferred, window);
}

static int r2_eyes_find_vlc_window(R2EyesProbe probe, unsigned long *window)
{
    char *argv[] = {"xdotool", "search", "--onlyvisible", "--class", "^vlc$", NULL};
    return r2_eyes_find_window(argv, probe, 0, window);
}

/* Prefer the current producer and window while they remain valid. All
 * lookups, including VLC fallback, share a single cancellable deadline. */
int r2_eyes_select_playback_window(const long *producers, size_t count,
                                  long *pid, unsigned long *window,
                                  int (*cancelled)(void *), void *userdata)
{
    if (!pid || !window || (count && !producers)) { errno = EINVAL; return -1; }
    const long preferred_pid = *pid;
    const unsigned long preferred_window = *window;
    *pid = -1;
    *window = 0;
    R2EyesProbe probe = {r2_eyes_probe_deadline(), cancelled, userdata};
    for (size_t i = 0; preferred_pid > 1 && i < count; ++i) {
        if (producers[i] != preferred_pid) continue;
        if (r2_eyes_find_pid_window(preferred_pid, probe, preferred_window, window) != 0)
            return -1;
        if (*window) { *pid = preferred_pid; return 0; }
        break;
    }
    int failed_search = 0;
    for (size_t i = 0; i < count; ++i) {
        if (producers[i] == preferred_pid) continue;
        if (r2_eyes_find_pid_window(producers[i], probe, 0, window) != 0) {
            if (errno == ECANCELED || errno == ETIMEDOUT || errno == ENOMEM ||
                errno == EOVERFLOW) return -1;
            if (!failed_search) failed_search = errno;
            continue;
        }
        if (*window) { *pid = producers[i]; return 0; }
    }
    if (failed_search) {
        errno = failed_search;
        return -1; /* Incomplete producer evidence does not authorize a fallback. */
    }
    return r2_eyes_find_vlc_window(probe, window);
}

unsigned long r2_eyes_window_for_pid(long pid)
{
    unsigned long window = 0;
    (void)r2_eyes_find_pid_window(pid, (R2EyesProbe){.deadline = r2_eyes_probe_deadline()}, 0, &window);
    return window;
}

unsigned long r2_eyes_vlc_window(void)
{
    unsigned long window = 0;
    (void)r2_eyes_find_vlc_window((R2EyesProbe){.deadline = r2_eyes_probe_deadline()}, &window);
    return window;
}

int r2_eyes_vlc_available(void)
{
    return r2_eyes_vlc_window() != 0;
}

static FILE *r2_eyes_start_vlc(unsigned long window_id, uint32_t width,
                              uint32_t height, pid_t *child_out)
{
    const char *display = getenv("DISPLAY");
    if (!window_id || !display || !*display) return NULL;
    char fps[32], window[32], size[32];
    snprintf(fps, sizeof(fps), "%d", R2_EYES_FPS);
    snprintf(window, sizeof(window), "%lu", window_id);
    snprintf(size, sizeof(size), "%ux%u", width, height);
    char *argv[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
        "-f", "x11grab", "-framerate", fps, "-window_id", window, "-video_size", size,
        "-draw_mouse", "0",
        "-i", (char *)display, "-vf", "format=rgb24", "-f", "rawvideo", "-pix_fmt",
        "rgb24", "-r", fps, "pipe:1", NULL};
    return r2_eyes_spawn_stream(argv, child_out, R2_EYES_STDERR_INHERIT);
}

/* ============================================================
   INITIALIZATION
   ============================================================ */

int r2_eyes_init(R2Eyes **eyes)
{
    if (!eyes)
        return -1;

    *eyes = NULL;

    R2Eyes *instance =
        calloc(1, sizeof(R2Eyes));

    if (!instance)
        return -1;

    instance->format.width = 0;
    instance->format.height = 0;
    instance->format.stride = 0;
    instance->format.bits_per_pixel = 0;
    instance->format.pixel_format =
        R2_PIXEL_UNKNOWN;

    if (r2_eyes_allocate_record_buffer(
            instance) != 0)
    {
        free(instance);
        return -1;
    }

    instance->origin =
        R2_VISION_NONE;
    instance->stream_child = -1;
    atomic_init(&instance->open, 0);

    instance->frame_count = 0;
    instance->timestamp = 0;

    r2_eyes_reset_frame(instance);
    r2_eyes_reset_event(instance);

    *eyes = instance;

    return 0;
}

/* ============================================================
   CLOSE / SHUTDOWN
   ============================================================ */

void r2_eyes_close(R2Eyes *eyes)
{
    if (!eyes)
        return;

    uint64_t session = eyes->preview_session;
    int clear_preview = session
        ? atomic_compare_exchange_strong(&active_preview_session, &session, 0)
        : !eyes->auxiliary_display && !r2_eyes_preview_session();
    if (clear_preview)
        r2_vwebcam_write_status(eyes, 0);
    eyes->preview_session = 0;

    if (eyes->stream)
    {
        r2_eyes_close_stream(eyes->stream, &eyes->stream_child);
        eyes->stream = NULL;
    }

    if (eyes->open) {
        char *details = NULL;
        if (asprintf(&details, "source=%s; frames_captured=%llu; resolution=%ux%u",
                 eyes->source_name ? eyes->source_name : "(unknown)",
                 (unsigned long long)eyes->frame_count,
                 eyes->format.width, eyes->format.height) < 0)
            details = NULL;
        r2_log_sensory("vision_source_closed",
                       "R2 Eyes stopped receiving visual frames.",
                       details, eyes->source_name);
        free(details);
    }

    eyes->open = 0;
    eyes->pending_frame_bytes = 0;
    eyes->frame_ready = 0;
    eyes->source_is_live = 0;
    eyes->origin = R2_VISION_NONE;
    eyes->frame_count = 0;
    eyes->timestamp = 0;
    eyes->last_log_timestamp = 0;

    free(eyes->source_name);
    eyes->source_name = NULL;

    memset(
        &eyes->event,
        0,
        sizeof(eyes->event)
    );

    memset(
        &eyes->frame,
        0,
        sizeof(eyes->frame)
    );

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;
}

void r2_eyes_set_auxiliary(R2Eyes *eyes, int auxiliary)
{
    if (eyes) eyes->auxiliary_display = auxiliary != 0;
}

void r2_eyes_shutdown(R2Eyes *eyes)
{
    if (!eyes)
        return;

    r2_eyes_close(eyes);
    if (!eyes->auxiliary_display) r2_vwebcam_stop_viewer();

    free(eyes->frame_buffer);
    eyes->frame_buffer = NULL;

    free(eyes->record_buffer);
    eyes->record_buffer = NULL;

    free(eyes);
}

/* ============================================================
   GENERIC SOURCE OPENING
   ============================================================ */

static int r2_eyes_open_internal(
    R2Eyes *eyes,
    const char *source,
    R2VisionOrigin origin,
    int live_device)
{
    if (!eyes || !source)
        return -1;

    r2_eyes_close(eyes);

    uint32_t width = 0;
    uint32_t height = 0;

    if (r2_eyes_probe_dimensions(
            source,
            live_device,
            &width,
            &height) != 0)
    {
        r2_log_sensory("vision_source_open_failed",
                       "R2 Eyes could not determine the input dimensions.",
                       "The source could not be probed or had unsupported dimensions.",
                       source);
        return -1;
    }

    if (r2_eyes_allocate_native_buffer(
            eyes,
            width,
            height) != 0)
    {
        r2_log_sensory("vision_buffer_allocation_failed",
                       "R2 Eyes could not allocate a native frame buffer.",
                       "The source could not be opened because frame memory allocation failed.",
                       source);
        return -1;
    }

    if (r2_eyes_allocate_record_buffer(
            eyes) != 0)
    {
        return -1;
    }

    char *source_copy = strdup(source);
    if (!source_copy) return -1;

    FILE *stream =
        r2_eyes_start_ffmpeg(
            source,
            live_device, &eyes->stream_child
        );

    if (!stream) {
        r2_log_sensory("vision_stream_open_failed",
                       "R2 Eyes could not start the FFmpeg capture stream.",
                       "FFmpeg did not provide a readable visual stream.",
                       source);
        free(source_copy);
        return -1;
    }

    eyes->source_name = source_copy;
    eyes->stream = stream;
    eyes->open = 1;
    eyes->source_is_live = live_device;
    eyes->origin = origin;
    eyes->frame_count = 0;
    eyes->timestamp = 0;

    r2_eyes_reset_event(eyes);
    r2_eyes_reset_frame(eyes);
    if (!eyes->auxiliary_display) r2_vwebcam_begin_session(eyes);
    if (!eyes->auxiliary_display) r2_vwebcam_start_viewer();

    char details[768];
    snprintf(details, sizeof(details),
             "origin=%s; resolution=%ux%u; pixel_format=RGB24; sampling_fps=%d",
             origin == R2_VISION_WORLD ? "world_camera" : "file",
             width, height, R2_EYES_FPS);
    r2_log_sensory("vision_source_opened",
                   "R2 Eyes opened a visual input source.",
                   details, eyes->source_name);

    return 0;
}

int r2_eyes_open_file(
    R2Eyes *eyes,
    const char *path)
{
    if (!eyes ||
        !path ||
        path[0] == '\0')
    {
        return -1;
    }

    return r2_eyes_open_internal(
        eyes,
        path,
        R2_VISION_FILE,
        0
    );
}

/* Find actual video-capture devices, rather than assuming /dev/video0.
 * A single camera can expose multiple V4L2 nodes (e.g. capture + metadata).
 * Query capabilities to avoid treating a metadata-only node as an eye. */
static char *r2_eyes_camera_path(void)
{
    const char *configured = getenv("R2_CAMERA_DEVICE");
    glob_t candidates = {0};
    if (!configured || !*configured) {
        if (glob("/dev/video*", 0, NULL, &candidates) != 0)
            return NULL;
    }
    size_t count = (configured && *configured) ? 1 : candidates.gl_pathc;
    for (size_t i = 0; i < count; ++i) {
        const char *path = (configured && *configured)
            ? configured : candidates.gl_pathv[i];
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode))
            continue;
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        struct v4l2_capability caps = {0};
        int rc = ioctl(fd, VIDIOC_QUERYCAP, &caps);
        close(fd);
        if (rc != 0) continue;
        uint32_t effective = (caps.capabilities & V4L2_CAP_DEVICE_CAPS)
            ? caps.device_caps : caps.capabilities;
        if (!(effective & (V4L2_CAP_VIDEO_CAPTURE |
                           V4L2_CAP_VIDEO_CAPTURE_MPLANE)))
            continue;
        char *selected = strdup(path);
        globfree(&candidates);
        return selected;
    }
    globfree(&candidates);
    return NULL;
}

int r2_eyes_camera_available(void)
{
    char *camera = r2_eyes_camera_path();
    int available = camera != NULL;
    free(camera);
    return available;
}

int r2_eyes_open_camera(R2Eyes *eyes)
{
    char *camera = r2_eyes_camera_path();
    if (!camera) return -1;
    int rc = r2_eyes_open_device(eyes, camera);
    free(camera);
    return rc;
}

int r2_eyes_open_device(
    R2Eyes *eyes,
    const char *device)
{
    if (!eyes ||
        !device ||
        device[0] == '\0')
    {
        return -1;
    }

    return r2_eyes_open_internal(
        eyes,
        device,
        R2_VISION_WORLD,
        1
    );
}

int r2_eyes_open_source(
    R2Eyes *eyes,
    const char *source_name)
{
    if (!eyes ||
        !source_name ||
        source_name[0] == '\0')
    {
        return -1;
    }

    if (strncmp(
            source_name,
            "/dev/video",
            10
        ) == 0)
    {
        return r2_eyes_open_device(
            eyes,
            source_name
        );
    }

    return r2_eyes_open_file(
        eyes,
        source_name
    );
}

/* ============================================================
   VLC SOURCE
   ============================================================ */

/* A video producer can publish its real X11 rendering window, regardless
 * of which UI framework hosts the player. Capture the window's pixels, never
 * a speculative description derived from its filename or player identity. */
int r2_eyes_window_available_cancelable(unsigned long window_id,
                                        int (*cancelled)(void *), void *userdata)
{
    uint32_t width = 0, height = 0;
    return window_id != 0 &&
        r2_eyes_probe_vlc_dimensions(window_id, &width, &height,
                                    (R2EyesProbe){r2_eyes_probe_deadline(), cancelled, userdata}) == 0;
}

int r2_eyes_window_available(unsigned long window_id)
{
    return r2_eyes_window_available_cancelable(window_id, NULL, NULL);
}

static char *r2_eyes_window_source(unsigned long window_id, int vlc, R2EyesProbe probe)
{
    char window[32];
    snprintf(window, sizeof(window), "%lu", window_id);
    if (!vlc) {
        char *source = NULL;
        return asprintf(&source, "video rendering window %s", window) < 0 ? NULL : source;
    }

    /* Keep the exact title as metadata; a title does not establish content. */
    char *argv[] = {"xdotool", "getwindowname", window, NULL};
    char *title = r2_eyes_query(argv, probe, 0);
    if (!title && (errno == ECANCELED || errno == ETIMEDOUT || errno == ENOMEM ||
                   errno == EOVERFLOW || errno == EPROTO))
        return NULL;
    if (title) {
        size_t length = strlen(title);
        if (length && title[length - 1] == '\n') title[length - 1] = '\0';
        if (*title) return title;
        free(title);
    }
    return strdup("VLC");
}

static int r2_eyes_open_x11(R2Eyes *eyes, unsigned long window_id, int vlc, R2EyesProbe probe)
{
    if (!eyes || !window_id || r2_eyes_probe_check(probe) != 0) return -1;
    r2_eyes_close(eyes);
    uint32_t width = 0, height = 0;
    if (r2_eyes_probe_vlc_dimensions(window_id, &width, &height, probe) != 0 ||
        r2_eyes_allocate_native_buffer(eyes, width, height) != 0 ||
        r2_eyes_allocate_record_buffer(eyes) != 0)
        return -1;
    char *source = r2_eyes_window_source(window_id, vlc, probe);
    if (!source) return -1;
    if (r2_eyes_probe_check(probe) != 0) { free(source); return -1; }

    FILE *stream = r2_eyes_start_vlc(window_id, width, height, &eyes->stream_child);
    if (!stream) { free(source); return -1; }
    if (r2_eyes_probe_check(probe) != 0) {
        int saved = errno;
        r2_eyes_close_stream(stream, &eyes->stream_child);
        free(source);
        errno = saved;
        return -1;
    }
    eyes->source_name = source;
    eyes->stream = stream;
    eyes->open = 1;
    eyes->source_is_live = 1;
    eyes->origin = vlc ? R2_VISION_VLC : R2_VISION_WINDOW;
    eyes->frame_count = 0;
    eyes->timestamp = 0;
    r2_eyes_reset_event(eyes);
    r2_eyes_reset_frame(eyes);
    if (!eyes->auxiliary_display) r2_vwebcam_begin_session(eyes);
    if (!eyes->auxiliary_display) r2_vwebcam_start_viewer();
    r2_log_sensory("vision_source_opened",
                   vlc ? "R2 Eyes began capturing the visible VLC window."
                       : "R2 Eyes connected to an active rendered video window.",
                   vlc ? "origin=VLC; frames are raw pixels until a vision model interprets them."
                       : "The source supplied a live window ID; frames are raw pixels.",
                   eyes->source_name);
    return 0;
}

int r2_eyes_open_x11_cancelable(R2Eyes *eyes, unsigned long window_id, int vlc,
                               int (*cancelled)(void *), void *userdata)
{
    return r2_eyes_open_x11(eyes, window_id, vlc,
                           (R2EyesProbe){r2_eyes_probe_deadline(), cancelled, userdata});
}

int r2_eyes_open_window(R2Eyes *eyes, unsigned long window_id)
{
    return r2_eyes_open_x11_cancelable(eyes, window_id, 0, NULL, NULL);
}

int r2_eyes_open_vlc_window(R2Eyes *eyes, unsigned long window_id)
{
    return r2_eyes_open_x11_cancelable(eyes, window_id, 1, NULL, NULL);
}

int r2_eyes_open_vlc(R2Eyes *eyes)
{
    if (!eyes) return -1;
    R2EyesProbe probe = {.deadline = r2_eyes_probe_deadline()};
    unsigned long window_id;
    if (r2_eyes_find_vlc_window(probe, &window_id) != 0 || !window_id) {
        r2_eyes_close(eyes);
        r2_log_sensory("vlc_window_not_found",
                       "R2 Eyes could not find a visible VLC window.",
                       "VLC window capture requires a visible X11 VLC window and xdotool.",
                       "VLC");
        return -1;
    }
    return r2_eyes_open_x11(eyes, window_id, 1, probe);
}

/* ============================================================
   FRAME CAPTURE
   ============================================================ */

int r2_eyes_capture(R2Eyes *eyes)
{
    return r2_eyes_capture_cancelable(eyes, NULL, NULL);
}

int r2_eyes_capture_cancelable(R2Eyes *eyes,
                               int (*cancelled)(void *), void *userdata)
{
    if (!eyes ||
        !eyes->open ||
        !eyes->stream ||
        !eyes->frame_buffer ||
        eyes->frame_size == 0)
    {
        return -1;
    }

    eyes->frame_ready = 0;
    int result =
        r2_eyes_read_exact(
            eyes, eyes->frame_buffer, eyes->frame_size, cancelled, userdata
        );

    if (result == 0)
    {
        /* FFmpeg EOF means this stream is dead, including a live camera whose
           device was unplugged or whose capture process exited. Close it in
           every case so later calls cannot loop forever on a dead pipe or
           expose the previous frame as though it were current. */
        r2_eyes_close(eyes);
        return 0;
    }

    if (result < 0)
    {
        int saved = errno;
        /* Stopping a worker is distinct from closing its caller-owned source.
         * Preserve any partial frame for the next reader of this same stream. */
        if (saved == ECANCELED) return -1;
        r2_log_sensory("visual_frame_read_failed",
                           "R2 Eyes encountered an error reading a visual frame.",
                           saved == ETIMEDOUT
                               ? "A live decoder did not deliver a complete frame within its read budget."
                               : "The capture stream returned incomplete pixels or a decoder error.",
                           eyes->source_name);
        r2_eyes_close(eyes);
        errno = saved;
        return -1;
    }

    eyes->frame_count++;
    eyes->timestamp =
        r2_eyes_timestamp();

    eyes->event.origin =
        eyes->origin;

    eyes->event.type =
        R2_VISUAL_UNKNOWN;

    eyes->event.format =
        eyes->format;

    eyes->event.timestamp =
        eyes->timestamp;

    eyes->event.frame_count =
        eyes->frame_count;

    eyes->frame.data =
        eyes->frame_buffer;

    eyes->frame.size =
        eyes->frame_size;

    eyes->frame.format =
        eyes->format;

    eyes->frame.timestamp =
        eyes->timestamp;

    eyes->frame.frame_number =
        eyes->frame_count;

    eyes->frame.preview_session = eyes->preview_session;
    eyes->frame_ready = 1;
    if (!eyes->auxiliary_display && eyes->preview_session &&
        eyes->preview_session == r2_eyes_preview_session()) {
        r2_vwebcam_publish_frame(eyes);
        r2_vwebcam_write_status(eyes, 1);
    }

    /*
     * Record a bounded-rate sensory journal heartbeat instead of
     * writing a database row for every 10-fps frame. The full frame
     * remains available to the consumer; this entry documents that
     * real pixels arrived and identifies their source and format.
     */
    if (eyes->last_log_timestamp == 0 ||
        eyes->timestamp - eyes->last_log_timestamp >= 5000) {
        char *details = NULL;
        if (asprintf(&details, "source=%s; frame=%llu; resolution=%ux%u; bytes=%zu; "
                 "timestamp_ms=%llu; interpretation=not_performed_by_Eyes",
                 eyes->source_name,
                 (unsigned long long)eyes->frame_count,
                 eyes->format.width, eyes->format.height,
                 eyes->frame_size,
                 (unsigned long long)eyes->timestamp) < 0)
            details = NULL;
        r2_log_sensory("visual_frame_received",
                       "R2 Eyes received a real visual frame.",
                       details, eyes->source_name);
        free(details);
        eyes->last_log_timestamp = eyes->timestamp;
    }

    return 1;
}

/* ============================================================
   FRAME ACCESS
   ============================================================ */

int r2_eyes_get_frame(
    R2Eyes *eyes,
    R2VisionFrame *frame)
{
    if (!eyes ||
        !frame ||
        !eyes->open || !eyes->frame_ready)
    {
        errno = EAGAIN;
        return -1;
    }

    *frame = eyes->frame;

    return 0;
}

ssize_t r2_eyes_copy_frame(
    R2Eyes *eyes,
    void *buffer,
    size_t buffer_size)
{
    if (!eyes ||
        !buffer ||
        !eyes->open || !eyes->frame_ready)
    {
        errno = EAGAIN;
        return -1;
    }

    if (buffer_size < eyes->frame_size)
    {
        errno = ENOBUFS;
        return -1;
    }

    memcpy(
        buffer,
        eyes->frame_buffer,
        eyes->frame_size
    );

    return (ssize_t)eyes->frame_size;
}

/* ============================================================
   EVENT INFORMATION
   ============================================================ */

int r2_eyes_get_event(
    R2Eyes *eyes,
    R2VisionEvent *event)
{
    if (!eyes || !event)
        return -1;

    const char *source = eyes->source_name ? eyes->source_name : "";
    size_t length = strlen(source);
    if (length >= sizeof(event->source_name)) {
        errno = EOVERFLOW;
        return -1;
    }
    *event = eyes->event;
    memcpy(event->source_name, source, length + 1);
    return 0;
}

char *r2_eyes_copy_source(const R2Eyes *eyes)
{
    return eyes && eyes->source_name ? strdup(eyes->source_name) : NULL;
}

int r2_eyes_get_format(
    R2Eyes *eyes,
    R2VideoFormat *format)
{
    if (!eyes || !format)
        return -1;

    *format = eyes->format;

    return 0;
}

/* ============================================================
   STATE
   ============================================================ */

int r2_eyes_is_open(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->open;
}

R2VisionOrigin r2_eyes_get_origin(
    R2Eyes *eyes)
{
    if (!eyes)
        return R2_VISION_NONE;

    return eyes->origin;
}

uint64_t r2_eyes_get_frame_count(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->frame_count;
}

uint64_t r2_eyes_get_timestamp(
    R2Eyes *eyes)
{
    if (!eyes)
        return 0;

    return eyes->timestamp;
}
