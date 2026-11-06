/* Stall a real libpulse context handshake on a local, silent Unix socket. */
#define _POSIX_C_SOURCE 200809L
#include "../AudioCapture.h"
#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    R2AudioCapture *capture;
    int rc, error;
} Connect;

static void *connect_source(void *userdata)
{
    Connect *request = userdata;
    pa_sample_spec spec = {PA_SAMPLE_S16LE, 48000, 2};
    request->rc = r2_audio_capture_connect(request->capture, "test.monitor", &spec, 1920);
    request->error = errno;
    return NULL;
}

static double seconds(void)
{
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return now.tv_sec + (double)now.tv_nsec / 1e9;
}

int main(void)
{
    char directory[] = "/tmp/r2-pulse-handshake-XXXXXX";
    assert(mkdtemp(directory));
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    assert(snprintf(address.sun_path, sizeof(address.sun_path), "%s/socket", directory) > 0);
    char server[sizeof(address.sun_path) + 6];
    assert(snprintf(server, sizeof(server), "unix:%s", address.sun_path) > 0);
    assert(setenv("PULSE_SERVER", server, 1) == 0);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(listener, 1) == 0);

    Connect request = {.capture = r2_audio_capture_new()};
    assert(request.capture);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, connect_source, &request) == 0);
    struct pollfd pending = {.fd = listener, .events = POLLIN};
    assert(poll(&pending, 1, 2000) == 1);
    int client = accept(listener, NULL, NULL);
    assert(client >= 0);
    /* Accept but never answer Pulse's authentication/protocol handshake. */
    pending.fd = client;
    pending.revents = 0;
    assert(poll(&pending, 1, 2000) == 1);
    double start = seconds();
    r2_audio_capture_cancel(request.capture);
    assert(pthread_join(thread, NULL) == 0);
    assert(seconds() - start < 1.0);
    assert(request.rc == -1 && request.error == ECANCELED);
    r2_audio_capture_free(request.capture);
    close(client);
    close(listener);
    assert(unlink(address.sun_path) == 0);
    assert(rmdir(directory) == 0);

    /* A cancellation before starting the event loop is also definitive. */
    request.capture = r2_audio_capture_new();
    assert(request.capture);
    r2_audio_capture_cancel(request.capture);
    connect_source(&request);
    assert(request.rc == -1 && request.error == ECANCELED);
    r2_audio_capture_free(request.capture);
    puts("Real libpulse stalled handshake and pre-start cancellation return ECANCELED promptly.");
    return 0;
}
