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

    execute_task("shutdown", command, 0);

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
    execute_task("archive", "printf BEGIN; head -c 30000 /dev/zero; printf END", 0);
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

static int count_rows(sqlite3 *fixture, const char *sql)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(fixture, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

static void check_durable_receipts(void)
{
    assert(r2_log_init() == 0);
    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER block_hands_accept BEFORE INSERT ON r2_log_pending_hands "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    assert(!start_background_task("printf rejected") && !task_head);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_events WHERE details='printf rejected';") == 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER block_hands_accept;", NULL, NULL, NULL) == SQLITE_OK);

    char *task_id = start_background_task("printf accepted-output");
    assert(task_id && task_head && task_head->receipt > 0);
    Task *task = task_head;
    task_head = task_tail = NULL;
    execute_task(task->id, task->command, task->receipt);
    CompletedTask *completion = completed_head;
    assert(completion && completion->return_code == 0 && !strcmp(completion->output, "accepted-output"));
    unlink(completion->archive_path);
    completed_head = completed_tail = NULL;
    free(completion->command);
    free(completion->output);
    free(completion);
    free(task_id);
    free(task->command);
    free(task);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 0);

    int64_t queued = r2_log_accept_hands("queued-fixture", "touch should-not-exist");
    int64_t interrupted = r2_log_accept_hands("active-fixture", "touch would-have-side-effects");
    assert(queued > 0 && interrupted > queued);
    assert(r2_log_arm_hands(interrupted, R2_HOME "/exact-output-archive") == 0);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER block_hands_outcome_ack BEFORE DELETE ON r2_log_pending_hands "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_recover_hands() == -1);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 2);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_events WHERE event_type='hands_task_outcome';") == 1);
    assert(sqlite3_exec(fixture, "DROP TRIGGER block_hands_outcome_ack;", NULL, NULL, NULL) == SQLITE_OK);
    r2_log_shutdown();
    assert(r2_log_init() == 0);
    assert(r2_log_recover_hands() == 0 && r2_log_recover_hands() == 0);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 0);
    assert(count_rows(fixture,
        "SELECT count(*) FROM r2_log_links l JOIN r2_log_events e ON e.id=l.from_event_id "
        "JOIN r2_log_events q ON q.id=l.to_event_id WHERE l.relationship='hands_outcome_for' AND ("
        "(q.details='printf accepted-output' AND e.details LIKE 'Exit code: 0%') OR "
        "(q.details='touch should-not-exist' AND e.summary LIKE '%before%launch%') OR "
        "(q.details='touch would-have-side-effects' AND e.summary LIKE '%outcome is unknown%' "
        "AND e.details LIKE '%exact-output-archive'));") == 3);
    assert(access(R2_HOME "/should-not-exist", F_OK) != 0);
    assert(access(R2_HOME "/would-have-side-effects", F_OK) != 0);
    assert(sqlite3_close(fixture) == SQLITE_OK);
    r2_log_shutdown();
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
    atomic_store(&shutting_down, 0);
    check_durable_receipts();
    puts("R2 active task shutdown, exact output archive and durable receipt smoke passed.");
    return 0;
}
