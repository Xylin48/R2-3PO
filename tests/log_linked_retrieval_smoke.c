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
    if (seed <= 0 || linked <= 0 || other <= 0 ||
        r2_log_link(seed, linked, "identity_evidence",
                    "explicit statement stored as a durable relationship link") != 0 ||
        r2_log_link(linked, other, "relationship_context",
                    "another explicit edge from the matching relationship event") != 0) {
        fprintf(stderr, "Could not create linked identity test records.\n");
        r2_log_shutdown();
        return 1;
    }

    char *from_seed = r2_log_search("identity", 10);
    char *from_relation = r2_log_search("explicit current-user identity relationship", 10);
    int ok = from_seed && from_relation &&
             strstr(from_seed, "explicit current-user identity relationship") &&
             strstr(from_seed, "identity_evidence") &&
             strstr(from_seed, "subject=current_user; relation=mother_of; object=R2") &&
             strstr(from_seed, "related prior experience") &&
             strstr(from_seed, "relationship_context") &&
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
    if (!ok || !memory_update_ok) return 1;
    puts("Life Log linked retrieval and failed memory_saved bookkeeping are handled correctly.");
    return 0;
}
