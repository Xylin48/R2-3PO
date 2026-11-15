#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

int r2_init(void);
void r2_shutdown(void);
char *r2_process_tools(const char *input);

static int query_count(sqlite3 *db, const char *query)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db, query, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return count;
}

static void check_location_reply(const char *action, const char *expected)
{
    char *reply = r2_process_tools(action);
    assert(reply && strstr(reply, expected));
    free(reply);
}

static void test_world_and_welcome_replay(void)
{
    assert(r2_init() == 0);
    sqlite3 *memory = NULL;
    assert(sqlite3_open_v2(R2_DIARY_DATABASE, &memory,
                          SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK);
    check_location_reply("[WORLD] add|fixture book|A sample book|pockets", "REAL WORLD RESULT");
    check_location_reply("[WORLD] remove|fixture book|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] move|fixture book|shelf|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] self|fixture|value|evidence|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] activity_start|fixture|Sample|details|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] money_withdraw|1.00|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] eat|fixture book|auto|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] sleep|8|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] tv_power|on|extra", "REAL WORLD ERROR");
    check_location_reply("[WORLD] tv_tune|99", "could not select RF channel 99");
    check_location_reply("[WORLD] gameboy_power_on|extra", "unsupported Game Boy console action");
    check_location_reply("[WORLD] remove|fixture book", "REAL WORLD RESULT");
    check_location_reply("[WORLD] location|library|outside|extra", "LOCATION ERROR");
    assert(query_count(memory, "SELECT count(*) FROM r2_log_events "
                               "WHERE event_type='location_changed'") == 0);
    check_location_reply("[WORLD] location|library|outside", "LOCATION RESULT");
    assert(sqlite3_exec(memory,
        "CREATE TRIGGER ignore_welcome BEFORE INSERT ON memories "
        "WHEN NEW.category='private_notification' BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    check_location_reply("[WORLD] location|home|home", "LOCATION PENDING");
    const char *welcome_count = "SELECT count(*) FROM memories "
        "WHERE category='private_notification' AND memory LIKE 'Welcome Home.%'";
    assert(query_count(memory, welcome_count) == 0);
    r2_shutdown();

    assert(sqlite3_exec(memory, "DROP TRIGGER ignore_welcome;",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_init() == 0);
    assert(query_count(memory, welcome_count) == 1);
    assert(query_count(memory, "SELECT welcome_pending FROM r2_log_location_state WHERE id=1") == 0);
    check_location_reply("[WORLD] location|home|home", "LOCATION RESULT");
    assert(query_count(memory, welcome_count) == 1);
    assert(query_count(memory, "SELECT count(*) FROM r2_log_events "
                               "WHERE event_type='location_changed'") == 2);
    assert(query_count(memory, "SELECT count(*) FROM r2_log_events "
                               "WHERE event_type='internal_notification'") == 1);
    sqlite3_close(memory);
    r2_shutdown();
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s hands|diary|locked|welcome\n", argv[0]);
        return 2;
    }

    if (!strcmp(argv[1], "locked")) {
        if (mkdir(R2_HOME, 0700) != 0 && errno != EEXIST) return 1;
        int lock = open(R2_HOME, O_RDONLY);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) return 1;
        pid_t child = fork();
        if (child == 0) {
            close(lock);
            int result = r2_init();
            r2_shutdown();
            _exit(result == -1 ? 0 : 1);
        }
        int status = 0;
        if (child < 0 || waitpid(child, &status, 0) != child ||
            !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
            access(R2_HOME "/r2_memory.db", F_OK) == 0) {
            fprintf(stderr, "a second core reached persistent initialization while the directory was locked\n");
            return 1;
        }
        close(lock);
        return 0;
    }

    if (!strcmp(argv[1], "welcome")) {
        test_world_and_welcome_replay();
        return 0;
    }

    const char *failure_env = NULL;
    if (strcmp(argv[1], "hands") == 0) {
        failure_env = "R2_TEST_FAIL_HANDS_THREAD_CREATE";
    } else if (strcmp(argv[1], "diary") == 0) {
        failure_env = "R2_TEST_FAIL_DIARY_THREAD_CREATE";
    } else {
        fprintf(stderr, "unknown startup failure scenario: %s\n", argv[1]);
        return 2;
    }

    if (setenv(failure_env, "1", 1) != 0) {
        perror("setenv");
        return 2;
    }

    if (r2_init() == 0) {
        fprintf(stderr, "r2_init unexpectedly succeeded with %s injected\n",
                failure_env);
        r2_shutdown();
        return 1;
    }

    /* This must be safe after either partial-startup failure. The test process
       exits only after the same centralized cleanup path used by main(). */
    r2_shutdown();
    int lock = open(R2_HOME, O_RDONLY);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "startup unwind left the persistent directory locked\n");
        return 1;
    }
    close(lock);
    return 0;
}
