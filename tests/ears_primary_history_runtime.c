/* Selected live PCM cannot silently cross primary stream generations. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

static char source_names[2][1024];
static int source_reads[2], level_notices[2], ended_notices[2];
int __wrap_r2_ears_get_event(R2Ears *receiver, R2HearingEvent *event)
{
    memset(event, 0, sizeof(*event));
    event->source_name = strdup(source_names[receiver == ears_desktop]);
    return event->source_name ? 0 : -1;
}
ssize_t __wrap_r2_ears_read(R2Ears *receiver, void *pcm, size_t bytes)
{
    int slot = receiver == ears_desktop;
    int count = source_reads[slot]++;
    if (count >= 2) return -1;
    memset(pcm, count ? 0x7f : 0, bytes);
    return (ssize_t)bytes;
}
int64_t __wrap_r2_log_sensory(const char *event, const char *summary,
                             const char *details, const char *source)
{
    (void)summary; (void)details;
    int slot = !strcmp(source, source_names[1]);
    assert(!strcmp(source, source_names[slot]));
    if (!strcmp(event, "audio_level_change_notice")) ++level_notices[slot];
    if (!strcmp(event, "audio_capture_ended")) ++ended_notices[slot];
    return 1;
}

typedef struct { int desktop; uint64_t generation; int result, error; FILE *wav; } Live;
static atomic_int waiting = ATOMIC_VAR_INIT(0);
static int announce_wait(void *unused)
{
    (void)unused;
    atomic_store(&waiting, 1);
    return 0;
}
static void *copy_live(void *userdata)
{
    Live *request = userdata;
    request->result = request->desktop
        ? r2_ears_write_desktop_recent_audio(1920, 0, request->generation,
                                              request->wav, announce_wait, NULL)
        : r2_ears_write_recent_audio(1920, 0, request->generation,
                                      request->wav, announce_wait, NULL);
    request->error = errno;
    return NULL;
}

int main(void)
{
    for (int desktop = 0; desktop <= 1; ++desktop) {
        pthread_mutex_t *lock = desktop ? &ears_desktop_recent_lock : &ears_recent_lock;
        uint64_t *generation = desktop ? &ears_desktop_recent_generation : &ears_recent_generation;
        uint64_t *total = desktop ? &ears_desktop_recent_total_bytes : &ears_recent_total_bytes;
        unsigned char *pcm = desktop ? ears_desktop_recent_audio : ears_recent_audio;
        atomic_int *running = desktop ? &ears_desktop_capture_running : &ears_capture_running;
        atomic_store(running, 1);
        Live request = {.desktop = desktop, .generation = *generation, .wav = tmpfile()};
        assert(request.wav);
        pthread_t thread;
        atomic_store(&waiting, 0);
        assert(pthread_create(&thread, NULL, copy_live, &request) == 0);
        struct timespec pause = {0, 1000000L};
        for (int i = 0; i < 1000 && !atomic_load(&waiting); ++i) nanosleep(&pause, NULL);
        assert(atomic_load(&waiting));
        /* Replacement has a complete interval ready before the old
         * listener wakes: it must reject it even though enough PCM exists. */
        pthread_mutex_lock(lock);
        ++*generation;
        memset(pcm, 0x55, 1920);
        *total = 1920;
        pthread_mutex_unlock(lock);
        assert(pthread_join(thread, NULL) == 0);
        assert(request.result == -1 && request.error == ECANCELED);
        assert(ftell(request.wav) == 0);
        fclose(request.wav);

        /* A new request to this same generation retains exact audio. */
        request.wav = tmpfile();
        request.generation = *generation;
        assert(request.wav);
        copy_live(&request);
        assert(request.result == 0);
        rewind(request.wav);
        unsigned char actual[1920];
        assert(fread(actual, 1, sizeof(actual), request.wav) == sizeof(actual));
        for (size_t i = 0; i < sizeof(actual); ++i) assert(actual[i] == 0x55);
        fclose(request.wav);
        atomic_store(running, 0);
    }
    /* Both primary workers preserve their actual full source identity
     * through level-change and failure journal events. */
    ears = (R2Ears *)(uintptr_t)1;
    ears_desktop = (R2Ears *)(uintptr_t)2;
    for (int slot = 0; slot < 2; ++slot) {
        memset(source_names[slot], slot ? 'd' : 'm', 900);
        source_names[slot][900] = '\0';
        atomic_store(slot ? &ears_desktop_capture_running : &ears_capture_running, 1);
        if (slot) r2_ears_desktop_capture_worker(NULL);
        else r2_ears_capture_worker(NULL);
        assert(level_notices[slot] == 1 && ended_notices[slot] == 1);
    }
    ears = ears_desktop = NULL;
    puts("Primary mic/desktop live intervals reject replacement PCM and preserve valid-generation samples and exact worker identities.");
    return 0;
}
