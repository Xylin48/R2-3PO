/*
 * Runtime regression: an already-running HAND task must not hold R2 shutdown
 * open forever when its command is silent and never exits on its own.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* Linker wrapping happens after optimization, so keep these observations
 * visible across calls compiled as ordinary libc calls. */
static volatile int fail_result_format, fail_result_growth, growth_armed;
static volatile int fail_completion_format;
static volatile uintptr_t read_path_address, read_data_address;

void __real_free(void *value);
void __wrap_free(void *value)
{
    if ((uintptr_t)value == read_path_address) read_path_address = 0;
    if ((uintptr_t)value == read_data_address) read_data_address = 0;
    __real_free(value);
}

void *__real_realloc(void *value, size_t size);
void *__wrap_realloc(void *value, size_t size)
{
    if (growth_armed) {
        growth_armed = 0;
        fail_result_growth = 0;
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(value, size);
}

static int format_tool_fixture(char **answer, const char *format, va_list args)
{
    if (fail_completion_format &&
        !strncmp(format, "REAL HAND TASK COMPLETED.", sizeof("REAL HAND TASK COMPLETED.") - 1)) {
        fail_completion_format = 0;
        *answer = NULL;
        errno = ENOMEM;
        return -1;
    }
    int read_result = (fail_result_format || fail_result_growth) &&
        !strcmp(format, "REAL READ RESULT:\nPath: %s\nContent:\n%s\n");
    if (read_result) {
        va_list copy;
        va_copy(copy, args);
        read_path_address = (uintptr_t)va_arg(copy, const char *);
        read_data_address = (uintptr_t)va_arg(copy, const char *);
        va_end(copy);
        if (fail_result_format) {
            fail_result_format = 0;
            *answer = NULL;
            errno = ENOMEM;
            return -1;
        }
    }
    int formatted = vasprintf(answer, format, args);
    growth_armed = read_result && fail_result_growth && formatted > 1024;
    return formatted;
}

int __wrap_asprintf(char **answer, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int formatted = format_tool_fixture(answer, format, args);
    va_end(args);
    return formatted;
}

/* Fortified glibc calls this symbol at the same production call site. */
int __wrap___asprintf_chk(char **answer, int flag, const char *format, ...)
{
    (void)flag;
    va_list args;
    va_start(args, format);
    int formatted = format_tool_fixture(answer, format, args);
    va_end(args);
    return formatted;
}

static void check_action_output_allocation_failure(void)
{
    static const char path[] = R2_WORKSPACE "/large read fixture.txt";
    FILE *fp = fopen(path, "w");
    assert(fp);
    for (unsigned i = 0; i < 2200; ++i) assert(fputc('x', fp) == 'x');
    assert(fclose(fp) == 0);
    for (int grow = 0; grow <= 1; ++grow) {
        fail_result_format = !grow;
        fail_result_growth = grow;
        char *answer = process_tools("[READ] large read fixture.txt");
        assert(!fail_result_format && !fail_result_growth);
        assert(answer && strstr(answer, "I ran out of memory while recording an action result."));
        assert(!read_path_address && !read_data_address);
        free(answer);
    }
    assert(unlink(path) == 0);
}

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

static void check_hands_exact_transactions(void)
{
    assert(r2_log_init() == 0);
    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER rewrite_hands_accept AFTER INSERT ON r2_log_pending_hands "
        "BEGIN UPDATE r2_log_pending_hands SET archive_path='wrong' WHERE event_id=NEW.event_id; END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_accept_hands("rewrite-accept", "fixture only") < 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER rewrite_hands_accept;", NULL, NULL, NULL) == SQLITE_OK);
    int64_t receipt = r2_log_accept_hands("exact-transaction", "fixture only");
    assert(receipt > 0);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER rewrite_hands_arm AFTER UPDATE OF archive_path ON r2_log_pending_hands "
        "WHEN NEW.archive_path IS NOT NULL BEGIN "
        "UPDATE r2_log_pending_hands SET archive_path='wrong' WHERE event_id=NEW.event_id; END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_arm_hands(receipt, "/tmp/exact-hands-archive") < 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER rewrite_hands_arm;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_arm_hands(receipt, "/tmp/exact-hands-archive") == 0);
    int before = count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;");
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER rewrite_hands_result AFTER INSERT ON r2_log_hands_results "
        "BEGIN UPDATE r2_log_hands_results SET result='wrong' WHERE outcome_id=NEW.outcome_id; END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_finish_hands(receipt, 1, 0, "complete exact fixture outcome") < 0);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 1);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;") == before);
    assert(sqlite3_exec(fixture, "DROP TRIGGER rewrite_hands_result;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_finish_hands(receipt, 1, 0, "complete exact fixture outcome") == 0);
    /* Other fixtures may have pending outbox rows. Resolve this exact receipt. */
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(fixture,
        "SELECT outcome_id FROM r2_log_hands_results WHERE result='complete exact fixture outcome';",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int64_t outcome = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    assert(r2_log_deliver_hands_result(outcome, receipt) < 0);
    int64_t turn = r2_log_event(R2_LOG_CONVERSATION, "conversation_turn",
                                "Fixture accepted conversation", NULL, "fixture");
    assert(turn > 0);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER undo_hands_delivery AFTER UPDATE OF delivered_to ON r2_log_hands_results "
        "WHEN NEW.delivered_to IS NOT NULL BEGIN "
        "UPDATE r2_log_hands_results SET delivered_to=NULL WHERE outcome_id=NEW.outcome_id; END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_deliver_hands_result(outcome, turn) < 0);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_links WHERE relationship='hands_result_in_context';") == 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER undo_hands_delivery;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_deliver_hands_result(outcome, turn) == 0);
    assert(r2_log_deliver_hands_result(outcome, turn) < 0);
    assert(count_rows(fixture, "SELECT count(*) FROM r2_log_links WHERE relationship='hands_result_in_context';") == 1);
    assert(sqlite3_close(fixture) == SQLITE_OK);
    r2_log_shutdown();
}

