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
    assert(unlink(completed->archive_path) == 0);
    free(completed->command);
    free(completed->output);
    free(completed);
}

static void check_complete_output_archive(void)
{
    atomic_store(&shutting_down, 0);
    int diagnostic = dup(STDERR_FILENO);
    int discard = open("/dev/null", O_WRONLY);
    assert(diagnostic >= 0 && discard >= 0 && dup2(discard, STDERR_FILENO) >= 0);
    close(discard);
    execute_task("archive", "printf BEGIN; head -c 30000 /dev/zero; printf END");
    assert(dup2(diagnostic, STDERR_FILENO) >= 0);
    close(diagnostic);
    CompletedTask *completed = completed_head;
    assert(completed && completed->return_code == 0);
    assert(strstr(completed->output, "binary output") != NULL);
    FILE *archive = fopen(completed->archive_path, "rb");
    assert(archive);
    char bytes[30009] = {0};
    assert(fread(bytes, 1, sizeof(bytes), archive) == 30008);
    assert(memcmp(bytes, "BEGIN", 5) == 0 && memcmp(bytes + 30005, "END", 3) == 0);
    for (size_t i = 5; i < 30005; ++i) assert(bytes[i] == 0);
    assert(fclose(archive) == 0 && unlink(completed->archive_path) == 0);
    char *result = collect_completed();
    assert(result && strstr(result, "Output archive:") != NULL);
    free(result);
}

static void check_console_shutdown(const char *script)
{
    char console[] = R2_HOME "/fake-console-XXXXXX";
    int fd = mkstemp(console);
    assert(fd >= 0);
    assert(dprintf(fd, "#!/bin/sh\n%s\n", script) > 0);
    assert(fchmod(fd, 0700) == 0 && close(fd) == 0);

    atomic_store(&shutting_down, 0);
    pthread_t stopper;
    struct timespec start, end;
    assert(pthread_create(&stopper, NULL, request_shutdown_after_delay, NULL) == 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    char *output = NULL;
    assert(run_gameboy_console(console, "events", NULL, NULL, &output) < 0 && !output);
    assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    assert(elapsed_seconds(start, end) < 5.0);
    assert(unlink(console) == 0);
}

static void check_workspace_path_with_spaces(void)
{
    static const char name[] = "hand fixture with spaces";
    static const char target[] = R2_WORKSPACE "/hand fixture with spaces";
    static const char link_path[] = R2_WORKSPACE "/hand link with spaces";
    FILE *file = fopen(target, "w");
    assert(file && fputs("complete fixture content", file) >= 0 && fclose(file) == 0);
    char *result = process_tools("[READ] hand fixture with spaces");
    assert(result && strstr(result, "complete fixture content"));
    free(result);

    assert(symlink(name, link_path) == 0);
    result = process_tools("[DELETE] hand link with spaces");
    assert(result && strstr(result, "Deleted: hand link with spaces"));
    free(result);
    assert(access(link_path, F_OK) != 0 && access(target, F_OK) == 0);
    assert(unlink(target) == 0);

    char command[MAX_COMMAND + 32];
    memcpy(command, "[HAND] ", 7);
    memset(command + 7, 'x', MAX_COMMAND + 1);
    command[MAX_COMMAND + 8] = '\0';
    result = process_tools(command);
    assert(result && strstr(result, "no task was queued") && !task_head);
    free(result);

    result = process_tools("[WORLD] buy|item|1.00|description|pockets|extra");
    assert(result && strstr(result, "too many fields; no action was taken"));
    free(result);
}

int main(void)
{
    check_shutdown("printf started; sleep 30 & wait");
    check_shutdown("printf started; exec >/dev/null 2>&1; sleep 30");

    check_complete_output_archive();
    atomic_store(&shutting_down, 0);
    check_workspace_path_with_spaces();
    check_console_shutdown("exec sleep 30");
    check_console_shutdown("exec >/dev/null; exec sleep 30");
    puts("R2 active task shutdown and exact output archive smoke passed.");
    return 0;
}
