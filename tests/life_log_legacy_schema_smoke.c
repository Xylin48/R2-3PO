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

static int journal_index_calls = 0;

void r2_sj_index_event(int64_t event_id, const char *category,
                       const char *event_type, const char *source)
{
    (void)event_id; (void)category; (void)event_type; (void)source;
    ++journal_index_calls;
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
        "INSERT INTO r2_log_conversations VALUES(2,printf('%.*c',10000,'u')||'LEGACY-USER-END',"
        "printf('%.*c',12000,'a')||'LEGACY-ANSWER-END');"
        "INSERT INTO r2_log_conversations VALUES(3,'left'||char(0)||'right','old answer');"
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

    char long_source[1600];
    memset(long_source, 'Z', sizeof(long_source) - 1);
    long_source[0] = 'S';
    long_source[sizeof(long_source) - 1] = '\0';
    int64_t audio_id = r2_log_event(
        R2_LOG_SENSORY, "long_audio_source_identity",
        "Long microphone monitor identity must survive persistence.",
        NULL, long_source);
    assert(audio_id > 0);

    /* SQLITE_DONE can describe an ignored insert. It must not return the
     * previous event ID, index it as new evidence, or archive a fabricated turn. */
    sqlite3 *writer = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &writer) == SQLITE_OK);
    assert(sqlite3_exec(writer,
        "CREATE TRIGGER ignore_fixture_events BEFORE INSERT ON r2_log_events "
        "WHEN NEW.event_type='ignored_fixture' OR "
        "(NEW.event_type='conversation_turn' AND NEW.summary LIKE '%ignored exact turn%') "
        "BEGIN SELECT RAISE(IGNORE); END;"
        "CREATE TRIGGER ignore_fixture_sessions BEFORE INSERT ON r2_log_sessions "
        "BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_link(7, 8, "related", "retry of a migrated edge") == 0);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_links WHERE from_event_id=7 "
                          "AND to_event_id=8 AND relationship='related';") == 1);
    int indexed = journal_index_calls;
    int event_count = scalar(writer, "SELECT COUNT(*) FROM r2_log_events;");
    int session_count = scalar(writer, "SELECT COUNT(*) FROM r2_log_sessions;");
    int actual_session = scalar(writer, "SELECT MAX(id) FROM r2_log_sessions;");
    assert(r2_log_sensory("ignored_fixture", "not persisted", NULL, "fixture") < 0);
    assert(journal_index_calls == indexed);
    assert(r2_log_conversation_turn("ignored exact turn", "not persisted") < 0);
    assert(journal_index_calls == indexed);
    assert(r2_log_session_start() == -1);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_events;") == event_count);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_sessions;") == session_count);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_conversations "
                          "WHERE user_text='ignored exact turn';") == 0);
    assert(sqlite3_exec(writer,
        "DROP TRIGGER ignore_fixture_events; DROP TRIGGER ignore_fixture_sessions;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_sensory("fixture_after_ignore", "actually persisted", NULL, "fixture") > 0);
    assert(journal_index_calls == indexed + 1);
    assert(scalar(writer, "SELECT session_id FROM r2_log_events "
                          "WHERE event_type='fixture_after_ignore';") == actual_session);

    assert(sqlite3_exec(writer,
        "CREATE TRIGGER ignore_activity BEFORE INSERT ON r2_log_activity_sessions "
        "WHEN NEW.activity_key='restart-fixture' BEGIN SELECT RAISE(IGNORE); END;"
        "CREATE TRIGGER ignore_milestone BEFORE INSERT ON r2_log_milestones "
        "WHEN NEW.milestone_key='test-first' BEGIN SELECT RAISE(IGNORE); END;"
        "CREATE TRIGGER ignore_location BEFORE INSERT ON r2_log_events "
        "WHEN NEW.event_type='location_changed' BEGIN SELECT RAISE(IGNORE); END;"
        "CREATE TRIGGER ignore_pending_turn BEFORE INSERT ON r2_log_pending_conversations "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    event_count = scalar(writer, "SELECT COUNT(*) FROM r2_log_events;");
    assert(r2_log_activity_start("restart-fixture", "Restart fixture", NULL) < 0);
    assert(r2_log_milestone("test-first", "Milestone fixture", NULL) < 0);
    assert(r2_log_location_transition("park", 0) < 0);
    assert(r2_log_conversation_turn("ignored stage", "reply") < 0);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_events;") == event_count);
    assert(scalar(writer, "SELECT is_home FROM r2_log_location_state WHERE id=1;") == 1);
    assert(sqlite3_exec(writer,
        "DROP TRIGGER ignore_activity; DROP TRIGGER ignore_milestone; "
        "DROP TRIGGER ignore_location; DROP TRIGGER ignore_pending_turn;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_milestone("test-first", "Milestone fixture", NULL) > 0);
    assert(r2_log_milestone("test-first", "Milestone fixture", NULL) == 0);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_events WHERE source='test-first';") == 1);
    assert(r2_log_location_transition("park", 0) == 0);
    assert(r2_log_location_transition("home", 1) == 0);
    assert(r2_log_activity_start("restart-fixture", "Restart fixture", NULL) > 0);
    assert(r2_log_activity_start("restart-fixture", "Restart fixture", NULL) < 0);
    assert(sqlite3_exec(writer,
        "UPDATE r2_log_activity_sessions SET start_monotonic_ms=0, "
        "started_epoch=CAST(strftime('%s','now') AS INTEGER)-3600 WHERE activity_key='restart-fixture';"
        "CREATE TRIGGER ignore_activity_end BEFORE UPDATE OF end_event_id ON r2_log_activity_sessions "
        "WHEN NEW.activity_key='restart-fixture' BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_activity_end("restart-fixture", "ready", "fixture", NULL) < 0);
    assert(scalar(writer, "SELECT COUNT(*) FROM r2_log_events WHERE event_type='activity_ended' "
                          "AND source='restart-fixture';") == 0);
    assert(sqlite3_exec(writer, "DROP TRIGGER ignore_activity_end;", NULL, NULL, NULL) == SQLITE_OK);
    r2_log_shutdown();
    assert(r2_log_init() == 0);
    assert(r2_log_activity_end("restart-fixture", "ready", "fixture", NULL) > 0);
    assert(scalar(writer, "SELECT duration_ms FROM r2_log_activity_sessions "
                          "WHERE activity_key='restart-fixture';") >= 3600000);
    sqlite3_close(writer);

    sqlite3 *db = NULL;
    assert(sqlite3_open_v2(R2_DIARY_DATABASE, &db,
        SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *source_statement = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT source FROM r2_log_events WHERE id=?;",
        -1, &source_statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(source_statement, 1, audio_id) == SQLITE_OK);
    assert(sqlite3_step(source_statement) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(source_statement, 0),
                  long_source) == 0);
    sqlite3_finalize(source_statement);

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
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_conversations c JOIN r2_log_events e ON e.id=c.event_id "
        "WHERE c.id=2 AND instr(e.details,c.user_text)>0 AND instr(e.details,c.assistant_text)>0;") == 1);
    assert(scalar(db,
        "SELECT COUNT(*) FROM r2_log_conversations c JOIN r2_log_events e ON e.id=c.event_id "
        "WHERE c.id=3 AND instr(e.details,'[hex bytes] 6c656674007269676874')>0 "
        "AND hex(c.user_text)='6C656674007269676874';") == 1);
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
