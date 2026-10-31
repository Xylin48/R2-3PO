/* Exercise production PCM consumption without PulseAudio hardware or ASR. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

typedef struct {
    int desktop;
    uint32_t length;
    uint64_t start;
    uint64_t generation;
    unsigned char *expected;
    atomic_uint consumed;
} IntervalFixture;

static ssize_t consume_pcm(void *opaque, const char *data, size_t length)
{
    IntervalFixture *f = opaque;
    unsigned offset = atomic_load(&f->consumed);
    assert(length <= f->length - offset);
    assert(memcmp(data, f->expected + offset, length) == 0);
    atomic_fetch_add(&f->consumed, (unsigned)length);
    return (ssize_t)length;
}

static void *produce_pcm(void *opaque)
{
    IntervalFixture *f = opaque;
    pthread_mutex_t *lock = f->desktop ? &ears_desktop_recent_lock : &ears_recent_lock;
    pthread_cond_t *cond = f->desktop ? &ears_desktop_recent_cond : &ears_recent_cond;
    unsigned char *ring = f->desktop ? ears_desktop_recent_audio : ears_recent_audio;
    uint64_t *total = f->desktop ? &ears_desktop_recent_total_bytes : &ears_recent_total_bytes;
    size_t *position = f->desktop ? &ears_desktop_recent_write_position : &ears_recent_write_position;
    size_t *retained = f->desktop ? &ears_desktop_recent_length : &ears_recent_length;
    uint32_t published = 0;
    while (published < f->length) {
        size_t count = f->length - published;
        if (count > 65536U) count = 65536U;
        pthread_mutex_lock(lock);
        size_t start = (size_t)(*total % R2_EARS_RECENT_CAPACITY);
        size_t first = R2_EARS_RECENT_CAPACITY - start;
        if (first > count) first = count;
        memcpy(ring + start, f->expected + published, first);
        if (count > first) memcpy(ring, f->expected + published + first, count - first);
        *total += count;
        *position = (size_t)(*total % R2_EARS_RECENT_CAPACITY);
        *retained += count;
        if (*retained > R2_EARS_RECENT_CAPACITY) *retained = R2_EARS_RECENT_CAPACITY;
        pthread_cond_broadcast(cond);
        pthread_mutex_unlock(lock);
        published += (uint32_t)count;
        /* Handshake with the real fwrite sink makes this independent of CI
           CPU speed while still crossing and wrapping the rolling history. */
        while (atomic_load(&f->consumed) < published) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
    }
    return NULL;
}

static void exercise_source(int desktop)
{
    IntervalFixture f = {.desktop = desktop, .length = 30U * R2_EARS_PCM_BYTES_PER_SECOND,
                          .start = R2_EARS_RECENT_CAPACITY - 10000U, .generation = 23};
    atomic_init(&f.consumed, 0);
    f.expected = malloc(f.length);
    assert(f.expected);
    for (size_t i = 0; i < f.length; ++i) f.expected[i] = (unsigned char)(i * 17U + desktop);
    uint64_t other_total = desktop ? ears_recent_total_bytes : ears_desktop_recent_total_bytes;
    if (desktop) {
        ears_desktop_recent_total_bytes = f.start;
        ears_desktop_recent_write_position = (size_t)f.start;
        ears_desktop_recent_length = 0;
        ears_desktop_recent_generation = f.generation;
        atomic_store(&ears_desktop_capture_running, 1);
    } else {
        ears_recent_total_bytes = f.start;
        ears_recent_write_position = (size_t)f.start;
        ears_recent_length = 0;
        ears_recent_generation = f.generation;
        atomic_store(&ears_capture_running, 1);
    }
    cookie_io_functions_t io = {.write = consume_pcm};
    FILE *sink = fopencookie(&f, "w", io);
    assert(sink && setvbuf(sink, NULL, _IONBF, 0) == 0);
    pthread_t producer;
    assert(pthread_create(&producer, NULL, produce_pcm, &f) == 0);
    int rc = desktop
        ? r2_ears_write_desktop_recent_audio(f.length, f.start, f.generation, sink, NULL, NULL)
        : r2_ears_write_recent_audio(f.length, f.start, f.generation, sink, NULL, NULL);
    assert(rc == 0);
    assert(pthread_join(producer, NULL) == 0);
    assert(atomic_load(&f.consumed) == f.length);
    assert((desktop ? ears_recent_total_bytes : ears_desktop_recent_total_bytes) == other_total);
    assert(fclose(sink) == 0);
    free(f.expected);
    if (desktop) atomic_store(&ears_desktop_capture_running, 0);
    else atomic_store(&ears_capture_running, 0);
}

static int cancel_now(void *unused) { (void)unused; return 1; }

int main(void)
{
    assert(R2_EARS_MAX_WAV_SECONDS > 30);
    exercise_source(0);
    exercise_source(1);
    FILE *sink = tmpfile(); assert(sink);
    /* A replacement source may have reused every old byte-counter value. */
    ears_recent_generation = 42;
    ears_recent_total_bytes = 128;
    atomic_store(&ears_capture_running, 1);
    assert(r2_ears_write_recent_audio(128, 0, 41, sink, NULL, NULL) < 0);
    assert(ftell(sink) == 0);
    /* Lost old samples cannot be reconstructed as the chosen interval. */
    ears_recent_total_bytes = R2_EARS_RECENT_CAPACITY + 4U;
    assert(r2_ears_write_recent_audio(128, 0, 42, sink, NULL, NULL) < 0);
    ears_recent_total_bytes = 0;
    assert(r2_ears_write_recent_audio(128, 0, 42, sink, cancel_now, NULL) < 0);
    atomic_store(&ears_capture_running, 0);
    assert(r2_ears_write_recent_audio(128, 0, 42, sink, NULL, NULL) < 0);
    fclose(sink);
    puts("Complete 30-second live PCM intervals, source isolation, wrap, restart identity and cancellation passed.");
    return 0;
}
