#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "AlternateSelf.h"
#include "Log.h"
#include "r2_diary.h"

#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static sqlite3 *as_db;
static pthread_mutex_t as_lock = PTHREAD_MUTEX_INITIALIZER;
static int as_ready;

static int as_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(as_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Alternate-Self Lab] SQLite error: %s\n",
                err ? err : (as_db ? sqlite3_errmsg(as_db) : "no database"));
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int altself_column_exists(const char *column)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(as_db, "PRAGMA table_info(r2_alternate_self_branches)",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && !strcasecmp((const char *)name, column)) {
                found = 1;
                break;
            }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

static int altself_ensure_column(const char *column, const char *definition)
{
    if (altself_column_exists(column)) return SQLITE_OK;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "ALTER TABLE r2_alternate_self_branches ADD COLUMN %s %s;",
                     column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    return sqlite3_exec(as_db, sql, NULL, NULL, NULL);
}

static int altself_restore_missing_log_links(void)
{
    /*
     * Early Alternate-Self schemas gained log_event_id with DEFAULT 0. Those
     * rows retained their hypothetical content but were disconnected from the
     * shared Life Log. Create/reuse a migration event and repair the link before
     * exposing the organ as initialized.
     */
    for (;;) {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(as_db,
            "SELECT b.id,b.name,b.scenario,b.status FROM r2_alternate_self_branches b "
            "WHERE b.log_event_id IS NULL OR b.log_event_id<=0 OR NOT EXISTS "
            "(SELECT 1 FROM r2_log_events e WHERE e.id=b.log_event_id) "
            "ORDER BY b.id LIMIT 1;", -1, &st, NULL);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            fprintf(stderr, "[R2 Alternate-Self Lab] Could not find unlinked legacy branches: %s\n",
                    sqlite3_errmsg(as_db));
            return -1;
        }
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(st);
            return 0;
        }
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "[R2 Alternate-Self Lab] Could not read legacy branch linkage: %s\n",
                    sqlite3_errmsg(as_db));
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_int64 branch_id = sqlite3_column_int64(st, 0);
        const unsigned char *name_text = sqlite3_column_text(st, 1);
        const unsigned char *scenario_text = sqlite3_column_text(st, 2);
        const unsigned char *status_text = sqlite3_column_text(st, 3);
        char name[256], scenario[512], status[64], summary[512], details[1200];
        snprintf(name, sizeof(name), "%s", name_text ? (const char *)name_text : "");
        snprintf(scenario, sizeof(scenario), "%s",
                 scenario_text ? (const char *)scenario_text : "");
        snprintf(status, sizeof(status), "%s",
                 status_text ? (const char *)status_text : "active");
        sqlite3_finalize(st);

        snprintf(summary, sizeof(summary),
                 "Restored Life Log link for legacy Alternate-Self branch #%lld: %.180s",
                 (long long)branch_id, name);
        snprintf(details, sizeof(details),
                 "branch_id=%lld; name=%s; status=%s; scenario=%s; "
                 "migration=restored missing Life Log relationship without changing the hypothetical",
                 (long long)branch_id, name, status, scenario);

        int64_t event_id = -1;
        rc = sqlite3_prepare_v2(as_db,
            "SELECT id FROM r2_log_events WHERE event_type=? AND summary=? "
            "ORDER BY id DESC LIMIT 1;", -1, &st, NULL);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            return -1;
        }
        sqlite3_bind_text(st, 1, "alternate_self_legacy_link_restored",
                          -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, summary, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(st);
        if (rc == SQLITE_ROW)
            event_id = sqlite3_column_int64(st, 0);
        else if (rc != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);

        if (event_id <= 0)
            event_id = r2_log_event(R2_LOG_THINKING,
                                    "alternate_self_legacy_link_restored",
                                    summary, details, "AlternateSelf");
        if (event_id <= 0) {
            fprintf(stderr, "[R2 Alternate-Self Lab] Could not create Life Log link for legacy branch #%lld.\n",
                    (long long)branch_id);
            return -1;
        }

        rc = sqlite3_prepare_v2(as_db,
            "UPDATE r2_alternate_self_branches SET log_event_id=? WHERE id=? "
            "AND (log_event_id IS NULL OR log_event_id<=0 OR NOT EXISTS "
            "(SELECT 1 FROM r2_log_events e WHERE e.id=log_event_id));",
            -1, &st, NULL);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            return -1;
        }
        sqlite3_bind_int64(st, 1, event_id);
        sqlite3_bind_int64(st, 2, branch_id);
        rc = sqlite3_step(st);
        int changed = sqlite3_changes(as_db);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE || changed != 1) {
            fprintf(stderr, "[R2 Alternate-Self Lab] Could not restore branch #%lld Life Log relationship.\n",
                    (long long)branch_id);
            return -1;
        }
    }
}

