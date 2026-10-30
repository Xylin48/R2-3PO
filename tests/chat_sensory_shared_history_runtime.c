/* A long chat and an actual sensory observation share durable Life Log and
 * Sensory Journal identity; full chat data and deep-tail recall survive restart.
 * No model-generated learning decision is forced by the storage test.
 */
#define _POSIX_C_SOURCE 200809L
#include "Log.h"
#include "SensoryJournal.h"
#include "Addiction.h"
#include "r2.h"

#include <assert.h>
#include <errno.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category;
    return 0;
}
int r2_addiction_record_choice(const char *target, const char *type,
                               int enjoyment, const char *source, const char *details)
{
    (void)target; (void)type; (void)enjoyment; (void)source; (void)details;
    return 0;
}
static int contains(const char *haystack, const char *needle)
{
    return haystack && strstr(haystack, needle) != NULL;
}
static void create_directory(const char *path)
{
    if (mkdir(path, 0700) != 0) assert(errno == EEXIST);
}
int main(void)
{
    create_directory(R2_ROOT);
    create_directory(R2_HOME);
    (void)unlink(R2_DIARY_DATABASE);

    sqlite3 *db = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS diary_entries ("
        "id INTEGER PRIMARY KEY, entry TEXT NOT NULL);",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);

    assert(r2_log_init() == 0);
    assert(r2_sj_init() == 0);
    int64_t sense_id = r2_log_sensory(
        "chosen_visual_observation", "R2 deliberately noticed an oak tree",
        "source=camera; observation=oak leaves in daylight",
        "primary-camera");
    assert(sense_id > 0);

    const size_t n_user = 73000, n_reply = 74000;
    const char *tail_user = "CHAT_USER_DEEP_TAIL_MOONFLOWER";
    const char *tail_reply = "CHAT_R2_DEEP_TAIL_SUMMERLIGHT";
    char *user = malloc(n_user + 1), *reply = malloc(n_reply + 1);
    assert(user && reply);
    memset(user, 'u', n_user);
    memset(reply, 'r', n_reply);
    memcpy(user + n_user - strlen(tail_user), tail_user, strlen(tail_user));
    memcpy(reply + n_reply - strlen(tail_reply), tail_reply, strlen(tail_reply));
    user[n_user] = '\0';
    reply[n_reply] = '\0';

    int64_t turn_id = r2_log_conversation_turn(user, reply);
    assert(turn_id > sense_id);
    assert(r2_sj_count() == 2);

    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    sqlite3_stmt *q = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT length(user_text),length(assistant_text),"
        "instr(user_text,?),instr(assistant_text,?) "
        "FROM r2_log_conversations WHERE event_id=?",
        -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(q, 1, tail_user, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_text(q, 2, tail_reply, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_bind_int64(q, 3, turn_id) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    assert(sqlite3_column_int64(q, 0) == (sqlite3_int64)n_user);
    assert(sqlite3_column_int64(q, 1) == (sqlite3_int64)n_reply);
    assert(sqlite3_column_int64(q, 2) > 65000);
    assert(sqlite3_column_int64(q, 3) > 65000);
    sqlite3_finalize(q);
    sqlite3_close(db);

    char *found = r2_log_search(tail_user, 5);
    assert(found && contains(found, "conversation_turn"));
    assert(contains(found, tail_user));
    assert(contains(found, "Matched full user archive excerpt"));
    free(found);
    found = r2_log_search(tail_reply, 5);
    assert(found && contains(found, tail_reply));
    assert(contains(found, "Matched full R2 archive excerpt"));
    free(found);
    found = r2_sj_search(tail_reply, 5);
    assert(found && contains(found, "conversation_turn"));
    assert(contains(found, tail_reply) && contains(found, tail_user));
    free(found);
    found = r2_sj_search("oak leaves in daylight", 5);
    assert(found && contains(found, "chosen_visual_observation"));
    free(found);

    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db,
        "SELECT details FROM r2_log_events WHERE id=?",
        -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(q, 1, turn_id) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    const char *details = (const char *)sqlite3_column_text(q, 0);
    assert(contains(details, "Excerpt only: full user text archived"));
    assert(contains(details, "Excerpt only: full R2 reply archived"));
    assert(!contains(details, tail_user));
    sqlite3_finalize(q);
    sqlite3_close(db);
    /* Archive failure rolls back the event as well; no false success ID,
       orphan event, or journal projection may claim a complete turn. */
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TRIGGER fail_archive BEFORE INSERT ON r2_log_conversations "
        "BEGIN SELECT RAISE(ABORT,'injected archive write failure'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_conversation_turn("ATOMIC_FAILURE_SENTINEL", "reply") < 0);
    assert(r2_sj_count() == 2);
    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM r2_log_events WHERE summary LIKE '%ATOMIC_FAILURE_SENTINEL%'",
        -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0) == 0);
    sqlite3_finalize(q);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_archive;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    r2_sj_shutdown();
    r2_log_shutdown();

    /* Both sensory and chat identities stay linked after a real restart. */
    assert(r2_log_init() == 0);
    assert(r2_sj_init() == 0);
    assert(r2_sj_count() == 2);
    found = r2_log_search(tail_reply, 5);
    assert(found && contains(found, tail_reply));
    free(found);
    found = r2_sj_search("oak leaves in daylight", 5);
    assert(found && contains(found, "chosen_visual_observation"));
    free(found);
    r2_sj_shutdown();
    r2_log_shutdown();
    free(user);
    free(reply);
    puts("Shared chat/sensory Life Log, full archive, deep-tail recall and restart passed.");
    return 0;
}
