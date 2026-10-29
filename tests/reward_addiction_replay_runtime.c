/*
 * Exercise the real Reward.c cross-organ delivery path with a deterministic
 * Addiction receiver. SQLite reward rows are real; the receiver simulates
 * failure before delivery and retry after an interrupted acknowledgement.
 */
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define R2_HOME "/tmp/r2-reward-addiction-replay-fixture"
#include "../Reward.c"

static int log_ready = 0;
static int fail_first_delivery = 1;
static int delivery_attempts = 0;
static int choice_events = 0;
static char last_choice_details[1600];

int r2_log_is_initialized(void) { return log_ready; }
int64_t r2_log_event_with_memory(R2LogCategory category,
                                const char *event_type, const char *summary,
                                const char *details, const char *source,
                                int save_as_memory)
{
    (void)category; (void)event_type; (void)summary;
    (void)details; (void)source; (void)save_as_memory;
    return -1;
}
int r2_save_memory(const char *text, const char *category)
{
    (void)text; (void)category;
    return -1;
}
int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment, const char *source,
                               const char *details)
{
    assert(target && *target && strcmp(target_type, "activity") == 0);
    assert(enjoyment >= 0 && enjoyment <= 100);
    assert(strcmp(source, "reinforcement") == 0);
    assert(strstr(details, "reward_event_id=") == details);
    ++delivery_attempts;
    if (fail_first_delivery) {
        fail_first_delivery = 0;
        return -1; /* Reward committed but no Addiction event was written. */
    }
    if (strcmp(last_choice_details, details) != 0) {
        ++choice_events;
        snprintf(last_choice_details, sizeof(last_choice_details), "%s", details);
    }
    return 0;
}
static int scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    value = sqlite3_column_int(st, 0);
    assert(sqlite3_finalize(st) == SQLITE_OK);
    return value;
}
static void exec_sql(sqlite3 *db, const char *sql)
{
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "Fixture SQL failed: %s\n", error ? error : "unknown");
        sqlite3_free(error);
        abort();
    }
}
int main(void)
{
    char path[sizeof(REWARD_DB_PATH) + 8U];
    for (int i = 0; i < 3; ++i) {
        snprintf(path, sizeof(path), "%s%s", REWARD_DB_PATH,
                 i == 0 ? "" : i == 1 ? "-wal" : "-shm");
        if (unlink(path) != 0) assert(errno == ENOENT);
    }
    if (rmdir(R2_HOME) != 0) assert(errno == ENOENT);
    assert(mkdir(R2_HOME, 0700) == 0);

    assert(r2_reward_init() == 0);
    assert(r2_reward_apply_once("reading", "habit_choice", 3,
                                "R2 made a deliberate reading choice", 1) == 0);
    assert(delivery_attempts == 1 && choice_events == 0);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_events") == 1);
    assert(scalar(reward_db, "SELECT reward_event_id=(SELECT id FROM reward_events LIMIT 1) FROM reward_once_keys") == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 3);
    assert(scalar(reward_db, "SELECT voluntary_choice FROM reward_events") == 1);
    assert(scalar(reward_db, "SELECT addiction_choice_indexed FROM reward_events") == 0);

    /* Simulate completion of an independent Life Log/memory link. The
       recovery scan must still find the missing Addiction delivery. */
    exec_sql(reward_db,
             "UPDATE reward_events SET log_event_id=789,memory_indexed=1");
    log_ready = 1;
    assert(r2_reward_reconnect_history(1) == 1);
    assert(delivery_attempts == 2 && choice_events == 1);
    assert(scalar(reward_db,
        "SELECT addiction_choice_indexed FROM reward_events") == 1);
    assert(r2_reward_reconnect_history(1) == 0);
    assert(delivery_attempts == 2 && choice_events == 1);

    /* Restart and retry never add reward points or duplicate a choice. */
    r2_reward_shutdown();
    assert(r2_reward_init() == 0);
    assert(r2_reward_apply_once("reading", "habit_choice", 3,
                                "same deliberate choice", 1) == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 3);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_events") == 1);
    assert(delivery_attempts == 2 && choice_events == 1);

    /* Simulate a crash after Addiction accepted its event but before Reward
       could save the marker. The receiver's stable reward ID deduplicates. */
    exec_sql(reward_db,
             "UPDATE reward_events SET addiction_choice_indexed=0");
    assert(r2_reward_reconnect_history(1) == 1);
    assert(delivery_attempts == 3 && choice_events == 1);
    assert(scalar(reward_db,
        "SELECT addiction_choice_indexed FROM reward_events") == 1);

    /* Prior-version rows remain unclassified until a genuine retry tells us
       whether their original action was a voluntary reinforcement. */
    exec_sql(reward_db,
        "INSERT INTO reward_events(target,source,points,reason,occurred_at,"
        "modifier,expires_at,log_event_id,memory_indexed) "
        "VALUES('legacy','once',2,'prior version',100,4,1900,800,1);"
        "INSERT INTO reward_once_keys(target,source,created_at) VALUES('legacy','once',100);");
    assert(scalar(reward_db,
        "SELECT voluntary_choice FROM reward_events WHERE target='legacy'") == 0);
    assert(r2_reward_apply_once("legacy", "once", 2,
                                "retry explicitly declared voluntary", 1) == 1);
    assert(scalar(reward_db,
        "SELECT voluntary_choice=1 AND addiction_choice_indexed=1 "
        "FROM reward_events WHERE target='legacy'") == 1);
    assert(scalar(reward_db,
        "SELECT COUNT(*) FROM reward_events WHERE target='legacy'") == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 3);
    assert(choice_events == 2);
    assert(scalar(reward_db,
        "SELECT reward_event_id=(SELECT id FROM reward_events WHERE target='legacy') "
        "FROM reward_once_keys WHERE target='legacy'") == 1);

    /* Ordinary rewards are not one-time keys. The same target/source can
       have an ordinary reward followed by exactly one idempotent reward. */
    assert(r2_reward_apply("shared","source",1,"ordinary event",0) == 0);
    assert(r2_reward_apply_once("shared","source",2,"one-time event",0) == 0);
    assert(r2_reward_apply_once("shared","source",2,"repeated once",0) == 1);
    assert(scalar(reward_db, "SELECT COUNT(*) FROM reward_events WHERE target='shared'") == 2);
    assert(scalar(reward_db,
        "SELECT reward_event_id=(SELECT id FROM reward_events "
        "WHERE target='shared' AND reason='one-time event') "
        "FROM reward_once_keys WHERE target='shared'") == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 6);

    /* A single older reward can be recovered even if its key never had
       a timestamp: no alternative event identity exists for that pair. */
    exec_sql(reward_db,
        "INSERT INTO reward_events(target,source,points,reason,occurred_at,"
        "modifier,expires_at,log_event_id,memory_indexed) "
        "VALUES('oldsingle','once',1,'original',90,2,1900,803,1);"
        "INSERT INTO reward_once_keys(target,source,created_at) "
        "VALUES('oldsingle','once',0);");
    assert(r2_reward_apply_once("oldsingle","once",1,"replayed",0) == 1);
    assert(scalar(reward_db,
        "SELECT reward_event_id=(SELECT id FROM reward_events WHERE target='oldsingle') "
        "FROM reward_once_keys WHERE target='oldsingle'") == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 6);

    /* An ancient key with two candidate rewards and no original timestamp
       must not assign its identity to either row or create a third. */
    exec_sql(reward_db,
        "INSERT INTO reward_events(target,source,points,reason,occurred_at,"
        "modifier,expires_at,log_event_id,memory_indexed) VALUES"
        "('ambiguous','once',1,'first',101,2,1900,801,1),"
        "('ambiguous','once',2,'second',102,4,1900,802,1);"
        "INSERT INTO reward_once_keys(target,source,created_at)"
        "VALUES('ambiguous','once',0);");
    assert(r2_reward_apply_once("ambiguous","once",3,"cannot guess",0) == -1);
    assert(scalar(reward_db,
        "SELECT COUNT(*) FROM reward_events WHERE target='ambiguous'") == 2);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 6);

    /* An interrupted old key-only write has no event to claim. Create
       exactly one, attach its precise ID, and preserve existing key row. */
    exec_sql(reward_db,
        "INSERT INTO reward_once_keys(target,source,created_at) "
        "VALUES('unfinished','once',0)");
    assert(r2_reward_apply_once("unfinished","once",1,"complete old key",0) == 0);
    assert(r2_reward_apply_once("unfinished","once",1,"do not duplicate",0) == 1);
    assert(scalar(reward_db,
        "SELECT reward_event_id=(SELECT id FROM reward_events "
        "WHERE target='unfinished') FROM reward_once_keys WHERE target='unfinished'") == 1);
    assert(scalar(reward_db, "SELECT points FROM reward_totals WHERE id=1") == 7);
    r2_reward_shutdown();
    puts("Reward-to-Addiction crash replay, legacy intent upgrade and no duplicate choices passed.");
    return 0;
}
