#include "Log.h"
#include "SensoryJournal.h"
#include "Addiction.h"
#include "r2.h"
#include "r2_diary.h"

#include <sqlite3.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id; (void)category; (void)event_type; (void)source;
}

int r2_save_memory(const char *memory, const char *category)
{
    (void)memory; (void)category;
    return 0;
}

int r2_addiction_record_choice(const char *target, const char *target_type,
                               int enjoyment_0_100, const char *source,
                               const char *details)
{
    (void)target; (void)target_type; (void)enjoyment_0_100;
    (void)source; (void)details;
    return 0;
}

int main(void)
{
    if (r2_log_init() != 0) {
        fprintf(stderr, "Life Log initialization failed.\n");
        return 1;
    }

    int64_t other = r2_log_event(R2_LOG_MEMORY, "related_experience",
                                 "related prior experience",
                                 "context reachable through explicit relationship links",
                                 "link-smoke");
    int64_t linked = r2_log_event(R2_LOG_MEMORY, "identity_relation",
                                  "explicit current-user identity relationship",
                                  "subject=current_user; relation=mother_of; object=R2",
                                  "link-smoke");
    int64_t seed = r2_log_event(R2_LOG_MEMORY, "identity_seed",
                                "unique identity seed event",
                                "the user identity is linked from this event",
                                "link-smoke");
    char notes[70001];
    memset(notes, 'N', sizeof(notes) - 1);
    memcpy(notes + sizeof(notes) - sizeof("VISUAL-SOURCE-TAIL"),
           "VISUAL-SOURCE-TAIL", sizeof("VISUAL-SOURCE-TAIL"));
    if (seed <= 0 || linked <= 0 || other <= 0 ||
        r2_log_link(seed, linked, "identity_evidence",
                    "explicit statement stored as a durable relationship link") != 0 ||
        r2_log_link(linked, other, "relationship_context",
                    notes) != 0) {
        fprintf(stderr, "Could not create linked identity test records.\n");
        r2_log_shutdown();
        return 1;
    }

    sqlite3 *edges = NULL;
    int edge_ok = sqlite3_open(R2_DIARY_DATABASE, &edges) == SQLITE_OK &&
        sqlite3_exec(edges,
            "CREATE TRIGGER smoke_skip_edge BEFORE INSERT ON r2_log_links "
            "WHEN NEW.relationship='skipped_edge' BEGIN SELECT RAISE(IGNORE); END; "
            "CREATE TRIGGER smoke_remove_edge AFTER INSERT ON r2_log_links "
            "WHEN NEW.relationship='removed_edge' BEGIN DELETE FROM r2_log_links WHERE id=NEW.id; END;",
            NULL, NULL, NULL) == SQLITE_OK &&
        r2_log_link(seed, linked, "identity_evidence", "duplicate retry") == 0 &&
        r2_log_link(seed, linked, "skipped_edge", NULL) != 0 &&
        r2_log_link(seed, linked, "removed_edge", NULL) != 0 &&
        r2_log_link(seed, INT64_MAX, "missing_endpoint", NULL) != 0;
    sqlite3_stmt *edge_count = NULL;
    if (edge_ok)
        edge_ok = sqlite3_prepare_v2(edges,
            "SELECT COUNT(*) FROM r2_log_links WHERE relationship IN "
            "('identity_evidence','skipped_edge','removed_edge','missing_endpoint');",
            -1, &edge_count, NULL) == SQLITE_OK &&
            sqlite3_step(edge_count) == SQLITE_ROW && sqlite3_column_int(edge_count, 0) == 1;
    sqlite3_finalize(edge_count);
    sqlite3_close(edges);
    if (!edge_ok)
        fprintf(stderr, "Stored relationship verification failed.\n");

    char *from_seed = r2_log_search("identity", 10);
    char *from_relation = r2_log_search("explicit current-user identity relationship", 10);
    int ok = from_seed && from_relation &&
             strstr(from_seed, "explicit current-user identity relationship") &&
             strstr(from_seed, "identity_evidence") &&
             strstr(from_seed, "subject=current_user; relation=mother_of; object=R2") &&
             strstr(from_seed, "related prior experience") &&
             strstr(from_seed, "relationship_context") &&
             strstr(from_seed, notes) &&
             strstr(from_relation, "unique identity seed event") &&
             strstr(from_relation, "identity_evidence") &&
             strstr(from_relation, "related prior experience");
    if (!ok) {
        fprintf(stderr, "Life Log search did not traverse explicit links in both directions.\n");
        if (from_seed) fprintf(stderr, "Seed search:\n%s\n", from_seed);
        if (from_relation) fprintf(stderr, "Relation search:\n%s\n", from_relation);
    }
    free(from_seed);
    free(from_relation);

    char archived_user[6001], archived_reply[6001];
    memset(archived_user, 'U', sizeof(archived_user) - 1);
    memset(archived_reply, 'A', sizeof(archived_reply) - 1);
    memcpy(archived_user + sizeof(archived_user) - sizeof("deep_archive%marker"),
           "deep_archive%marker", sizeof("deep_archive%marker"));
    memcpy(archived_reply + sizeof(archived_reply) - sizeof("assistant archive tail"),
           "assistant archive tail", sizeof("assistant archive tail"));
    int64_t archived = r2_log_conversation_turn(archived_user, archived_reply);
    int64_t anchor = r2_log_event(R2_LOG_MEMORY, "archive_anchor",
                                  "deep_archive%marker", "related pointer", "link-smoke");
    R2LogRecordBatch records = {0};
    int archive_ok = archived > 0 && anchor > archived &&
        r2_log_link(anchor, archived, "archive_evidence", NULL) == 0 &&
        r2_log_search_records("deep_archive%marker", 10, &records) == 0 &&
        records.count == 2 && strstr(records.text, archived_user) &&
        strstr(records.text, archived_reply);
    r2_log_release_records(&records);
    char *literal_miss = r2_log_search("deepXarchive%marker", 10);
    archive_ok = archive_ok && literal_miss &&
        strstr(literal_miss, "No Life Log events matched");
    free(literal_miss);
    char *reply_match = r2_log_search("assistant archive tail", 10);
    archive_ok = archive_ok && reply_match &&
        strstr(reply_match, archived_user) && strstr(reply_match, archived_reply);
    free(reply_match);
    if (!archive_ok)
        fprintf(stderr, "Life Log did not return the complete original archived turn.\n");

    /* Legacy rows can contain bytes that the text interface cannot render. */
    sqlite3 *legacy = NULL;
    int exact_ok = sqlite3_open(R2_DIARY_DATABASE, &legacy) == SQLITE_OK &&
        sqlite3_exec(legacy,
            "UPDATE r2_log_events SET details='prefix'||char(0)||'tail' "
            "WHERE event_type='related_experience';", NULL, NULL, NULL) == SQLITE_OK;
    if (legacy) sqlite3_close(legacy);
    if (exact_ok) {
        char *direct = r2_log_search("related prior experience", 10);
        char *indirect = r2_log_search("explicit current-user identity relationship", 10);
        exact_ok = !direct && !indirect;
        free(direct);
        free(indirect);
    }
    if (!exact_ok)
        fprintf(stderr, "Life Log returned shortened text for an unrepresentable record.\n");

    /* A memory_saved bookkeeping failure must be reported, not silently
       treated as a successful cross-database update. */
    int memory_update_ok = 1;
    sqlite3 *test_db = NULL;
    if (sqlite3_open(R2_DIARY_DATABASE, &test_db) != SQLITE_OK) {
        fprintf(stderr, "Could not open Life Log DB for memory_saved failure injection.\\n");
        memory_update_ok = 0;
    } else if (sqlite3_exec(test_db,
        "CREATE TRIGGER smoke_fail_memory_saved "
        "BEFORE UPDATE OF memory_saved ON r2_log_events "
        "BEGIN SELECT RAISE(ABORT,'intentional memory_saved failure'); END",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "Could not install memory_saved failure trigger.\\n");
        memory_update_ok = 0;
    }
    if (test_db) sqlite3_close(test_db);

    int64_t event_id = -1;
    if (memory_update_ok) {
        event_id = r2_log_event_with_memory(
            R2_LOG_MEMORY, "memory_saved_failure_regression",
            "memory_saved bookkeeping failure test",
            "The Life Log event should remain authoritative after a pointer-update failure.",
            "log-linked-retrieval-smoke", 1);
        if (event_id <= 0) {
            fprintf(stderr, "Life Log event was lost during memory_saved failure injection.\\n");
            memory_update_ok = 0;
        }
    }

    test_db = NULL;
    sqlite3_stmt *statement = NULL;
    if (memory_update_ok &&
        (sqlite3_open(R2_DIARY_DATABASE, &test_db) != SQLITE_OK ||
         sqlite3_prepare_v2(test_db,
            "SELECT memory_saved FROM r2_log_events WHERE id=?",
            -1, &statement, NULL) != SQLITE_OK ||
         sqlite3_bind_int64(statement, 1, event_id) != SQLITE_OK ||
         sqlite3_step(statement) != SQLITE_ROW ||
         sqlite3_column_int(statement, 0) != 0)) {
        fprintf(stderr, "Life Log did not preserve the event after memory_saved update failure.\\n");
        memory_update_ok = 0;
    }
    if (statement) sqlite3_finalize(statement);
    if (test_db) {
        if (sqlite3_exec(test_db, "DROP TRIGGER IF EXISTS smoke_fail_memory_saved",
                         NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "Could not remove memory_saved failure trigger.\\n");
            memory_update_ok = 0;
        }
        sqlite3_close(test_db);
    }

    r2_log_shutdown();
    if (!ok || !memory_update_ok || !edge_ok || !exact_ok || !archive_ok) return 1;
    puts("Life Log linked retrieval and failed memory_saved bookkeeping are handled correctly.");
    return 0;
}
