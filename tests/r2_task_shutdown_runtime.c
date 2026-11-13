/*
 * Runtime regression: an already-running HAND task must not hold R2 shutdown
 * open forever when its command is silent and never exits on its own.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <time.h>

static void *request_shutdown_after_delay(void *unused)
{
    (void)unused;
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 200000000L};
    (void)nanosleep(&pause, NULL);
    atomic_store(&shutting_down, 1);
    return NULL;
}

static double elapsed_seconds(struct timespec start, struct timespec end)
{
    return (double)(end.tv_sec - start.tv_sec) +
           (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

static void check_shutdown(const char *command)
{
    pthread_t stopper;
    struct timespec start, end;

    atomic_store(&shutting_down, 0);
    assert(pthread_create(&stopper, NULL, request_shutdown_after_delay, NULL) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);

    execute_task("shutdown", command);

    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    assert(elapsed_seconds(start, end) < 5.0);

    pthread_mutex_lock(&tasks_lock);
    CompletedTask *completed = completed_head;
    assert(completed != NULL);
    assert(completed->output != NULL && strstr(completed->output, "started") != NULL);
    assert(completed->return_code == -1);
    completed_head = NULL;
    completed_tail = NULL;
    pthread_mutex_unlock(&tasks_lock);
    free(completed->command);
    free(completed->output);
    free(completed);
}

int main(void)
{
    check_shutdown("printf started; sleep 30 & wait");
    check_shutdown("printf started; exec >/dev/null 2>&1; sleep 30");

    puts("R2 active task shutdown cancellation smoke passed.");
    return 0;
}
