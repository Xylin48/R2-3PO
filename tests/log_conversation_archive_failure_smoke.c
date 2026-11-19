#include "Log.h"
#include "r2_diary.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Life Log is exercised without external sensors or an Ollama server. */
void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id;
    (void)category;
    (void)event_type;
    (void)source;
}

static void require_query(sqlite3 *db, const char *sql, int64_t id,
                          const char *first, const char *second, int64_t expected)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, id);
    if (rc == SQLITE_OK && first)
        rc = sqlite3_bind_text(st, 2, first, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK && second)
        rc = sqlite3_bind_text(st, 3, second, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc != SQLITE_ROW || sqlite3_column_int64(st, 0) != expected) {
        fprintf(stderr, "FAIL: query result mismatch for event %lld (rc=%d)\n",
                (long long)id, rc);
        sqlite3_finalize(st);
        exit(1);
    }
    sqlite3_finalize(st);
}

static void sql(sqlite3 *db, const char *statement)
{
    char *error = NULL;
    if (sqlite3_exec(db, statement, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "FAIL: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
        exit(1);
    }
}

int main(void)
{
    const char *user = "USER_EXACT_PENDING_TURN_é_73628";
    const char *reply = "ASSISTANT_EXACT_PENDING_REPLY_🐈_52817";
    if (r2_log_init() != 0) {
        fputs("FAIL: could not initialize Life Log\n", stderr);
        return 1;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        fputs("FAIL: could not connect to fixture SQLite store\n", stderr);
        return 1;
    }
    sql(db, "CREATE TRIGGER block_archive BEFORE INSERT ON r2_log_conversations "
            "BEGIN SELECT RAISE(ABORT, 'injected archive write failure'); END;");
    int64_t event_id = r2_log_conversation_turn(user, reply);
    if (event_id <= 0) {
        fputs("FAIL: accepted conversation turn disappeared after archive failure\n", stderr);
        return 1;
    }
    require_query(db, "SELECT count(*) FROM r2_log_events "
                     "WHERE id=?1 AND event_type='conversation_turn'", event_id, NULL, NULL, 1);
    require_query(db, "SELECT count(*) FROM r2_log_pending_conversations "
                     "WHERE event_id=?1 AND user_text=?2 AND assistant_text=?3",
                  event_id, user, reply, 1);
    require_query(db, "SELECT count(*) FROM r2_log_conversations WHERE event_id=?1",
                  event_id, NULL, NULL, 0);

    if (r2_log_replay_conversation_archives() != -1) {
        fputs("FAIL: live replay reported success while archive writes remained blocked.\n", stderr);
        return 1;
    }
    sql(db, "DROP TRIGGER block_archive;");
    if (r2_log_replay_conversation_archives() != 0 ||
        r2_log_replay_conversation_archives() != 0) {
        fputs("FAIL: live replay did not repair the archive exactly once.\n", stderr);
        return 1;
    }
    require_query(db, "SELECT count(*) FROM r2_log_conversations "
                     "WHERE event_id=?1 AND user_text=?2 AND assistant_text=?3",
                  event_id, user, reply, 1);
    require_query(db, "SELECT count(*) FROM r2_log_pending_conversations WHERE event_id=?1",
                  event_id, NULL, NULL, 0);

    /* Exercise startup recovery separately with another accepted turn. */
    sql(db, "CREATE TRIGGER block_archive BEFORE INSERT ON r2_log_conversations "
            "BEGIN SELECT RAISE(ABORT, 'injected archive write failure'); END;");
    int64_t pending_after_reopen = r2_log_conversation_turn(user, reply);
    if (pending_after_reopen <= event_id) {
        fputs("FAIL: second accepted turn did not receive a new event ID.\n", stderr);
        return 1;
    }
    require_query(db, "SELECT count(*) FROM r2_log_pending_conversations WHERE event_id=?1",
                  pending_after_reopen, NULL, NULL, 1);
    sql(db, "DROP TRIGGER block_archive;");
    sqlite3_close(db);
    r2_log_shutdown();
    if (r2_log_init() != 0) {
        fputs("FAIL: restart did not reopen the durable Life Log\n", stderr);
        return 1;
    }
    if (sqlite3_open(R2_DIARY_DATABASE, &db) != SQLITE_OK) {
        fputs("FAIL: could not reopen fixture SQLite store\n", stderr);
        return 1;
    }
    require_query(db, "SELECT count(*) FROM r2_log_conversations "
                     "WHERE event_id=?1 AND user_text=?2 AND assistant_text=?3",
                  event_id, user, reply, 1);
    require_query(db, "SELECT count(*) FROM r2_log_pending_conversations WHERE event_id=?1",
                  event_id, NULL, NULL, 0);
    require_query(db, "SELECT count(*) FROM r2_log_events WHERE id=?1 "
                     "AND event_type='conversation_turn'", event_id, NULL, NULL, 1);
    require_query(db, "SELECT count(*) FROM r2_log_conversations "
                     "WHERE event_id=?1 AND user_text=?2 AND assistant_text=?3",
                  pending_after_reopen, user, reply, 1);
    require_query(db, "SELECT count(*) FROM r2_log_pending_conversations WHERE event_id=?1",
                  pending_after_reopen, NULL, NULL, 0);
    sqlite3_close(db);
    r2_log_shutdown();
    puts("PASS: accepted conversations survived failed archive writes and replayed exactly once during service and after restart.");
    return 0;
}