static void check_completion_persistence_retry(void)
{
    assert(r2_log_init() == 0);
    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    for (int failure = 0; failure < 2; ++failure) {
        const char *id = failure ? "sqlfail" : "fmtfail";
        const char *command = failure ? "printf sql-exact-output" : "printf format-exact-output";
        int64_t receipt = r2_log_accept_hands(id, command);
        assert(receipt > 0);
        int before = count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;");
        if (failure)
            assert(sqlite3_exec(fixture,
                "CREATE TRIGGER block_completion BEFORE INSERT ON r2_log_hands_results "
                "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
        else
            fail_completion_format = 1;
        execute_task(id, command, receipt);
        assert(!fail_completion_format && completed_head &&
               completed_head->receipt == receipt && !completed_head->durable_handoff);
        assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 1);
        assert(count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;") == before);
        char archive[PATH_MAX];
        memcpy(archive, completed_head->archive_path, sizeof(archive));
        assert(access(archive, F_OK) == 0);
        if (failure) {
            char *blocked = collect_completed();
            assert(blocked && !*blocked && completed_head && completed_head->receipt == receipt);
            free(blocked);
            assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 1);
            assert(sqlite3_exec(fixture, "DROP TRIGGER block_completion;", NULL, NULL, NULL) == SQLITE_OK);
        }
        char *retried = collect_completed();
        assert(retried && !*retried && !completed_head && !completed_tail);
        free(retried);
        assert(count_rows(fixture, "SELECT count(*) FROM r2_log_pending_hands;") == 0);
        assert(count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;") == before + 1);
        sqlite3_stmt *st = NULL;
        assert(sqlite3_prepare_v2(fixture,
            "SELECT r.result FROM r2_log_hands_results r JOIN r2_log_links l "
            "ON l.from_event_id=r.outcome_id WHERE l.to_event_id=? AND l.relationship='hands_outcome_for';",
            -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_int64(st, 1, receipt) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
        const char *stored = (const char *)sqlite3_column_text(st, 0);
        assert(stored && strstr(stored, command) && strstr(stored, archive) &&
               strstr(stored, failure ? "Output:\nsql-exact-output" : "Output:\nformat-exact-output"));
        assert(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        retried = collect_completed();
        assert(retried && !*retried);
        free(retried);
        assert(count_rows(fixture, "SELECT count(*) FROM r2_log_hands_results;") == before + 1);
        assert(unlink(archive) == 0);
    }
    assert(sqlite3_close(fixture) == SQLITE_OK);
    r2_log_shutdown();
}

int main(void)
{
    check_shutdown("printf started; sleep 30 & wait");
    check_shutdown("printf started; exec >/dev/null 2>&1; sleep 30");

    check_complete_output_archive();
    atomic_store(&shutting_down, 0);
    check_action_output_allocation_failure();
    check_workspace_path_with_spaces();
    check_console_shutdown("exec sleep 30");
    check_console_shutdown("exec >/dev/null; exec sleep 30");
    atomic_store(&shutting_down, 0);
    check_durable_receipts();
    check_completion_persistence_retry();
    check_hands_exact_transactions();
    puts("R2 active task shutdown, exact output archive and durable receipt smoke passed.");
    return 0;
}
