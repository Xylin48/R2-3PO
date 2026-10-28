#define _GNU_SOURCE
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
#include <unistd.h>

#ifndef R2_DIARY_DATABASE
#error R2_DIARY_DATABASE must point to an isolated test database
#endif

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

static void remove_if_present(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT) {
        perror(path);
        exit(1);
    }
}

static int scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL;
    int value = -1;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

int main(void)
{
    char sidecar[sizeof(R2_DIARY_DATABASE) + 8];
    remove_if_present(R2_DIARY_DATABASE);
    snprintf(sidecar, sizeof(sidecar), "%s-wal", R2_DIARY_DATABASE);
    remove_if_present(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s-shm", R2_DIARY_DATABASE);
    remove_if_present(sidecar);

    sqlite3 *legacy = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &legacy) == SQLITE_OK);
    assert(sqlite3_exec(legacy,
        "CREATE TABLE r2_log_events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "summary TEXT NOT NULL,details TEXT);"
        "INSERT INTO r2_log_events(id,summary,details) VALUES"
        "(7,'legacy preserved identity record',"
        "'Existing Life Log content must remain readable after additive migration.'),"
        "(8,'legacy linked endpoint record','Preserve old link target identity.');"
        "CREATE TABLE r2_log_activity_sessions(id INTEGER PRIMARY KEY,activity_key TEXT,"
        "activity_name TEXT,started_utc TEXT,status TEXT);"
        "INSERT INTO r2_log_activity_sessions VALUES(1,'legacy-game','Legacy Game',"
        "'2020-01-01T00:00:00Z','closed');"
        "CREATE TABLE r2_log_milestones(id INTEGER PRIMARY KEY,milestone_key TEXT,description TEXT);"
        "INSERT INTO r2_log_milestones VALUES(1,'legacy-first','Old milestone');"
        "CREATE TABLE r2_log_conversations(id INTEGER PRIMARY KEY,user_text TEXT,assistant_text TEXT);"
        "INSERT INTO r2_log_conversations VALUES(1,'old question','old answer');"
        "CREATE TABLE r2_log_inner_states(id INTEGER PRIMARY KEY,state_kind TEXT,description TEXT,"
        "evidence TEXT,confidence REAL,origin TEXT);"
        "INSERT INTO r2_log_inner_states VALUES(1,'curious','Old state','legacy evidence',0.7,'old beta');"
        "CREATE TABLE r2_log_beliefs(id INTEGER PRIMARY KEY,belief_key TEXT,belief TEXT,evidence TEXT,"
        "confidence REAL,status TEXT,origin TEXT);"
        "INSERT INTO r2_log_beliefs VALUES(1,'old-belief','Legacy belief','old evidence',0.6,'tentative','old beta');"
        "CREATE TABLE r2_log_continuity(id INTEGER PRIMARY KEY,item_key TEXT,item_type TEXT,title TEXT,"
        "description TEXT,status TEXT,next_action TEXT,origin TEXT);"
        "INSERT INTO r2_log_continuity VALUES(1,'old-task','task','Old task','legacy task details','open','continue','old beta');"
        "CREATE TABLE r2_log_hypotheticals(id INTEGER PRIMARY KEY,scenario TEXT,assumptions TEXT,"
        "predicted_outcome TEXT,conclusion TEXT,origin TEXT);"
        "INSERT INTO r2_log_hypotheticals VALUES(1,'Old scenario','Old assumptions','Old prediction','Old conclusion','old beta');"
        "CREATE TABLE r2_log_system_snapshots(id INTEGER PRIMARY KEY,hostname TEXT,operating_system TEXT,"
        "kernel_release TEXT,machine TEXT,process_id INTEGER,process_elapsed_ms INTEGER);"
        "INSERT INTO r2_log_system_snapshots VALUES(1,'old-host','old-os','old-kernel','x86_64',123,42);"
        "CREATE TABLE r2_log_links(id INTEGER PRIMARY KEY,event_id INTEGER,related_event_id INTEGER,relationship TEXT);"
        "INSERT INTO r2_log_links VALUES(1,7,8,'related');",
        NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(legacy);

    assert(r2_log_init() == 0);
    char *old_record = r2_log_search("legacy preserved identity", 10);
    assert(old_record != NULL);
    assert(strstr(old_record, "legacy preserved identity record") != NULL);
    free(old_record);

    int64_t new_id = r2_log_event(
        R2_LOG_MEMORY, "post_migration_write",
        "new event after legacy migration",
        "A writable post-migration event.", "legacy-schema-smoke");
    assert(new_id > 0);

    char *new_record = r2_log_search("new event after legacy migration", 10);
    assert(new_record != NULL);
    assert(strstr(new_record, "new event after legacy migration") != NULL);
    free(new_record);

    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_events WHERE id=7 "
        "AND summary='legacy preserved identity record'") == 1);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_events WHERE event_type='post_migration_write'") == 1);
    assert(scalar(db,
        "SELECT (SELECT COUNT(*) FROM r2_log_activity_sessions s JOIN r2_log_events e ON e.id=s.start_event_id WHERE s.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_milestones m JOIN r2_log_events e ON e.id=m.first_event_id WHERE m.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_conversations c JOIN r2_log_events e ON e.id=c.event_id WHERE c.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_inner_states s JOIN r2_log_events e ON e.id=s.event_id WHERE s.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_beliefs b JOIN r2_log_events e ON e.id=b.last_event_id WHERE b.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_continuity c JOIN r2_log_events e ON e.id=c.last_event_id WHERE c.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_hypotheticals h JOIN r2_log_events e ON e.id=h.event_id WHERE h.id=1) + "
        "(SELECT COUNT(*) FROM r2_log_system_snapshots s JOIN r2_log_events e ON e.id=s.event_id WHERE s.id=1)") == 8);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_links l "
        "JOIN r2_log_events f ON f.id=l.from_event_id "
        "JOIN r2_log_events t ON t.id=l.to_event_id "
        "WHERE l.id=1 AND l.from_event_id=7 AND l.to_event_id=8") == 1);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_events WHERE event_type='legacy_log_link_restored'") >= 8);
    sqlite3_stmt *integrity = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1,
                              &integrity, NULL) == SQLITE_OK);
    assert(sqlite3_step(integrity) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(integrity, 0), "ok") == 0);
    sqlite3_finalize(integrity);
    sqlite3_close(db);

    r2_log_shutdown();
    puts("Life Log legacy schema migration preserved old events and accepted new writable events.");
    return 0;
}
