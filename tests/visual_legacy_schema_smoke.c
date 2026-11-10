/* Include the production implementation so this test exercises its real
   legacy-schema migration and insert helper against SQLite. */
#include "../Visual.c"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

/* These inference/logging dependencies are not exercised by this database
   migration test; keep the test local and independent of an Ollama server. */
int r2_ollama_context_tokens(void) { return 8192; }
int r2_ollama_chat_url(char *out, size_t out_cap)
{
    if (!out || out_cap < sizeof("http://127.0.0.1:11434")) return -1;
    snprintf(out, out_cap, "%s", "http://127.0.0.1:11434");
    return 0;
}
int r2_ollama_vision_request_begin(void) { return 1; }
void r2_ollama_vision_request_end(void) {}
int r2_ollama_vision_request_should_abort(void) { return 0; }
int64_t r2_log_sensory(const char *sense, const char *observation,
                       const char *details, const char *source)
{
    (void)sense; (void)observation; (void)details; (void)source;
    return 1;
}

int r2_log_is_initialized(void) { return 0; }
int64_t r2_log_save_visual_once(const char *key, const char *description, const char *source)
{
    (void)key; (void)description; (void)source;
    return -1;
}

int r2_log_save_shared_sensory(sqlite3 *peer, const char *type, const char *summary,
                              const char *details, const char *source,
                              int (*save)(sqlite3 *, int64_t, void *), void *context)
{
    (void)peer; (void)type; (void)summary; (void)details; (void)source;
    (void)save; (void)context;
    return 1;
}

static int visit_legacy_visual(void *context, int64_t id, const char *row, size_t length)
{
    int *count = context;
    assert(id == 1);
    assert(length == strlen(row));
    assert(strcmp(row, "[2020-01-02T03:04:05Z] source=legacy-camera model=unknown "
                       "image=/legacy/old.jpg\nold frame\n\n") == 0);
    ++*count;
    return 0;
}

int main(void)
{
    char root[] = "/tmp/r2-visual-legacy-smoke-XXXXXX";
    assert(mkdtemp(root));
    char path[PATH_MAX], library[PATH_MAX];
    assert(snprintf(path, sizeof(path), "%s/legacy.db", root) < (int)sizeof(path));
    assert(snprintf(library, sizeof(library), "%s/archive", root) < (int)sizeof(library));
    assert(unsetenv("DISPLAY") == 0);
    sqlite3 *legacy_db = NULL;
    assert(sqlite3_open(path, &legacy_db) == SQLITE_OK);

    assert(sqlite3_exec(legacy_db,
        "CREATE TABLE r2_visual_experiences ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_at TEXT,"
        "timestamp TEXT NOT NULL,"
        "file_path TEXT,"
        "path TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "description TEXT NOT NULL);"
        "INSERT INTO r2_visual_experiences(created_at,timestamp,file_path,path,source,description) "
        "VALUES('2020-01-02T03:04:05Z','2020-01-02T03:04:05Z',"
        "'/legacy/old.jpg','/legacy/old.jpg','legacy-camera','old frame'),"
        "(NULL,'2020-02-03T04:05:06Z','','/legacy/alternate.jpg','another camera','alternate aliases');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(sqlite3_exec(legacy_db, "CREATE TRIGGER deny_backfill "
        "BEFORE UPDATE ON r2_visual_experiences BEGIN SELECT RAISE(ABORT, 'fixture'); END",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(legacy_db) == SQLITE_OK);
    assert(r2_visual_init(path, library) == -1 && !r2_visual_is_initialized());
    assert(sqlite3_open(path, &legacy_db) == SQLITE_OK);
    assert(!visual_column_exists(legacy_db, "created_utc"));
    assert(!visual_column_exists(legacy_db, "life_log_event_id"));
    assert(sqlite3_exec(legacy_db, "DROP TRIGGER deny_backfill", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(legacy_db) == SQLITE_OK);
    assert(r2_visual_init(path, path) == -1 && !r2_visual_is_initialized());
    assert(r2_visual_init(path, library) == 0);
    int legacy_rows = 0;
    assert(r2_visual_visit_search("legacy-camera", 2, visit_legacy_visual, &legacy_rows) == 0);
    assert(legacy_rows == 1);
    char *legacy = r2_visual_search("old frame", 2);
    assert(legacy && strstr(legacy, "image=/legacy/old.jpg\nold frame\n\n"));
    free(legacy);
    legacy = r2_visual_search("alternate aliases", 2);
    assert(legacy && strstr(legacy, "[2020-02-03T04:05:06Z] source=another camera "
                                  "model=unknown image=/legacy/alternate.jpg"));
    free(legacy);

    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(visual_db,
        "SELECT created_utc,image_path FROM r2_visual_experiences WHERE description='old frame'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "2020-01-02T03:04:05Z") == 0);
    assert(strcmp((const char *)sqlite3_column_text(st, 1), "/legacy/old.jpg") == 0);
    sqlite3_finalize(st);

    assert(visual_insert_compat(visual_db, "2026-10-16T07:00:00Z", "Eyes",
        "/new/frame.jpg", "qwen3.5:9b", NULL, "new frame",
        640, 480, 7, 123456, 0, NULL) == SQLITE_DONE);

    assert(sqlite3_prepare_v2(visual_db,
        "SELECT created_utc,image_path,created_at,timestamp,file_path,path "
        "FROM r2_visual_experiences WHERE description='new frame'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    for (int i = 0; i < 6; ++i) {
        const char *expected = (i == 1 || i == 4 || i == 5)
            ? "/new/frame.jpg" : "2026-10-16T07:00:00Z";
        assert(strcmp((const char *)sqlite3_column_text(st, i), expected) == 0);
    }
    sqlite3_finalize(st);
    r2_visual_shutdown();
    assert(unlink(path) == 0);
    assert(rmdir(library) == 0);
    assert(rmdir(root) == 0);
    puts("Legacy visual schema migration preserves old rows and supports new writes.");
    return 0;
}
