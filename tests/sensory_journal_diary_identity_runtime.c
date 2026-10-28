/* Verify exact diary ID relationships survive duplicate text and restart. */
#define _POSIX_C_SOURCE 200809L
#include "SensoryJournal.h"
#include "r2_diary.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sqlite3.h>

int64_t r2_log_thinking(const char *kind, const char *summary, const char *details)
{
    (void)kind; (void)summary; (void)details;
    return -1; /* Only direct Journal links are exercised by this fixture. */
}

static void sql(sqlite3 *db, const char *statement)
{
    char *error = NULL;
    int rc = sqlite3_exec(db, statement, NULL, NULL, &error);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Fixture SQL: %s\n", error ? error : sqlite3_errmsg(db));
        sqlite3_free(error);
    }
    assert(rc == SQLITE_OK);
}

static int64_t event(sqlite3 *db, const char *kind, const char *summary,
                     const char *details)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO r2_log_events (utc_time,local_time,category,event_type,summary,details,source) "
        "VALUES ('2026-01-01','2026-01-01','thinking',?,?,?,'diary');",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 1, kind, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 2, summary, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 3, details, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

static void authoritative_link(sqlite3 *db, int64_t diary_id, int64_t event_id)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO r2_diary_entry_links(diary_entry_id,log_event_id) VALUES(?,?)",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, diary_id) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 2, event_id) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
}

static void expect_link(const char *query, const char *expected)
{
    char *result = r2_sj_search(query, 20);
    assert(result != NULL);
    if (!strstr(result, expected)) {
        fprintf(stderr, "Expected %s for %s, received:\n%s\n", expected, query, result);
        abort();
    }
    free(result);
}

int main(void)
{
    (void)mkdir(R2_ROOT, 0700);
    (void)mkdir(R2_HOME, 0700);
    (void)remove(R2_DIARY_DATABASE);
    sqlite3 *db = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    sql(db,
        "CREATE TABLE r2_log_events (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " utc_time TEXT,local_time TEXT,category TEXT,event_type TEXT,"
        " summary TEXT,details TEXT,source TEXT);"
        "CREATE TABLE diary_entries (id INTEGER PRIMARY KEY,entry TEXT);"
        "CREATE TABLE r2_diary_entry_links (diary_entry_id INTEGER PRIMARY KEY,"
        " log_event_id INTEGER);"
        "INSERT INTO diary_entries(id,entry) VALUES"
        "(101,'same diary prose'),(102,'same diary prose');");
    int64_t first = event(db, "diary_entry_linked", "Exact first diary entry",
                          "diary_entry_id=101; content_location=diary_entries");
    int64_t second = event(db, "diary_entry_linked", "Exact second diary entry",
                           "diary_entry_id=102; content_location=diary_entries");
    int64_t ambiguous = event(db, "diary_entry_written", "Ambiguous older entry",
                              "same diary prose");
    authoritative_link(db, 101, first);
    authoritative_link(db, 102, second);
    sqlite3_close(db);

    assert(r2_sj_init() == 0);
    expect_link("Exact first diary entry", "Diary link: 101");
    expect_link("Exact second diary entry", "Diary link: 102");
    expect_link("Ambiguous older entry", "Diary link: none");

    /* The direct diary->journal path works after initialization too. */
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    sql(db, "INSERT INTO diary_entries(id,entry) VALUES(103,'same diary prose');");
    int64_t third = event(db, "diary_entry_linked", "Exact new diary entry",
                          "diary_entry_id=103; content_location=diary_entries");
    authoritative_link(db, 103, third);
    sqlite3_close(db);
    r2_sj_index_event(third, "thinking", "diary_entry_linked", "diary");
    assert(r2_sj_link_diary_event(third, 103) == 0);
    expect_link("Exact new diary entry", "Diary link: 103");

    /* A unique legacy text match remains supported for older databases. */
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    sql(db, "INSERT INTO diary_entries(id,entry) VALUES(104,'unique old prose');");
    int64_t unique = event(db, "diary_entry_written", "Unique legacy event",
                           "unique old prose");
    sqlite3_close(db);
    r2_sj_index_event(unique, "thinking", "diary_entry_written", "diary");
    expect_link("Unique legacy event", "Diary link: 104");

    /* An interrupted Journal projection is restored from the durable exact
       diary linkage rather than guessed from repeated prose. */
    r2_sj_shutdown();
    assert(sqlite3_open(R2_DIARY_DATABASE, &db) == SQLITE_OK);
    sql(db, "DELETE FROM r2_sensory_journal_diary_links WHERE diary_entry_id=103;");
    sqlite3_close(db);
    assert(r2_sj_init() == 0);
    expect_link("Exact new diary entry", "Diary link: 103");
    expect_link("Ambiguous older entry", "Diary link: none");
    r2_sj_shutdown();
    (void)remove(R2_DIARY_DATABASE);
    puts("Sensory Journal exact diary identity and backward-compatible migration passed.");
    return 0;
}