static int migrate_altself_schema(void)
{
    const char *columns[][2] = {
        {"name", "TEXT NOT NULL DEFAULT ''"},
        {"scenario", "TEXT NOT NULL DEFAULT ''"},
        {"assumptions", "TEXT"},
        {"predicted_outcome", "TEXT"},
        {"conclusion", "TEXT"},
        {"branch_kind", "TEXT"},
        {"imagination_context", "TEXT"},
        {"status", "TEXT NOT NULL DEFAULT 'active'"},
        {"evidence_event_id", "INTEGER"},
        {"log_event_id", "INTEGER NOT NULL DEFAULT 0"},
        {"created_utc", "TEXT NOT NULL DEFAULT ''"},
        {"updated_utc", "TEXT NOT NULL DEFAULT ''"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (altself_ensure_column(columns[i][0], columns[i][1]) != SQLITE_OK) {
            fprintf(stderr, "[R2 Alternate-Self Lab] Could not migrate column %s: %s\n",
                    columns[i][0], sqlite3_errmsg(as_db));
            return -1;
        }
    }
    if (as_exec(
        "UPDATE r2_alternate_self_branches SET created_utc=CURRENT_TIMESTAMP "
        "WHERE created_utc IS NULL OR created_utc='';"
        "UPDATE r2_alternate_self_branches SET updated_utc=CURRENT_TIMESTAMP "
        "WHERE updated_utc IS NULL OR updated_utc='';"
        "UPDATE r2_alternate_self_branches SET branch_kind=CASE "
        "WHEN name='Imagination: scenario' COLLATE BINARY "
        "AND instr(assumptions,'IMAGINATION RECORD — hypothetical only;')=1 "
        "THEN 'imagination' ELSE 'counterfactual' END WHERE branch_kind IS NULL;"
        "CREATE INDEX IF NOT EXISTS r2_alternate_self_status_idx "
        "ON r2_alternate_self_branches(status,id);"
        "CREATE INDEX IF NOT EXISTS r2_alternate_self_log_event_idx "
        "ON r2_alternate_self_branches(log_event_id);") != 0)
        return -1;
    return altself_restore_missing_log_links();
}

int r2_altself_init(void)
{
    pthread_mutex_lock(&as_lock);
    if (as_ready) { pthread_mutex_unlock(&as_lock); return 0; }
    if (sqlite3_open_v2(R2_DIARY_DATABASE, &as_db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            NULL) != SQLITE_OK) {
        if (as_db) sqlite3_close(as_db);
        as_db = NULL;
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    if (sqlite3_busy_timeout(as_db, 5000) != SQLITE_OK ||
        as_exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;") != 0) {
        sqlite3_close(as_db);
        as_db = NULL;
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_alternate_self_branches ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL,"
        " scenario TEXT NOT NULL,"
        " assumptions TEXT,"
        " predicted_outcome TEXT,"
        " conclusion TEXT,"
        " branch_kind TEXT,"
        " imagination_context TEXT,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " evidence_event_id INTEGER,"
        " log_event_id INTEGER NOT NULL,"
        " created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " updated_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " FOREIGN KEY(evidence_event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(log_event_id) REFERENCES r2_log_events(id)"
        ");";
    const char *links_schema =
        "CREATE TABLE IF NOT EXISTS r2_alternate_self_event_links ("
        " branch_id INTEGER NOT NULL,"
        " event_id INTEGER NOT NULL,"
        " relation TEXT NOT NULL,"
        " details TEXT,"
        " created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        " PRIMARY KEY(branch_id,event_id,relation),"
        " FOREIGN KEY(branch_id) REFERENCES r2_alternate_self_branches(id) ON DELETE CASCADE,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");";
    if (as_exec(schema) != 0 || migrate_altself_schema() != 0 ||
        as_exec(links_schema) != 0) {
        sqlite3_close(as_db); as_db = NULL;
        pthread_mutex_unlock(&as_lock); return -1;
    }
    as_ready = 1;
    pthread_mutex_unlock(&as_lock);
    fprintf(stderr, "[R2 Alternate-Self Lab] Initialized.\n");
    return 0;
}

void r2_altself_shutdown(void)
{
    pthread_mutex_lock(&as_lock);
    if (as_db) sqlite3_close(as_db);
    as_db = NULL; as_ready = 0;
    pthread_mutex_unlock(&as_lock);
}

static int64_t create_branch(const char *name, const char *scenario,
                             const char *assumptions, const char *predicted_outcome,
                             const char *conclusion, int64_t evidence_event_id,
                             const char *imagination_context)
{
    if (!name || !*name || !scenario || !*scenario) return -1;
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    if (evidence_event_id > 0) {
        sqlite3_stmt *check = NULL;
        int check_rc = sqlite3_prepare_v2(as_db,
            "SELECT 1 FROM r2_log_events WHERE id=?;", -1, &check, NULL);
        if (check_rc == SQLITE_OK) check_rc = sqlite3_bind_int64(check, 1, evidence_event_id);
        if (check_rc == SQLITE_OK) check_rc = sqlite3_step(check);
        sqlite3_finalize(check);
        if (check_rc != SQLITE_ROW) {
            pthread_mutex_unlock(&as_lock);
            return -1;
        }
    }
    pthread_mutex_unlock(&as_lock);

    int64_t event_id = r2_log_hypothetical(scenario, assumptions,
        predicted_outcome, conclusion, "Alternate-Self Lab; hypothetical only");
    if (event_id < 0) return -1;

    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return -1; }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "INSERT INTO r2_alternate_self_branches"
        "(name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,log_event_id,created_utc,updated_utc,branch_kind,imagination_context)"
        " VALUES(?,?,?,?,?,'active',?,?,CURRENT_TIMESTAMP,CURRENT_TIMESTAMP,?,?);", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, scenario, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = assumptions ? sqlite3_bind_text(st, 3, assumptions, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 3);
    if (rc == SQLITE_OK) rc = predicted_outcome ? sqlite3_bind_text(st, 4, predicted_outcome, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 4);
    if (rc == SQLITE_OK) rc = conclusion ? sqlite3_bind_text(st, 5, conclusion, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 5);
    if (rc == SQLITE_OK) rc = evidence_event_id > 0 ? sqlite3_bind_int64(st, 6, evidence_event_id) : sqlite3_bind_null(st, 6);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 7, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 8,
        imagination_context ? "imagination" : "counterfactual", -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = imagination_context
        ? sqlite3_bind_text64(st, 9, imagination_context, strlen(imagination_context),
                             SQLITE_TRANSIENT, SQLITE_UTF8)
        : sqlite3_bind_null(st, 9);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int64_t id = rc == SQLITE_DONE && sqlite3_changes(as_db) == 1
        ? sqlite3_last_insert_rowid(as_db) : -1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (id > 0 && evidence_event_id > 0)
        r2_log_link(evidence_event_id, event_id, "evidence_for_hypothesis",
                    "Evidence is referenced for comparison; it is not altered by this branch.");
    return id;
}

int64_t r2_altself_create(const char *name, const char *scenario,
                          const char *assumptions, const char *predicted_outcome,
                          const char *conclusion, int64_t evidence_event_id)
{
    return create_branch(name, scenario, assumptions, predicted_outcome,
                         conclusion, evidence_event_id, NULL);
}

int64_t r2_altself_create_imagination(const char *scenario, const char *assumptions,
                                      const char *outcome, const char *context)
{
    if (!context) return -1;
    return create_branch("Imagination: scenario", scenario, assumptions,
                         "Not evaluated yet. This imagined scene is not automatically a prediction.",
                         outcome, 0, context);
}

int r2_altself_is_imagination(int64_t branch_id)
{
    if (branch_id <= 0) return 0;
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "SELECT 1 FROM r2_alternate_self_branches WHERE id=? AND branch_kind='imagination';",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, branch_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}

int64_t r2_altself_import_hypothesis(const char *scenario,
                                    const char *assumptions,
                                    const char *predicted_outcome,
                                    const char *conclusion,
                                    int64_t hypothetical_event_id,
                                    int64_t evidence_event_id)
{
    if (!scenario || !*scenario || hypothetical_event_id <= 0) return -1;
    char name[128];
    snprintf(name, sizeof(name), "R2 hypothesis from event %lld",
             (long long)hypothetical_event_id);
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "SELECT id,log_event_id FROM r2_alternate_self_branches "
        "WHERE scenario=? AND COALESCE(assumptions,'')=COALESCE(?,'') "
        "AND COALESCE(predicted_outcome,'')=COALESCE(?,'') "
        "AND COALESCE(conclusion,'')=COALESCE(?,'') AND status!='discarded' "
        "ORDER BY id LIMIT 1;", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, scenario, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = assumptions ? sqlite3_bind_text(st, 2, assumptions, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 2);
    if (rc == SQLITE_OK) rc = predicted_outcome ? sqlite3_bind_text(st, 3, predicted_outcome, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 3);
    if (rc == SQLITE_OK) rc = conclusion ? sqlite3_bind_text(st, 4, conclusion, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 4);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        int64_t existing = sqlite3_column_int64(st, 0);
        int64_t original_event = sqlite3_column_int64(st, 1);
        sqlite3_finalize(st);
        pthread_mutex_unlock(&as_lock);
        if (original_event > 0 && original_event != hypothetical_event_id)
            r2_log_link(hypothetical_event_id, original_event,
                        "same_hypothesis_as",
                        "The same counterfactual was already retained as a branch; this occurrence remains in the Life Log.");
        return existing;
    }
    sqlite3_finalize(st);
    st = NULL;
    if (rc != SQLITE_DONE) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(as_db,
        "INSERT OR IGNORE INTO r2_alternate_self_branches"
        "(name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,log_event_id,created_utc,updated_utc,branch_kind)"
        " VALUES(?,?,?,?,?,'active',?,?,CURRENT_TIMESTAMP,CURRENT_TIMESTAMP,'counterfactual');", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, scenario, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = assumptions ? sqlite3_bind_text(st, 3, assumptions, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 3);
    if (rc == SQLITE_OK) rc = predicted_outcome ? sqlite3_bind_text(st, 4, predicted_outcome, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 4);
    if (rc == SQLITE_OK) rc = conclusion ? sqlite3_bind_text(st, 5, conclusion, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 5);
    if (rc == SQLITE_OK) rc = evidence_event_id > 0 ? sqlite3_bind_int64(st, 6, evidence_event_id) : sqlite3_bind_null(st, 6);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 7, hypothetical_event_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int64_t id = rc == SQLITE_DONE ? sqlite3_last_insert_rowid(as_db) : -1;
    if (rc == SQLITE_DONE && sqlite3_changes(as_db) == 0) {
        sqlite3_stmt *existing = NULL;
        int existing_rc = sqlite3_prepare_v2(as_db,
            "SELECT id FROM r2_alternate_self_branches WHERE log_event_id=?;",
            -1, &existing, NULL);
        if (existing_rc == SQLITE_OK)
            existing_rc = sqlite3_bind_int64(existing, 1, hypothetical_event_id);
        if (existing_rc == SQLITE_OK) existing_rc = sqlite3_step(existing);
        if (existing_rc == SQLITE_ROW)
            id = sqlite3_column_int64(existing, 0);
        else
            id = -1;
        sqlite3_finalize(existing);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    return id;
}

static int as_set_status(int64_t id, const char *status)
{
    if (id <= 0) return -1;
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return -1; }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "UPDATE r2_alternate_self_branches SET status=?,updated_utc=CURRENT_TIMESTAMP "
        "WHERE id=? AND status!='discarded';", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int changed = rc == SQLITE_DONE ? sqlite3_changes(as_db) : 0;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (changed) {
        char details[256];
        snprintf(details,sizeof(details),"Branch ID: %lld; new status: %s. The branch remains hypothetical.",
                 (long long)id,status);
        r2_log_event(R2_LOG_HYPOTHETICAL,"alternate_self_branch_status",
                     "Alternate-Self branch status changed.",details,"AlternateSelf.c");
        return 0;
    }
    return -1;
}
int r2_altself_link_event(int64_t branch_id, int64_t event_id,
                          const char *relation, const char *details)
{
    if (branch_id <= 0 || event_id <= 0 || !relation || !*relation ||
        strlen(relation) > 128 || (details && strlen(details) > 2048))
        return -1;

    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) {
        pthread_mutex_unlock(&as_lock);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(as_db,
        "INSERT OR IGNORE INTO r2_alternate_self_event_links"
        "(branch_id,event_id,relation,details,created_utc) "
        "SELECT ?,?,?,?,CURRENT_TIMESTAMP WHERE "
        "EXISTS(SELECT 1 FROM r2_alternate_self_branches WHERE id=?) AND "
        "EXISTS(SELECT 1 FROM r2_log_events WHERE id=?);",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, branch_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, relation, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = details ? sqlite3_bind_text(st, 4, details, -1, SQLITE_TRANSIENT) : sqlite3_bind_null(st, 4);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 5, branch_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 6, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (st) sqlite3_finalize(st);
    int linked = rc == SQLITE_DONE && sqlite3_changes(as_db) > 0;
    pthread_mutex_unlock(&as_lock);
    return linked ? 0 : -1;
}

int r2_altself_discard(int64_t id) { return as_set_status(id,"discarded"); }
int r2_altself_retain_hypothesis(int64_t id) { return as_set_status(id,"retained_hypothesis"); }

static char *as_query(const char *sql, int64_t first, int64_t second, int limit)
{
    pthread_mutex_lock(&as_lock);
    if (!as_ready || !as_db) { pthread_mutex_unlock(&as_lock); return NULL; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(as_db,sql,-1,&st,NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&as_lock); return NULL;
    }
    int param_count = sqlite3_bind_parameter_count(st);
    int bind_rc = SQLITE_OK;
    if (param_count >= 1) bind_rc = sqlite3_bind_int64(st, 1, first);
    if (bind_rc == SQLITE_OK && param_count >= 2) bind_rc = sqlite3_bind_int64(st, 2, second);
    if (bind_rc == SQLITE_OK && param_count >= 3) bind_rc = sqlite3_bind_int(st, 3, limit);
    if (bind_rc != SQLITE_OK) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&as_lock);
        return NULL;
    }
    size_t capacity = 4096, used = 0;
    char *out = malloc(capacity);
    if (!out) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&as_lock);
        return NULL;
    }
    out[0] = '\0';
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const char *fields[10] = {0};
        const int count = sqlite3_column_count(st);
        for (int i = 1; i < count; ++i) {
            if (i == 7) continue; /* evidence event ID */
            fields[i] = (const char *)sqlite3_column_text(st, i);
            if ((!fields[i] && sqlite3_column_type(st, i) != SQLITE_NULL) ||
                (fields[i] && strlen(fields[i]) != (size_t)sqlite3_column_bytes(st, i)))
                goto failed;
        }
        char *row = NULL;
        int n = asprintf(&row,
            "Branch #%lld [%s] %s\n  Scenario: %s\n  Assumptions: %s\n"
            "  Predicted outcome: %s\n  Tentative conclusion: %s\n"
            "  Evidence event ID: %lld\n  Created UTC: %s\n%s%s%s\n",
            (long long)sqlite3_column_int64(st, 0),
            fields[6] ? fields[6] : "unknown", fields[1] ? fields[1] : "unnamed",
            fields[2] ? fields[2] : "", fields[3] ? fields[3] : "(none)",
            fields[4] ? fields[4] : "(not specified)", fields[5] ? fields[5] : "(not specified)",
            (long long)sqlite3_column_int64(st, 7), fields[8] ? fields[8] : "unknown",
            fields[9] ? "  Archived imagination context (input evidence):\n" : "",
            fields[9] ? fields[9] : "", fields[9] ? "\n" : "");
        if (n < 0) goto failed;
        if ((size_t)n > SIZE_MAX - used - 1) {
            free(row);
            errno = EOVERFLOW;
            goto failed;
        }
        size_t needed = used + (size_t)n + 1;
        if (needed > capacity) {
            size_t next = capacity;
            while (next < needed) next = next > SIZE_MAX / 2 ? needed : next * 2;
            char *grown = realloc(out, next);
            if (!grown) { free(row); goto failed; }
            out = grown;
            capacity = next;
        }
        memcpy(out + used, row, (size_t)n + 1);
        used += (size_t)n;
        free(row);
    }
    if (rc != SQLITE_DONE) goto failed;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    if (!used) strcpy(out, "No Alternate-Self branches found.\n");
    return out;

failed:
    free(out);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&as_lock);
    return NULL;
}

char *r2_altself_list(int limit)
{
    if (limit < 1) limit = 25;
    if (limit > 200) limit = 200;
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc "
                    "FROM r2_alternate_self_branches ORDER BY id DESC LIMIT ?;",limit,0,0);
}
char *r2_altself_show(int64_t id)
{
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc,imagination_context "
                    "FROM r2_alternate_self_branches WHERE id=?;",id,0,0);
}
char *r2_altself_compare(int64_t a, int64_t b)
{
    return as_query("SELECT id,name,scenario,assumptions,predicted_outcome,conclusion,status,evidence_event_id,created_utc "
                    "FROM r2_alternate_self_branches WHERE id IN (?,?) ORDER BY id;",a,b,0);
}
