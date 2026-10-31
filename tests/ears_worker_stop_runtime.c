/* Exercise the actual core workers and public stop methods with paused devices. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include "ears_native_test_endpoints.h"
#include <sched.h>

int main(void)
{
    assert(r2_ears_init(&ears) == 0 && r2_ears_init(&ears_desktop) == 0);
    for (int cycle = 0; cycle < 3; ++cycle) {
        assert(r2_ears_open_source(ears, "test_mic") == 0);
        FakePulse *mic = last_stream;
        mic->paused = 1;
        assert(r2_ears_open_source(ears_desktop, "test.monitor") == 0);
        FakePulse *desktop = last_stream;
        desktop->paused = 1;
        atomic_store(&ears_capture_running, 1);
        atomic_store(&ears_desktop_capture_running, 1);
        assert(pthread_create(&ears_capture_thread, NULL, r2_ears_capture_worker, NULL) == 0);
        ears_capture_thread_started = 1;
        assert(pthread_create(&ears_desktop_capture_thread, NULL, r2_ears_desktop_capture_worker, NULL) == 0);
        ears_desktop_capture_thread_started = 1;
        while (!atomic_load(&mic->peeks) || !atomic_load(&desktop->peeks)) sched_yield();
        uint64_t mic_generation = ears_recent_generation;
        uint64_t desktop_generation = ears_desktop_recent_generation;
        assert(r2_ears_stop() == 0);
        assert(!ears_capture_thread_started && !r2_ears_is_open(ears));
        assert(ears_recent_generation == mic_generation + 1);
        assert(ears_desktop_recent_generation == desktop_generation);
        assert(atomic_load(&ears_desktop_capture_running));
        assert(r2_ears_is_open(ears_desktop));
        assert(r2_ears_stop_desktop() == 0);
        assert(!ears_desktop_capture_thread_started && !r2_ears_is_open(ears_desktop));
        assert(ears_desktop_recent_generation == desktop_generation + 1);
        assert(ears_recent_total_bytes == 0 && ears_desktop_recent_total_bytes == 0);
    }
    r2_ears_shutdown(ears); ears = NULL;
    r2_ears_shutdown(ears_desktop); ears_desktop = NULL;
    assert(contexts == 0 && open_count == close_count && open_count == 6);
    puts("Production stop joins paused native workers, isolates sources and permits restart.");
    return 0;
}
