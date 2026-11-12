/*
 * ============================================================
 * R2-3PO LIFE LOG
 * ============================================================
 *
 * Log.c maintains a durable chronological record of R2's actual
 * operation, observations, system state, and milestones.
 *
 * This is NOT the private reflective diary:
 *
 *   r2_diary.c  -> first-person reflection and diary writing
 *   Log.c       -> factual event chronology and system history
 *   r2.c        -> existing conversation, memory, and model core
 *
 * The Life Log uses the existing R2 SQLite database but owns a
 * separate connection and creates only r2_log_* tables. It does
 * not alter memories or diary_entries.
 *
 * Build with the existing R2 modules and SQLite:
 *   gcc ... r2.c shell.c r2_diary.c Log.c Ears.c Eyes.c ...
 * ============================================================
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include "Log.h"
#include "SensoryJournal.h"
#include "Addiction.h"
#include "r2.h"
#include "r2_diary.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/types.h>

#ifdef __linux__
#include <sys/sysinfo.h>
#endif

#include <sqlite3.h>

#define R2_LOG_MAX_TEXT       65536
#define R2_LOG_DEFAULT_LIMIT  25
#define R2_LOG_MAX_LIMIT      500

static sqlite3 *log_db = NULL;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static int log_initialized = 0;
static int64_t current_session_id = 0;
static struct timespec process_start_time;
static int process_clock_ready = 0;
static void log_replay_conversation_archives(void);
static int log_link_locked(int64_t from_event_id, int64_t to_event_id,
                           const char *relationship, const char *notes);
static int log_link_context_review_locked(int64_t archive_id, int64_t conversation_id);

/* ------------------------------------------------------------
 * SMALL INTERNAL HELPERS
 * ------------------------------------------------------------ */

static const char *category_name(R2LogCategory category)
{
    switch (category) {
        case R2_LOG_SYSTEM:     return "system";
        case R2_LOG_LIFECYCLE:  return "lifecycle";
        case R2_LOG_SENSORY:    return "sensory";
        case R2_LOG_THINKING:   return "thinking";
        case R2_LOG_FILESYSTEM: return "filesystem";
        case R2_LOG_MEDIA:      return "media";
        case R2_LOG_WORLD:      return "world";
        case R2_LOG_MILESTONE:  return "milestone";
        case R2_LOG_MEMORY:     return "memory";
        case R2_LOG_EXPERIMENT: return "experiment";
        case R2_LOG_ERROR:      return "error";
        case R2_LOG_EMOTION:    return "emotion";
        case R2_LOG_BELIEF:     return "belief";
        case R2_LOG_CONTINUITY: return "continuity";
        case R2_LOG_CONVERSATION:return "conversation";
        case R2_LOG_HYPOTHETICAL:return "hypothetical";
        default:                return "other";
    }
}

static void timestamp_pair(char *utc, size_t utc_size,
                           char *local, size_t local_size)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    struct tm tm_local;

    if (utc && utc_size) {
        if (gmtime_r(&now, &tm_utc))
            strftime(utc, utc_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        else
            snprintf(utc, utc_size, "unavailable");
    }

    if (local && local_size) {
        if (localtime_r(&now, &tm_local))
            strftime(local, local_size, "%Y-%m-%d %H:%M:%S %z",
                     &tm_local);
        else
            snprintf(local, local_size, "unavailable");
    }
}

uint64_t r2_log_elapsed_ms(void)
{
    struct timespec now;

    if (!process_clock_ready)
        return 0;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;

    int64_t seconds = (int64_t)now.tv_sec -
                      (int64_t)process_start_time.tv_sec;
    int64_t nanoseconds = (int64_t)now.tv_nsec -
                          (int64_t)process_start_time.tv_nsec;

    if (nanoseconds < 0) {
        seconds--;
        nanoseconds += 1000000000LL;
    }

    if (seconds < 0)
        return 0;

    return (uint64_t)seconds * 1000ULL +
           (uint64_t)nanoseconds / 1000000ULL;
}

static int exec_sql(const char *sql)
{
    char *error = NULL;
    int rc = sqlite3_exec(log_db, sql, NULL, NULL, &error);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Life Log] SQLite error: %s\n",
                error ? error : sqlite3_errmsg(log_db));
        sqlite3_free(error);
        return -1;
    }

    return 0;
}

/* Call with log_lock held and an owned write transaction. */
static int log_finish_write(int saved)
{
    if (saved && exec_sql("COMMIT;") == 0) return 0;
    if (!sqlite3_get_autocommit(log_db)) (void)exec_sql("ROLLBACK;");
    return -1;
}

static int bind_optional_text(sqlite3_stmt *statement, int index,
                              const char *value)
{
    if (value)
        return sqlite3_bind_text(statement, index, value, -1,
                                 SQLITE_TRANSIENT);

    return sqlite3_bind_null(statement, index);
}

static int valid_text(const char *value)
{
    return value && value[0] != '\0';
}

/* Formatting a prefix of a stored text field would misrepresent the record. */
static int log_read_text_fields(sqlite3_stmt *statement, int first, int count,
                                const char **fields)
{
    for (int i = 0; i < count; ++i) {
        int column = first + i;
        int type = sqlite3_column_type(statement, column);
        const char *value = (const char *)sqlite3_column_text(statement, column);
        if ((!value && type != SQLITE_NULL) ||
            (value && strlen(value) != (size_t)sqlite3_column_bytes(statement, column)))
            return -1;
        fields[i] = value;
    }
    return 0;
}

/* Grow to preserve complete selected query records; never clip their text. */
static int append_text(char **buffer, size_t *length, size_t *capacity,
                       const char *format, ...)
{
    va_list args;
    va_list copy;
    int needed;

    va_start(args, format);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);

    if (needed < 0) {
        va_end(args);
        return -1;
    }

    if (*length == SIZE_MAX || (size_t)needed > SIZE_MAX - *length - 1) {
        va_end(args);
        errno = EOVERFLOW;
        return -1;
    }

    size_t required = *length + (size_t)needed + 1;

    if (required > *capacity) {
        size_t next = *capacity ? *capacity : 4096;

        while (next < required) {
            if (next > SIZE_MAX / 2) {
                next = required;
                break;
            }
            next *= 2;
        }

        char *grown = realloc(*buffer, next);
        if (!grown) {
            va_end(args);
            return -1;
        }

        *buffer = grown;
        *capacity = next;
    }

    vsnprintf(*buffer + *length, *capacity - *length, format, args);
    *length += (size_t)needed;
    va_end(args);
    return 0;
}

static int log_column_exists(const char *table, const char *column)
{
    char sql[256];
    int n = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    if (n < 0 || (size_t)n >= sizeof(sql)) return 0;
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(log_db, sql, -1, &st, NULL) == SQLITE_OK) {
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

static int log_ensure_column(const char *table, const char *column,
                             const char *definition)
{
    if (log_column_exists(table, column)) return SQLITE_OK;
    char sql[512];
    int n = snprintf(sql, sizeof(sql), "ALTER TABLE %s ADD COLUMN %s %s;",
                     table, column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    return sqlite3_exec(log_db, sql, NULL, NULL, NULL);
}

static int migrate_legacy_log_schema(void)
{
    static const char *const columns[][3] = {
        {"r2_log_sessions","started_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_sessions","started_local","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_sessions","ended_utc","TEXT"},
        {"r2_log_sessions","ended_local","TEXT"},
        {"r2_log_sessions","start_monotonic_ms","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_sessions","end_monotonic_ms","INTEGER"},
        {"r2_log_sessions","process_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_sessions","end_reason","TEXT"},

        {"r2_log_events","utc_time","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_events","local_time","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_events","monotonic_ms","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_events","session_id","INTEGER"},
        {"r2_log_events","category","TEXT NOT NULL DEFAULT 'system'"},
        {"r2_log_events","event_type","TEXT NOT NULL DEFAULT 'legacy'"},
        {"r2_log_events","summary","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_events","details","TEXT"},
        {"r2_log_events","source","TEXT"},
        {"r2_log_events","related_event_id","INTEGER"},
        {"r2_log_events","memory_saved","INTEGER NOT NULL DEFAULT 0"},

        {"r2_log_activity_sessions","activity_key","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_activity_sessions","activity_name","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_activity_sessions","started_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_activity_sessions","started_local","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_activity_sessions","started_epoch","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_activity_sessions","start_monotonic_ms","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_activity_sessions","ended_utc","TEXT"},
        {"r2_log_activity_sessions","ended_local","TEXT"},
        {"r2_log_activity_sessions","ended_epoch","INTEGER"},
        {"r2_log_activity_sessions","duration_ms","INTEGER"},
        {"r2_log_activity_sessions","last_verified_state","TEXT"},
        {"r2_log_activity_sessions","stop_reason","TEXT"},
        {"r2_log_activity_sessions","start_event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_activity_sessions","end_event_id","INTEGER"},
        {"r2_log_activity_sessions","status","TEXT NOT NULL DEFAULT 'active'"},

        {"r2_log_location_state","current_location","TEXT NOT NULL DEFAULT 'home'"},
        {"r2_log_location_state","is_home","INTEGER NOT NULL DEFAULT 1"},
        {"r2_log_location_state","was_away","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_location_state","updated_utc","TEXT NOT NULL DEFAULT ''"},

        {"r2_log_milestones","milestone_key","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_milestones","description","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_milestones","first_event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_milestones","recorded_utc","TEXT NOT NULL DEFAULT ''"},

        {"r2_log_conversations","event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_conversations","user_text","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_conversations","assistant_text","TEXT NOT NULL DEFAULT ''"},

        {"r2_log_inner_states","event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_inner_states","state_kind","TEXT NOT NULL DEFAULT 'unknown'"},
        {"r2_log_inner_states","description","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_inner_states","evidence","TEXT"},
        {"r2_log_inner_states","confidence","REAL"},
        {"r2_log_inner_states","origin","TEXT"},

        {"r2_log_beliefs","belief_key","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_beliefs","belief","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_beliefs","evidence","TEXT"},
        {"r2_log_beliefs","confidence","REAL"},
        {"r2_log_beliefs","status","TEXT NOT NULL DEFAULT 'tentative'"},
        {"r2_log_beliefs","origin","TEXT"},
        {"r2_log_beliefs","first_seen_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_beliefs","last_updated_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_beliefs","last_event_id","INTEGER NOT NULL DEFAULT 0"},

        {"r2_log_continuity","item_key","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_continuity","item_type","TEXT NOT NULL DEFAULT 'task'"},
        {"r2_log_continuity","title","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_continuity","description","TEXT"},
        {"r2_log_continuity","status","TEXT NOT NULL DEFAULT 'open'"},
        {"r2_log_continuity","next_action","TEXT"},
        {"r2_log_continuity","origin","TEXT"},
        {"r2_log_continuity","first_seen_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_continuity","last_updated_utc","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_continuity","completed_utc","TEXT"},
        {"r2_log_continuity","last_event_id","INTEGER NOT NULL DEFAULT 0"},

        {"r2_log_links","from_event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_links","to_event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_links","relationship","TEXT NOT NULL DEFAULT 'related'"},
        {"r2_log_links","notes","TEXT"},
        {"r2_log_links","created_utc","TEXT NOT NULL DEFAULT ''"},

        {"r2_log_hypotheticals","event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_hypotheticals","scenario","TEXT NOT NULL DEFAULT ''"},
        {"r2_log_hypotheticals","assumptions","TEXT"},
        {"r2_log_hypotheticals","predicted_outcome","TEXT"},
        {"r2_log_hypotheticals","conclusion","TEXT"},
        {"r2_log_hypotheticals","origin","TEXT"},

        {"r2_log_context_reviews","conversation_event_id","INTEGER"},
        {"r2_log_system_snapshots","event_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_system_snapshots","hostname","TEXT"},
        {"r2_log_system_snapshots","operating_system","TEXT"},
        {"r2_log_system_snapshots","kernel_release","TEXT"},
        {"r2_log_system_snapshots","machine","TEXT"},
        {"r2_log_system_snapshots","process_id","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_system_snapshots","process_elapsed_ms","INTEGER NOT NULL DEFAULT 0"},
        {"r2_log_system_snapshots","system_uptime_seconds","INTEGER"},
        {"r2_log_system_snapshots","total_ram_bytes","INTEGER"},
        {"r2_log_system_snapshots","free_ram_bytes","INTEGER"},
        {"r2_log_system_snapshots","workspace_total_bytes","INTEGER"},
        {"r2_log_system_snapshots","workspace_free_bytes","INTEGER"}
    };

    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (log_ensure_column(columns[i][0], columns[i][1], columns[i][2]) != SQLITE_OK) {
            fprintf(stderr, "[R2 Log] Could not migrate %s.%s: %s\n",
                    columns[i][0], columns[i][1], sqlite3_errmsg(log_db));
            return -1;
        }
    }

    /* Older beta tables used event_id/related_event_id for links. Preserve
       those actual endpoints when upgrading to the current column names. */
    if (log_column_exists("r2_log_links", "event_id") &&
        exec_sql("UPDATE r2_log_links SET from_event_id=event_id "
                 "WHERE from_event_id<=0 AND event_id>0;") != 0)
        return -1;
    if (log_column_exists("r2_log_links", "related_event_id") &&
        exec_sql("UPDATE r2_log_links SET to_event_id=related_event_id "
                 "WHERE to_event_id<=0 AND related_event_id>0;") != 0)
        return -1;

    /* Existing tables are upgraded before indexes are created, so a missing
       column from an older beta cannot abort the entire Life Log startup. */
    return exec_sql(
        "CREATE INDEX IF NOT EXISTS r2_log_events_time_idx ON r2_log_events(id);"
        "CREATE INDEX IF NOT EXISTS r2_log_events_category_idx ON r2_log_events(category,id);"
        "CREATE INDEX IF NOT EXISTS r2_log_activity_key_idx ON r2_log_activity_sessions(activity_key,status);"
        "CREATE INDEX IF NOT EXISTS r2_log_inner_states_kind_idx ON r2_log_inner_states(state_kind,id);"
        "CREATE INDEX IF NOT EXISTS r2_log_continuity_status_idx ON r2_log_continuity(status,item_type);");
}

static int ensure_tables(void)
{
    static const char *schema =
        "PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS r2_log_sessions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " started_utc TEXT NOT NULL,"
        " started_local TEXT NOT NULL,"
        " ended_utc TEXT,"
        " ended_local TEXT,"
        " start_monotonic_ms INTEGER NOT NULL,"
        " end_monotonic_ms INTEGER,"
        " process_id INTEGER NOT NULL,"
        " end_reason TEXT"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_events ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " utc_time TEXT NOT NULL,"
        " local_time TEXT NOT NULL,"
        " monotonic_ms INTEGER NOT NULL,"
        " session_id INTEGER,"
        " category TEXT NOT NULL,"
        " event_type TEXT NOT NULL,"
        " summary TEXT NOT NULL,"
        " details TEXT,"
        " source TEXT,"
        " related_event_id INTEGER,"
        " memory_saved INTEGER NOT NULL DEFAULT 0,"
        " FOREIGN KEY(session_id) REFERENCES r2_log_sessions(id),"
        " FOREIGN KEY(related_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_visual_keys ("
        " archive_key TEXT PRIMARY KEY NOT NULL,"
        " event_id INTEGER NOT NULL UNIQUE REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_activity_sessions ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " activity_key TEXT NOT NULL,"
        " activity_name TEXT NOT NULL,"
        " started_utc TEXT NOT NULL,"
        " started_local TEXT NOT NULL,"
        " started_epoch INTEGER NOT NULL,"
        " start_monotonic_ms INTEGER NOT NULL,"
        " ended_utc TEXT,"
        " ended_local TEXT,"
        " ended_epoch INTEGER,"
        " duration_ms INTEGER,"
        " last_verified_state TEXT,"
        " stop_reason TEXT,"
        " start_event_id INTEGER NOT NULL,"
        " end_event_id INTEGER,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " FOREIGN KEY(start_event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(end_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_location_state ("
        " id INTEGER PRIMARY KEY CHECK(id=1),"
        " current_location TEXT NOT NULL,"
        " is_home INTEGER NOT NULL DEFAULT 1,"
        " was_away INTEGER NOT NULL DEFAULT 0,"
        " updated_utc TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_milestones ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " milestone_key TEXT NOT NULL UNIQUE,"
        " description TEXT NOT NULL,"
        " first_event_id INTEGER NOT NULL,"
        " recorded_utc TEXT NOT NULL,"
        " FOREIGN KEY(first_event_id) REFERENCES r2_log_events(id)"
        ");"

        "CREATE TABLE IF NOT EXISTS r2_log_conversations ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL UNIQUE,"
        " user_text TEXT NOT NULL,"
        " assistant_text TEXT NOT NULL,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_pending_conversations ("
        " event_id INTEGER PRIMARY KEY REFERENCES r2_log_events(id),"
        " user_text TEXT NOT NULL, assistant_text TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_pending_turn_links ("
        " event_id INTEGER NOT NULL REFERENCES r2_log_events(id),"
        " kind INTEGER NOT NULL CHECK(kind IN (0,1)),"
        " target_id INTEGER NOT NULL CHECK(target_id>0),"
        " PRIMARY KEY(event_id,kind,target_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_inner_states ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL,"
        " state_kind TEXT NOT NULL,"
        " description TEXT NOT NULL,"
        " evidence TEXT,"
        " confidence REAL,"
        " origin TEXT,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_beliefs ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " belief_key TEXT NOT NULL UNIQUE,"
        " belief TEXT NOT NULL,"
        " evidence TEXT,"
        " confidence REAL,"
        " status TEXT NOT NULL,"
        " origin TEXT,"
        " first_seen_utc TEXT NOT NULL,"
        " last_updated_utc TEXT NOT NULL,"
        " last_event_id INTEGER NOT NULL,"
        " FOREIGN KEY(last_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_continuity ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " item_key TEXT NOT NULL UNIQUE,"
        " item_type TEXT NOT NULL,"
        " title TEXT NOT NULL,"
        " description TEXT,"
        " status TEXT NOT NULL,"
        " next_action TEXT,"
        " origin TEXT,"
        " first_seen_utc TEXT NOT NULL,"
        " last_updated_utc TEXT NOT NULL,"
        " completed_utc TEXT,"
        " last_event_id INTEGER NOT NULL,"
        " FOREIGN KEY(last_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_links ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " from_event_id INTEGER NOT NULL,"
        " to_event_id INTEGER NOT NULL,"
        " relationship TEXT NOT NULL,"
        " notes TEXT,"
        " created_utc TEXT NOT NULL,"
        " UNIQUE(from_event_id, to_event_id, relationship),"
        " FOREIGN KEY(from_event_id) REFERENCES r2_log_events(id),"
        " FOREIGN KEY(to_event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_sensory_journal_reflection_links ("
        " event_id INTEGER PRIMARY KEY REFERENCES r2_log_events(id),"
        " diary_entry_id INTEGER NOT NULL REFERENCES diary_entries(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_hypotheticals ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL UNIQUE,"
        " scenario TEXT NOT NULL,"
        " assumptions TEXT,"
        " predicted_outcome TEXT,"
        " conclusion TEXT,"
        " origin TEXT,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_context_reviews ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " request TEXT NOT NULL,"
        " original_context TEXT NOT NULL,"
        " review_notes TEXT NOT NULL,"
        " conversation_event_id INTEGER,"
        " created_utc TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP"
        ");"
        "CREATE TABLE IF NOT EXISTS r2_log_system_snapshots ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " event_id INTEGER NOT NULL,"
        " hostname TEXT,"
        " operating_system TEXT,"
        " kernel_release TEXT,"
        " machine TEXT,"
        " process_id INTEGER NOT NULL,"
        " process_elapsed_ms INTEGER NOT NULL,"
        " system_uptime_seconds INTEGER,"
        " total_ram_bytes INTEGER,"
        " free_ram_bytes INTEGER,"
        " workspace_total_bytes INTEGER,"
        " workspace_free_bytes INTEGER,"
        " FOREIGN KEY(event_id) REFERENCES r2_log_events(id)"
        ");";

    if (exec_sql(schema) != 0)
        return -1;

    return migrate_legacy_log_schema();
}

typedef struct R2LegacyLogLinkBackfill {
    const char *table;
    const char *link_column;
    const char *summary_expression;
    const char *details_expression;
} R2LegacyLogLinkBackfill;

/* SQLite text can contain NUL bytes. Encode such a legacy value losslessly
 * so the recovered event remains usable through the text recall interface. */
static char *log_legacy_text(sqlite3_stmt *st, int column)
{
    const unsigned char *value = sqlite3_column_text(st, column);
    if (!value) return NULL; /* These queries use COALESCE. */
    size_t length = (size_t)sqlite3_column_bytes(st, column);
    if (!memchr(value, '\0', length)) return strdup((const char *)value);
    static const char prefix[] = "[hex bytes] ";
    if (length > (SIZE_MAX - sizeof(prefix)) / 2) return NULL;
    char *encoded = malloc(sizeof(prefix) + length * 2);
    if (!encoded) return NULL;
    memcpy(encoded, prefix, sizeof(prefix) - 1);
    char *out = encoded + sizeof(prefix) - 1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        *out++ = hex[value[i] >> 4];
        *out++ = hex[value[i] & 15];
    }
    *out = '\0';
    return encoded;
}

static int log_backfill_one_legacy_link(const R2LegacyLogLinkBackfill *spec)
{
    if (!spec || !spec->table || !spec->link_column ||
        !spec->summary_expression || !spec->details_expression)
        return -1;

    char select_sql[4096];
    int n = snprintf(select_sql, sizeof(select_sql),
        "SELECT t.id, COALESCE((%s),''), COALESCE((%s),'') "
        "FROM %s AS t WHERE t.%s IS NULL OR t.%s<=0 OR NOT EXISTS "
        "(SELECT 1 FROM r2_log_events e WHERE e.id=t.%s) "
        "ORDER BY t.id LIMIT 1;",
        spec->summary_expression, spec->details_expression,
        spec->table, spec->link_column, spec->link_column, spec->link_column);
    if (n < 0 || (size_t)n >= sizeof(select_sql))
        return -1;

    for (;;) {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(log_db, select_sql, -1, &st, NULL);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            fprintf(stderr, "[R2 Log] Could not inspect legacy link %s.%s: %s\n",
                    spec->table, spec->link_column, sqlite3_errmsg(log_db));
            return -1;
        }
        rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(st);
            return 0;
        }
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "[R2 Log] Could not read legacy link %s.%s: %s\n",
                    spec->table, spec->link_column, sqlite3_errmsg(log_db));
            sqlite3_finalize(st);
            return -1;
        }

        sqlite3_int64 row_id = sqlite3_column_int64(st, 0);
        char *legacy_summary = log_legacy_text(st, 1);
        char *legacy_details = log_legacy_text(st, 2);
        sqlite3_finalize(st);
        st = NULL;

        char source[256], summary[512];
        snprintf(source, sizeof(source), "LogMigration:%s:%lld",
                 spec->table, (long long)row_id);
        snprintf(summary, sizeof(summary), "Recovered legacy %s record #%lld",
                 spec->table, (long long)row_id);

        int64_t event_id = -1;
        rc = sqlite3_prepare_v2(log_db,
            "SELECT id FROM r2_log_events WHERE event_type=? AND source=? "
            "ORDER BY id DESC LIMIT 1;", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, "legacy_log_link_restored", -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, source, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) event_id = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        st = NULL;

        char *details = NULL;
        if (rc == SQLITE_DONE && legacy_summary && legacy_details &&
            asprintf(&details, "table=%s; legacy_id=%lld; legacy_summary=%s; legacy_details=%s",
                     spec->table, (long long)row_id, legacy_summary, legacy_details) >= 0)
            event_id = r2_log_event(R2_LOG_THINKING, "legacy_log_link_restored",
                                    summary, details, source);
        free(details);
        free(legacy_summary);
        free(legacy_details);
        if (event_id <= 0) {
            fprintf(stderr, "[R2 Log] Could not create a Life Log link for %s row %lld.\n",
                    spec->table, (long long)row_id);
            return -1;
        }

        char update_sql[1024];
        n = snprintf(update_sql, sizeof(update_sql),
            "UPDATE %s SET %s=? WHERE id=? AND "
            "(%s IS NULL OR %s<=0 OR NOT EXISTS "
            "(SELECT 1 FROM r2_log_events e WHERE e.id=%s));",
            spec->table, spec->link_column, spec->link_column,
            spec->link_column, spec->link_column);
        if (n < 0 || (size_t)n >= sizeof(update_sql))
            return -1;
        rc = sqlite3_prepare_v2(log_db, update_sql, -1, &st, NULL);
        if (rc != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            return -1;
        }
        rc = sqlite3_bind_int64(st, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, row_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        int changed = sqlite3_changes(log_db);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE || changed != 1) {
            fprintf(stderr, "[R2 Log] Could not repair Life Log relationship for %s row %lld.\n",
                    spec->table, (long long)row_id);
            return -1;
        }
    }
}

static int log_validate_required_event_links(void)
{
    static const char *const queries[] = {
        "SELECT COUNT(*) FROM r2_log_activity_sessions s LEFT JOIN r2_log_events e ON e.id=s.start_event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_milestones m LEFT JOIN r2_log_events e ON e.id=m.first_event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_conversations c LEFT JOIN r2_log_events e ON e.id=c.event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_inner_states s LEFT JOIN r2_log_events e ON e.id=s.event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_beliefs b LEFT JOIN r2_log_events e ON e.id=b.last_event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_continuity c LEFT JOIN r2_log_events e ON e.id=c.last_event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_hypotheticals h LEFT JOIN r2_log_events e ON e.id=h.event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_system_snapshots s LEFT JOIN r2_log_events e ON e.id=s.event_id WHERE e.id IS NULL;",
        "SELECT COUNT(*) FROM r2_log_links l LEFT JOIN r2_log_events f ON f.id=l.from_event_id LEFT JOIN r2_log_events t ON t.id=l.to_event_id WHERE f.id IS NULL OR t.id IS NULL;"
    };
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(log_db, queries[i], -1, &st, NULL) != SQLITE_OK) {
            if (st) sqlite3_finalize(st);
            return -1;
        }
        int rc = sqlite3_step(st);
        sqlite3_int64 broken = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
        sqlite3_finalize(st);
        if (broken != 0) {
            fprintf(stderr, "[R2 Log] Required event relationships remain broken (check %zu: %lld).\n",
                    i + 1, (long long)broken);
            return -1;
        }
    }
    return 0;
}

static int log_restore_missing_event_links(void)
{
    static const R2LegacyLogLinkBackfill backfills[] = {
        {"r2_log_activity_sessions", "start_event_id",
         "COALESCE(t.activity_name,'Legacy activity session')",
         "'activity_key='||COALESCE(t.activity_key,'')||'; started_utc='||COALESCE(t.started_utc,'')||'; status='||COALESCE(t.status,'')"},
        {"r2_log_milestones", "first_event_id",
         "COALESCE(t.description,t.milestone_key,'Legacy milestone')",
         "'milestone_key='||COALESCE(t.milestone_key,'')"},
        {"r2_log_conversations", "event_id",
         "COALESCE(t.user_text,'Legacy conversation')",
         "'assistant_text='||COALESCE(t.assistant_text,'')"},
        {"r2_log_inner_states", "event_id",
         "COALESCE(t.description,t.state_kind,'Legacy inner state')",
         "'state_kind='||COALESCE(t.state_kind,'')||'; evidence='||COALESCE(t.evidence,'')||'; confidence='||COALESCE(CAST(t.confidence AS TEXT),'')||'; origin='||COALESCE(t.origin,'')"},
        {"r2_log_beliefs", "last_event_id",
         "COALESCE(t.belief,t.belief_key,'Legacy belief')",
         "'belief_key='||COALESCE(t.belief_key,'')||'; evidence='||COALESCE(t.evidence,'')||'; status='||COALESCE(t.status,'')||'; origin='||COALESCE(t.origin,'')"},
        {"r2_log_continuity", "last_event_id",
         "COALESCE(t.title,t.item_key,'Legacy continuity item')",
         "'item_key='||COALESCE(t.item_key,'')||'; item_type='||COALESCE(t.item_type,'')||'; description='||COALESCE(t.description,'')||'; status='||COALESCE(t.status,'')||'; next_action='||COALESCE(t.next_action,'')"},
        {"r2_log_hypotheticals", "event_id",
         "COALESCE(t.scenario,'Legacy hypothetical')",
         "'assumptions='||COALESCE(t.assumptions,'')||'; predicted_outcome='||COALESCE(t.predicted_outcome,'')||'; conclusion='||COALESCE(t.conclusion,'')||'; origin='||COALESCE(t.origin,'')"},
        {"r2_log_system_snapshots", "event_id",
         "COALESCE(t.hostname,'')||' '||COALESCE(t.operating_system,'')",
         "'kernel='||COALESCE(t.kernel_release,'')||'; machine='||COALESCE(t.machine,'')||'; pid='||COALESCE(CAST(t.process_id AS TEXT),'')||'; elapsed_ms='||COALESCE(CAST(t.process_elapsed_ms AS TEXT),'')"}
    };
    for (size_t i = 0; i < sizeof(backfills) / sizeof(backfills[0]); ++i) {
        if (log_backfill_one_legacy_link(&backfills[i]) != 0)
            return -1;
    }
    return log_validate_required_event_links();
}

/* ------------------------------------------------------------
 * INITIALIZATION / SHUTDOWN
 * ------------------------------------------------------------ */

int r2_log_init(void)
{
    int rc;

    pthread_mutex_lock(&log_lock);

    if (log_initialized) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &process_start_time) == 0)
        process_clock_ready = 1;

    rc = sqlite3_open_v2(
        R2_DIARY_DATABASE,
        &log_db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
        SQLITE_OPEN_FULLMUTEX,
        NULL
    );

    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Life Log] Cannot open %s: %s\n",
                R2_DIARY_DATABASE,
                log_db ? sqlite3_errmsg(log_db) : "unknown error");

        if (log_db)
            sqlite3_close(log_db);

        log_db = NULL;
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    sqlite3_busy_timeout(log_db, 5000);
    sqlite3_exec(log_db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", NULL, NULL, NULL);

    if (ensure_tables() != 0) {
        sqlite3_close(log_db);
        log_db = NULL;
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    /* Quiet initial baseline: startup at home is not a return-home event. */
    sqlite3_exec(log_db,
        "INSERT OR IGNORE INTO r2_log_location_state "
        "(id,current_location,is_home,was_away,updated_utc) "
        "VALUES (1,'home',1,0,strftime('%Y-%m-%dT%H:%M:%SZ','now'));",
        NULL, NULL, NULL);

    log_initialized = 1;
    pthread_mutex_unlock(&log_lock);

    if (log_restore_missing_event_links() != 0) {
        fprintf(stderr, "[R2 Life Log] Required legacy event links could not be restored; refusing to initialize with disconnected records.\n");
        r2_log_shutdown();
        return -1;
    }
    log_replay_conversation_archives();

    if (r2_log_session_start() != 0) {
        fprintf(stderr,
                "[R2 Life Log] Warning: database initialized, "
                "but session start could not be recorded.\n");
    }

    r2_log_event(
        R2_LOG_LIFECYCLE,
        "life_log_initialized",
        "R2 Life Log initialized.",
        "The chronological event log is available.",
        "Log.c"
    );

    r2_log_milestone(
        "life_log_first_initialized",
        "R2 Life Log was initialized for the first time.",
        "First initialization is recorded once and is not replaced "
        "by later starts."
    );

    fprintf(stderr, "[R2 Life Log] Initialized: %s\n",
            R2_DIARY_DATABASE);
    return 0;
}

void r2_log_shutdown(void)
{
    sqlite3 *to_close = NULL;

    if (!r2_log_is_initialized())
        return;

    r2_log_session_end("normal_shutdown");

    pthread_mutex_lock(&log_lock);

    if (log_db) {
        sqlite3_wal_checkpoint_v2(log_db, NULL,
                                  SQLITE_CHECKPOINT_PASSIVE,
                                  NULL, NULL);
        to_close = log_db;
        log_db = NULL;
    }

    log_initialized = 0;
    current_session_id = 0;

    pthread_mutex_unlock(&log_lock);

    if (to_close)
        sqlite3_close(to_close);

    fprintf(stderr, "[R2 Life Log] Shutdown.\n");
}

int r2_log_is_initialized(void)
{
    int result;

    pthread_mutex_lock(&log_lock);
    result = log_initialized && log_db != NULL;
    pthread_mutex_unlock(&log_lock);

    return result;
}

/* ------------------------------------------------------------
 * EVENT RECORDING
 * ------------------------------------------------------------ */

static int64_t log_insert_event_locked(R2LogCategory category,
                                       const char *event_type, const char *summary,
                                       const char *details, const char *source)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int64_t inserted_id = -1;
    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    int rc = sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_events "
        "(utc_time, local_time, monotonic_ms, session_id, category, "
        "event_type, summary, details, source) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(statement, 3, (sqlite3_int64)r2_log_elapsed_ms());
        if (rc == SQLITE_OK) {
            if (current_session_id > 0)
                rc = sqlite3_bind_int64(statement, 4, current_session_id);
            else
                rc = sqlite3_bind_null(statement, 4);
        }
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(statement, 5, category_name(category), -1, SQLITE_STATIC);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 6, event_type, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 7, summary, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(statement, 8, details);
        if (rc == SQLITE_OK) rc = bind_optional_text(statement, 9, source);
        if (rc == SQLITE_OK) rc = sqlite3_step(statement);

        if (rc == SQLITE_DONE && sqlite3_changes(log_db) == 1)
            inserted_id = sqlite3_last_insert_rowid(log_db);
        else if (rc == SQLITE_DONE)
            fprintf(stderr, "[R2 Life Log] Event insert did not create a row.\n");
        else
            fprintf(stderr, "[R2 Life Log] Insert/bind failed: %s (SQLite code %d)\n",
                    sqlite3_errmsg(log_db), rc);
    } else {
        fprintf(stderr, "[R2 Life Log] Prepare failed: %s\n",
                sqlite3_errmsg(log_db));
    }

    sqlite3_finalize(statement);
    return inserted_id;
}

int64_t r2_log_event(R2LogCategory category, const char *event_type,
                     const char *summary, const char *details, const char *source)
{
    if (!valid_text(event_type) || !valid_text(summary))
        return -1;
    if (strlen(event_type) > 128 || strlen(summary) > 8192) {
        errno = E2BIG;
        return -1;
    }

    pthread_mutex_lock(&log_lock);
    int64_t inserted_id = log_initialized && log_db
        ? log_insert_event_locked(category, event_type, summary, details, source) : -1;
    pthread_mutex_unlock(&log_lock);

    /* Index original Life Log events without duplicating their text. */
    if (inserted_id > 0) {
        const char *journal_category = NULL;
        switch (category) {
            case R2_LOG_SENSORY: journal_category = "sensory"; break;
            case R2_LOG_THINKING: journal_category = "thinking"; break;
            case R2_LOG_CONVERSATION: journal_category = "conversation"; break;
            default: break;
        }
        if (journal_category)
            r2_sj_index_event(inserted_id, journal_category, event_type, source);
    }
    return inserted_id;
}

int r2_log_save_shared_sensory(sqlite3 *peer, const char *event_type, const char *summary,
                                const char *details, const char *source,
                                int (*save)(sqlite3 *, int64_t, void *), void *context)
{
    if (!peer || !save || !valid_text(event_type) || !valid_text(summary) ||
        strlen(event_type) > 128 || strlen(summary) > 8192)
        return -1;

    pthread_mutex_lock(&log_lock);
    const char *life_path = log_initialized && log_db ? sqlite3_db_filename(log_db, "main") : NULL;
    const char *peer_path = sqlite3_db_filename(peer, "main");
    if (!life_path || !*life_path || !peer_path || strcmp(life_path, peer_path) != 0) {
        pthread_mutex_unlock(&log_lock);
        return 1;
    }

    int rc = sqlite3_exec(log_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Life Log] Could not start shared sensory write: %s\n", sqlite3_errmsg(log_db));
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    int64_t event_id = log_insert_event_locked(R2_LOG_SENSORY, event_type, summary, details, source);
    int saved = event_id > 0 && save(log_db, event_id, context) == SQLITE_DONE;
    if (saved)
        saved = sqlite3_exec(log_db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!saved) {
        const char *reason = sqlite3_errcode(log_db) == SQLITE_OK
            ? "a required row was rejected or cancelled" : sqlite3_errmsg(log_db);
        fprintf(stderr, "[R2 Life Log] Shared sensory write failed: %s\n", reason);
        if (!sqlite3_get_autocommit(log_db))
            (void)sqlite3_exec(log_db, "ROLLBACK", NULL, NULL, NULL);
    }
    pthread_mutex_unlock(&log_lock);
    if (!saved) return -1;

    /* Journal indexing runs after commit. Journal startup can rebuild it. */
    r2_sj_index_event(event_id, "sensory", event_type, source);
    return 0;
}

int64_t r2_log_save_visual_once(const char *archive_key, const char *description, const char *source)
{
    const char *type = "visual_experience";
    const char *summary = "R2's vision model interpreted a real captured frame.";
    if (!valid_text(archive_key) || !valid_text(description) || !valid_text(source))
        return -1;

    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db ||
        sqlite3_exec(log_db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "SELECT e.id FROM r2_log_visual_keys k JOIN r2_log_events e ON e.id=k.event_id "
        "WHERE k.archive_key=? AND e.category='sensory' AND e.event_type='visual_experience' "
        "AND e.details COLLATE BINARY=? AND e.source COLLATE BINARY=?",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = bind_optional_text(st, 1, archive_key);
    if (rc == SQLITE_OK) rc = bind_optional_text(st, 2, description);
    if (rc == SQLITE_OK) rc = bind_optional_text(st, 3, source);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int64_t id = rc == SQLITE_ROW ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);

    if (rc == SQLITE_DONE) {
        id = log_insert_event_locked(R2_LOG_SENSORY, type, summary, description, source);
        if (id > 0) {
            st = NULL;
            rc = sqlite3_prepare_v2(log_db,
                "INSERT INTO r2_log_visual_keys(archive_key,event_id) VALUES(?,?)",
                -1, &st, NULL);
            if (rc == SQLITE_OK) rc = bind_optional_text(st, 1, archive_key);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, id);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            if (rc == SQLITE_DONE && sqlite3_changes(log_db) != 1) rc = SQLITE_CONSTRAINT;
            sqlite3_finalize(st);
        }
    }

    if (id <= 0 || (rc != SQLITE_DONE && rc != SQLITE_ROW) ||
        sqlite3_exec(log_db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        if (!sqlite3_get_autocommit(log_db))
            (void)sqlite3_exec(log_db, "ROLLBACK", NULL, NULL, NULL);
        id = -1;
    }
    pthread_mutex_unlock(&log_lock);
    if (id > 0) r2_sj_index_event(id, "sensory", type, source);
    return id;
}

/* Check a stable source identifier before replaying an outbox event. */
int r2_log_event_exists(const char *event_type, const char *source)
{
    sqlite3_stmt *statement = NULL;
    int exists = 0;
    int rc;
    if (!valid_text(event_type) || !valid_text(source)) return 0;

    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    rc = sqlite3_prepare_v2(log_db,
        "SELECT 1 FROM r2_log_events WHERE event_type=? AND source=? LIMIT 1",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, event_type, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, source, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) exists = sqlite3_step(statement) == SQLITE_ROW;
    if (statement) sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);
    return exists;
}

int64_t r2_log_event_with_memory(
    R2LogCategory category,
    const char *event_type,
    const char *summary,
    const char *details,
    const char *source,
    int save_as_memory
)
{
    int64_t event_id = r2_log_event(
        category, event_type, summary, details, source
    );

    if (event_id < 0 || !save_as_memory)
        return event_id;

    /*
     * Save only a concise index/pointer in ordinary memory.
     * The full factual event remains in r2_log_events.
     * This call happens outside log_lock to avoid lock coupling.
     */
    char memory[2048];
    snprintf(memory, sizeof(memory),
             "Life Log event %" PRId64 ": [%s/%s] %s",
             event_id, category_name(category), event_type, summary);

    if (r2_save_memory(memory, "experience") == 0) {
        sqlite3_stmt *statement = NULL;
        int update_rc = SQLITE_ERROR;
        int updated_rows = 0;

        pthread_mutex_lock(&log_lock);
        if (log_initialized && log_db &&
            sqlite3_prepare_v2(
                log_db,
                "UPDATE r2_log_events SET memory_saved=1 WHERE id=?;",
                -1, &statement, NULL
            ) == SQLITE_OK) {
            if (sqlite3_bind_int64(statement, 1, event_id) == SQLITE_OK) {
                update_rc = sqlite3_step(statement);
                if (update_rc == SQLITE_DONE)
                    updated_rows = sqlite3_changes(log_db);
            } else {
                update_rc = sqlite3_errcode(log_db);
            }
        } else if (log_initialized && log_db) {
            update_rc = sqlite3_errcode(log_db);
        }
        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);

        if (update_rc != SQLITE_DONE || updated_rows != 1) {
            fprintf(stderr,
                    "[R2 Life Log] Memory pointer was saved, but event %lld "
                    "could not be marked memory_saved (sqlite rc=%d, rows=%d).\n",
                    (long long)event_id, update_rc, updated_rows);
        }
    }

    return event_id;
}

/* The searchable event is already committed. Publish and acknowledge the
 * archive in a second transaction so an archive failure cannot lose the turn. */
static int log_publish_conversation_archive_locked(int64_t event_id)
{
    if (exec_sql("BEGIN IMMEDIATE;") != 0) return -1;
    sqlite3_stmt *statement = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_conversations(event_id,user_text,assistant_text) "
        "SELECT p.event_id,p.user_text,p.assistant_text FROM r2_log_pending_conversations p "
        "WHERE p.event_id=? AND NOT EXISTS "
        "(SELECT 1 FROM r2_log_conversations c WHERE c.event_id=p.event_id);",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    statement = NULL;
    if (rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(log_db,
            "DELETE FROM r2_log_pending_conversations WHERE event_id=?1 AND EXISTS "
            "(SELECT 1 FROM r2_log_conversations c WHERE c.event_id=?1 "
            "AND c.user_text COLLATE BINARY=r2_log_pending_conversations.user_text "
            "AND c.assistant_text COLLATE BINARY=r2_log_pending_conversations.assistant_text);",
            -1, &statement, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    }
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(statement);
    return log_finish_write(saved);
}

/* Kind zero identifies a sensory event and kind one a context review.
 * Replay independently from the text archive so either can recover first. */
static int log_publish_turn_links_locked(int64_t event_id)
{
    if (exec_sql("BEGIN IMMEDIATE;") != 0) return -1;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "SELECT p.kind,p.target_id,EXISTS(SELECT 1 FROM r2_log_events e "
        "WHERE e.id=p.target_id AND e.category='sensory' AND e.event_type='speech_transcribed') "
        "FROM r2_log_pending_turn_links p WHERE p.event_id=?;", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, event_id);
    int saved = rc == SQLITE_OK;
    sqlite3_int64 count = 0;
    while (saved && (rc = sqlite3_step(st)) == SQLITE_ROW) {
        ++count;
        int kind = sqlite3_column_int(st, 0);
        int64_t target = sqlite3_column_int64(st, 1);
        if (kind == 0) {
            saved = sqlite3_column_int(st, 2) && log_link_locked(event_id, target,
                "speech_was_transcribed_for_turn",
                "Exact locally recognized audio event; availability alone is not interpretation.") == 0;
        } else {
            saved = kind == 1 && log_link_context_review_locked(target, event_id) == 0;
        }
    }
    sqlite3_finalize(st);
    st = NULL;
    if (saved && rc == SQLITE_DONE) {
        rc = sqlite3_prepare_v2(log_db,
            "DELETE FROM r2_log_pending_turn_links WHERE event_id=?;", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        saved = rc == SQLITE_DONE && sqlite3_changes64(log_db) == count;
        sqlite3_finalize(st);
    } else {
        saved = 0;
    }
    return log_finish_write(saved);
}

static int log_stage_turn_links_locked(int64_t event_id, int kind,
                                       const int64_t *ids, size_t count)
{
    if (!count) return 0;
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_pending_turn_links(event_id,kind,target_id) VALUES(?1,?2,?3) "
        "ON CONFLICT(event_id,kind,target_id) DO NOTHING;", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, kind);
    for (size_t i = 0; rc == SQLITE_OK && i < count; ++i) {
        rc = sqlite3_bind_int64(st, 3, ids[i]);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_DONE && sqlite3_changes(log_db) == 0) {
            /* A duplicate supplied ID is fine. An ignored staging write is not. */
            size_t earlier = 0;
            while (earlier < i && ids[earlier] != ids[i]) ++earlier;
            if (earlier == i) rc = SQLITE_CONSTRAINT;
        }
        if (rc == SQLITE_DONE) rc = sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return rc == SQLITE_OK ? 0 : -1;
}

static void log_replay_conversation_archives(void)
{
    int64_t after = 0;
    pthread_mutex_lock(&log_lock);
    while (log_initialized && log_db) {
        sqlite3_stmt *statement = NULL;
        int rc = sqlite3_prepare_v2(log_db,
            "WITH next_event(id) AS (SELECT MIN(id) FROM ("
            "SELECT MIN(event_id) AS id FROM r2_log_pending_conversations WHERE event_id>?1 "
            "UNION ALL SELECT MIN(event_id) FROM r2_log_pending_turn_links WHERE event_id>?1)) "
            "SELECT id,EXISTS(SELECT 1 FROM r2_log_pending_conversations WHERE event_id=next_event.id),"
            "EXISTS(SELECT 1 FROM r2_log_pending_turn_links WHERE event_id=next_event.id) "
            "FROM next_event WHERE id IS NOT NULL;", -1, &statement, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, after);
        if (rc == SQLITE_OK) rc = sqlite3_step(statement);
        int archive = 0, links = 0;
        if (rc == SQLITE_ROW) {
            after = sqlite3_column_int64(statement, 0);
            archive = sqlite3_column_int(statement, 1);
            links = sqlite3_column_int(statement, 2);
        }
        sqlite3_finalize(statement);
        if (rc != SQLITE_ROW) {
            if (rc != SQLITE_DONE)
                fprintf(stderr, "[R2 Life Log] Could not read pending conversation archives: %s\n",
                        sqlite3_errmsg(log_db));
            break;
        }
        if (archive && log_publish_conversation_archive_locked(after) != 0)
            fprintf(stderr, "[R2 Life Log] Conversation archive remains pending for event #%" PRId64 ".\n", after);
        if (links && log_publish_turn_links_locked(after) != 0)
            fprintf(stderr, "[R2 Life Log] Conversation evidence remains pending for event #%" PRId64 ".\n", after);
    }
    pthread_mutex_unlock(&log_lock);
}

/*
 * Store the exact visible conversation turn in both the searchable
 * chronological event stream and the dedicated conversation table.
 * The dedicated table is for durable full-text continuity; the event
 * details make the same turn discoverable through r2_log_search().
 */
int64_t r2_log_conversation_turn_with_links(const char *user_text, const char *assistant_text,
                                            const int64_t *speech_ids, size_t speech_count,
                                            const int64_t *review_ids, size_t review_count)
{
    if (!valid_text(user_text) || !valid_text(assistant_text) ||
        (speech_count && !speech_ids) || (review_count && !review_ids))
        return -1;

    size_t user_len = strlen(user_text);
    size_t assistant_len = strlen(assistant_text);
    const char user_prefix[] = "USER SAID:\n";
    const char reply_prefix[] = "\nR2 REPLIED:\n";
    size_t overhead = sizeof(user_prefix) - 1 + sizeof(reply_prefix);
    if (user_len > SIZE_MAX - overhead ||
        assistant_len > SIZE_MAX - overhead - user_len) {
        errno = EOVERFLOW;
        return -1;
    }
    size_t details_size = user_len + assistant_len + overhead;
    char *details = malloc(details_size);
    if (!details) return -1;
    memcpy(details, user_prefix, sizeof(user_prefix) - 1);
    memcpy(details + sizeof(user_prefix) - 1, user_text, user_len);
    memcpy(details + sizeof(user_prefix) - 1 + user_len,
           reply_prefix, sizeof(reply_prefix) - 1);
    memcpy(details + sizeof(user_prefix) - 1 + user_len + sizeof(reply_prefix) - 1,
           assistant_text, assistant_len + 1);

    char summary[1024];
    size_t preview = user_len;
    if (preview > 800) preview = 800;
    while (preview && ((unsigned char)user_text[preview] & 0xc0) == 0x80) --preview;
    snprintf(summary, sizeof(summary),
             "Conversation turn: %.*s",
             (int)preview, user_text);
    sqlite3_stmt *statement = NULL;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        free(details);
        return -1;
    }
    int64_t event_id = log_insert_event_locked(R2_LOG_CONVERSATION, "conversation_turn",
                                               summary, details, "r2_talk");
    free(details);
    int rc = event_id <= 0 ? SQLITE_ERROR : sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_pending_conversations "
        "(event_id,user_text,assistant_text) VALUES(?,?,?);",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text64(statement, 2, user_text, (sqlite3_uint64)user_len, SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text64(statement, 3, assistant_text, (sqlite3_uint64)assistant_len, SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(statement);
    saved = saved && log_stage_turn_links_locked(event_id, 0, speech_ids, speech_count) == 0 &&
                    log_stage_turn_links_locked(event_id, 1, review_ids, review_count) == 0;
    int result = log_finish_write(saved);
    if (result == 0 && log_publish_conversation_archive_locked(event_id) != 0)
        fprintf(stderr, "[R2 Life Log] Conversation archive remains pending for event #%" PRId64 ".\n", event_id);
    if (result == 0 && (speech_count || review_count) &&
        log_publish_turn_links_locked(event_id) != 0)
        fprintf(stderr, "[R2 Life Log] Conversation evidence remains pending for event #%" PRId64 ".\n", event_id);
    pthread_mutex_unlock(&log_lock);
    if (result != 0) return -1;
    r2_sj_index_event(event_id, "conversation", "conversation_turn", "r2_talk");
    return event_id;
}

int64_t r2_log_conversation_turn(const char *user_text, const char *assistant_text)
{
    return r2_log_conversation_turn_with_links(user_text, assistant_text, NULL, 0, NULL, 0);
}

/* ------------------------------------------------------------
 * SESSION / FIRSTS
 * ------------------------------------------------------------ */

int r2_log_session_start(void)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int rc;
    int64_t id = -1;

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    rc = sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_sessions "
        "(started_utc, started_local, start_monotonic_ms, process_id) "
        "VALUES (?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(statement, 3, (sqlite3_int64)r2_log_elapsed_ms());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 4, (int)getpid());
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_DONE && sqlite3_changes(log_db) == 1)
        id = sqlite3_last_insert_rowid(log_db);

    sqlite3_finalize(statement);

    if (id > 0)
        current_session_id = id;

    pthread_mutex_unlock(&log_lock);

    if (id > 0) {
        r2_log_event(R2_LOG_LIFECYCLE, "session_started",
                     "R2 process session started.",
                     "A new session record was created.", "Log.c");
        return 0;
    }

    return -1;
}

int r2_log_session_end(const char *reason)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];
    char local[48];
    int rc;
    int64_t session_id;

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db || current_session_id <= 0) {
        pthread_mutex_unlock(&log_lock);
        return 0;
    }

    session_id = current_session_id;

    rc = sqlite3_prepare_v2(
        log_db,
        "UPDATE r2_log_sessions SET ended_utc=?, ended_local=?, "
        "end_monotonic_ms=?, end_reason=? WHERE id=? AND ended_utc IS NULL;",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(statement, 3, (sqlite3_int64)r2_log_elapsed_ms());
    if (rc == SQLITE_OK) rc = bind_optional_text(statement, 4, reason);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 5, session_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_DONE && sqlite3_changes(log_db) != 1) rc = SQLITE_NOTFOUND;

    sqlite3_finalize(statement);
    pthread_mutex_unlock(&log_lock);

    if (rc != SQLITE_DONE)
        return -1;

    /* Keep the active session ID until its final event is written. */
    r2_log_event(R2_LOG_LIFECYCLE, "session_ended",
                 "R2 process session ended.",
                 reason ? reason : "No shutdown reason supplied.",
                 "Log.c");

    pthread_mutex_lock(&log_lock);
    current_session_id = 0;
    pthread_mutex_unlock(&log_lock);

    return 0;
}

int64_t r2_log_milestone(
    const char *milestone_key,
    const char *description,
    const char *details
)
{
    sqlite3_stmt *statement = NULL;
    char utc[40];

    if (!valid_text(milestone_key) || !valid_text(description) || strlen(description) > 8192)
        return -1;

    /*
     * Check before creating the event: repeat boots must not create
     * duplicate "first" milestones.
     */
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    int lookup_rc = sqlite3_prepare_v2(log_db,
        "SELECT 1 FROM r2_log_milestones WHERE milestone_key=?;",
        -1, &statement, NULL);
    if (lookup_rc == SQLITE_OK)
        lookup_rc = sqlite3_bind_text(statement, 1, milestone_key, -1, SQLITE_TRANSIENT);
    if (lookup_rc == SQLITE_OK) lookup_rc = sqlite3_step(statement);
    sqlite3_finalize(statement);
    statement = NULL;
    if (lookup_rc != SQLITE_DONE) {
        (void)log_finish_write(0);
        pthread_mutex_unlock(&log_lock);
        return lookup_rc == SQLITE_ROW ? 0 : -1;
    }

    int64_t event_id = log_insert_event_locked(R2_LOG_MILESTONE, "milestone",
                                               description, details, milestone_key);
    timestamp_pair(utc, sizeof(utc), NULL, 0);
    int rc = event_id <= 0 ? SQLITE_ERROR : sqlite3_prepare_v2(
        log_db,
        "INSERT INTO r2_log_milestones "
        "(milestone_key, description, first_event_id, recorded_utc) "
        "VALUES (?, ?, ?, ?);",
        -1, &statement, NULL
    );

    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, milestone_key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, description, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 3, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, utc, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(statement);
    int result = log_finish_write(saved);
    pthread_mutex_unlock(&log_lock);

    return result == 0 ? event_id : -1;
}

/* ------------------------------------------------------------
 * SPECIALIZED EVENT HELPERS
 * ------------------------------------------------------------ */

int64_t r2_log_sensory(const char *sense, const char *observation,
                      const char *details, const char *source)
{
    if (!valid_text(sense) || !valid_text(observation))
        return -1;

    return r2_log_event(R2_LOG_SENSORY, sense, observation,
                        details, source ? source : "sensory");
}

int64_t r2_log_thinking(const char *thought_kind, const char *summary,
                        const char *details)
{
    if (!valid_text(thought_kind) || !valid_text(summary))
        return -1;

    return r2_log_event(R2_LOG_THINKING, thought_kind, summary,
                        details, "internal_thinking");
}

int64_t r2_log_file_event(const char *action, const char *path,
                          const char *result, const char *details)
{
    if (!valid_text(action) || !valid_text(path) || !valid_text(result))
        return -1;

    char *summary = NULL;
    if (asprintf(&summary, "%s: %s (%s)", action, path, result) < 0) return -1;
    int64_t id = r2_log_event(R2_LOG_FILESYSTEM, action, summary, details, "filesystem");
    free(summary);
    return id;
}

int64_t r2_log_media_event(const char *action, const char *media_type,
                           const char *path_or_identifier,
                           const char *details)
{
    char summary[2048];

    if (!valid_text(action) || !valid_text(media_type) ||
        !valid_text(path_or_identifier))
        return -1;

    snprintf(summary, sizeof(summary), "%s %s: %s",
             action, media_type, path_or_identifier);

    return r2_log_event(R2_LOG_MEDIA, action, summary,
                        details, "media");
}

int64_t r2_log_world_event(const char *object_or_device,
                           const char *event, const char *result,
                           const char *details)
{
    char summary[2048];

    if (!valid_text(object_or_device) || !valid_text(event) ||
        !valid_text(result))
        return -1;

    snprintf(summary, sizeof(summary), "%s: %s — %s",
             object_or_device, event, result);

    return r2_log_event(R2_LOG_WORLD, event, summary,
                        details, "world");
}


/* ------------------------------------------------------------
 * GENERIC ACTIVITY SESSION TRACKER
 * ------------------------------------------------------------ */
int64_t r2_log_activity_start(const char *activity_key,
                              const char *activity_name,
                              const char *details)
{
    sqlite3_stmt *statement = NULL;
    char utc[40], local[48], event_details[4096], summary[1024];
    int rc;
    if (!valid_text(activity_key) || !valid_text(activity_name) ||
        strlen(activity_key) > 256 || strlen(activity_name) > 512 ||
        (details && strlen(details) > 3000)) return -1;

    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "SELECT 1 FROM r2_log_activity_sessions "
        "WHERE activity_key=? AND status='active' LIMIT 1",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, activity_key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) {
        int step_rc = sqlite3_step(statement);
        if (step_rc == SQLITE_ROW)
            rc = SQLITE_CONSTRAINT;
        else if (step_rc == SQLITE_DONE)
            rc = SQLITE_OK;
        else
            rc = step_rc;
    }
    if (statement) sqlite3_finalize(statement);
    statement = NULL;
    if (rc != SQLITE_OK) {
        (void)log_finish_write(0);
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    snprintf(event_details, sizeof(event_details), "activity_key=%s%s%s",
             activity_key, details && *details ? "; " : "",
             details && *details ? details : "");
    snprintf(summary, sizeof(summary), "R2 started activity '%s'.", activity_name);
    int64_t event_id = log_insert_event_locked(R2_LOG_MEDIA, "activity_started",
        summary, event_details, activity_key);

    timestamp_pair(utc, sizeof(utc), local, sizeof(local));
    rc = event_id <= 0 ? SQLITE_ERROR : sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_activity_sessions "
        "(activity_key,activity_name,started_utc,started_local,started_epoch,"
        "start_monotonic_ms,start_event_id,status) VALUES (?,?,?,?,?,?,?,'active')",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, activity_key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, activity_name, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, utc, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, local, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 5, (sqlite3_int64)time(NULL));
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 6, (sqlite3_int64)r2_log_elapsed_ms());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 7, (sqlite3_int64)event_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(statement);
    int result = log_finish_write(saved);
    pthread_mutex_unlock(&log_lock);
    if (result != 0) return -1;

    /* Every explicitly started activity is a voluntary choice signal for the
       shared preference evaluator. Start/end events are not double-counted. */
    (void)r2_addiction_record_choice(activity_name, "activity", 50,
                                     "activity_start", event_details);
    return event_id;
}

int64_t r2_log_activity_end(const char *activity_key,
                            const char *last_verified_state,
                            const char *stop_reason,
                            const char *details)
{
    sqlite3_stmt *statement = NULL;
    char *activity_name = NULL, *start_local = NULL;
    char utc[40], local[48];
    char *event_details = NULL, *summary = NULL;
    const char *state_text = valid_text(last_verified_state) ? last_verified_state : "unknown";
    const char *reason_text = valid_text(stop_reason) ? stop_reason : "unknown";
    sqlite3_int64 started_epoch = 0, started_monotonic = 0, start_session = 0;
    sqlite3_int64 ended_epoch = (sqlite3_int64)time(NULL);
    sqlite3_int64 duration_ms = -1;
    int rc;
    if (!valid_text(activity_key) || strlen(activity_key) > 256 ||
        (last_verified_state && strlen(last_verified_state) > 1024) ||
        (stop_reason && strlen(stop_reason) > 1024) ||
        (details && strlen(details) > 2000)) return -1;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "SELECT a.activity_name,a.started_local,a.started_epoch,a.start_monotonic_ms,e.session_id "
        "FROM r2_log_activity_sessions a LEFT JOIN r2_log_events e ON e.id=a.start_event_id "
        "WHERE a.activity_key=? AND a.status='active' ORDER BY a.id DESC LIMIT 1", -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, activity_key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    if (rc == SQLITE_ROW) {
        const char *fields[2];
        if (log_read_text_fields(statement, 0, 2, fields) == 0) {
            activity_name = strdup(fields[0] ? fields[0] : "unknown activity");
            start_local = strdup(fields[1] ? fields[1] : "unknown");
        }
        started_epoch = sqlite3_column_int64(statement, 2);
        started_monotonic = sqlite3_column_int64(statement, 3);
        start_session = sqlite3_column_int64(statement, 4);
    }
    sqlite3_finalize(statement);
    statement = NULL;

    uint64_t elapsed = r2_log_elapsed_ms();
    if (start_session > 0 && start_session == current_session_id &&
        started_monotonic >= 0 && elapsed >= (uint64_t)started_monotonic) {
        duration_ms = (sqlite3_int64)(elapsed - (uint64_t)started_monotonic);
    } else if (started_epoch > 0 && ended_epoch >= started_epoch &&
               ended_epoch - started_epoch <= INT64_MAX / 1000) {
        duration_ms = (ended_epoch - started_epoch) * 1000;
    }

    int64_t event_id = -1;
    if (activity_name && start_local &&
        asprintf(&summary, "R2 ended activity '%s'; last verified state: %s; stop reason: %s.",
                 activity_name, state_text, reason_text) >= 0 &&
        asprintf(&event_details,
                 "activity_key=%s; activity=%s; started=%s; duration_seconds=%.3f; "
                 "duration_ms=%lld; last_verified_state=%s; stop_reason=%s%s%s",
                 activity_key, activity_name, start_local,
                 duration_ms >= 0 ? duration_ms / 1000.0 : -1.0,
                 (long long)duration_ms, state_text, reason_text,
                 details && *details ? "; " : "", details && *details ? details : "") >= 0) {
        event_id = log_insert_event_locked(R2_LOG_MEDIA, "activity_ended",
                                           summary, event_details, activity_key);
    }
    free(summary);
    free(event_details);
    free(activity_name);
    free(start_local);
    timestamp_pair(utc, sizeof(utc), local, sizeof(local));
    rc = event_id <= 0 ? SQLITE_ERROR : sqlite3_prepare_v2(log_db,
        "UPDATE r2_log_activity_sessions SET ended_utc=?,ended_local=?,ended_epoch=?,"
        "duration_ms=?,last_verified_state=?,stop_reason=?,end_event_id=?,status='ended' "
        "WHERE activity_key=? AND status='active'",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, local, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 3, ended_epoch);
    if (rc == SQLITE_OK) rc = duration_ms >= 0
        ? sqlite3_bind_int64(statement, 4, duration_ms)
        : sqlite3_bind_null(statement, 4);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 5, state_text, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 6, reason_text, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 7, (sqlite3_int64)event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 8, activity_key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) > 0;
    sqlite3_finalize(statement);
    int result = log_finish_write(saved);
    pthread_mutex_unlock(&log_lock);
    return result == 0 ? event_id : -1;
}


/* ------------------------------------------------------------
 * PERSISTENT LOCATION TRANSITIONS
 * ------------------------------------------------------------ */
int r2_log_location_transition(const char *location, int is_home)
{
    sqlite3_stmt *statement = NULL;
    char previous[257] = "home";
    int previous_home = 1, was_away = 0, should_welcome = 0, rc;
    char summary[640], details[1200];
    if (!valid_text(location) || strlen(location) > 256) return -1;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "SELECT current_location,is_home,was_away FROM r2_log_location_state WHERE id=1",
        -1, &statement, NULL);
    const char *old;
    if (rc == SQLITE_OK && sqlite3_step(statement) == SQLITE_ROW &&
        log_read_text_fields(statement, 0, 1, &old) == 0 &&
        (!old || strlen(old) < sizeof(previous))) {
        if (old) strcpy(previous, old);
        previous_home = sqlite3_column_int(statement, 1);
        was_away = sqlite3_column_int(statement, 2);
    } else {
        if (statement) sqlite3_finalize(statement);
        (void)log_finish_write(0);
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    sqlite3_finalize(statement);
    statement = NULL;
    if (!strcasecmp(previous, location) && previous_home == !!is_home) {
        (void)log_finish_write(0);
        pthread_mutex_unlock(&log_lock);
        return 0;
    }
    should_welcome = is_home && !previous_home && was_away;
    rc = sqlite3_prepare_v2(log_db,
        "UPDATE r2_log_location_state SET current_location=?,is_home=?,"
        "was_away=?,updated_utc=strftime('%Y-%m-%dT%H:%M:%SZ','now') WHERE id=1",
        -1, &statement, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 1, location, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 2, is_home ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 3, is_home ? 0 : 1);
    if (rc == SQLITE_OK) rc = sqlite3_step(statement);
    int saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(statement);
    snprintf(summary, sizeof(summary), "R2 moved to %s.", location);
    snprintf(details, sizeof(details),
             "Previous location=%s; destination=%s; destination classified as %s.",
             previous, location, is_home ? "home" : "away");
    if (saved)
        saved = log_insert_event_locked(R2_LOG_WORLD, "location_changed", summary, details,
                                        "world_location") > 0;
    if (saved && should_welcome)
        saved = log_insert_event_locked(R2_LOG_CONTINUITY, "internal_notification",
                           "Private Welcome Home notification recorded for R2.",
                           "Triggered only by a persisted away-to-home transition.",
                           "world_location") > 0;
    int result = log_finish_write(saved);
    pthread_mutex_unlock(&log_lock);
    if (result != 0) return -1;
    if (should_welcome) {
        (void)r2_save_memory(
            "Welcome Home. You have returned home after being away.",
            "private_notification");
    }
    return 0;
}


/* ------------------------------------------------------------
 * STRUCTURED STATE / BELIEF / CONTINUITY / HYPOTHETICAL RECORDS
 * ------------------------------------------------------------ */

/* Build a bounded details string without silently overflowing a fixed buffer. */
static char *format_record_details(const char *format, ...)
{
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || (size_t)needed > R2_LOG_MAX_TEXT) {
        va_end(args);
        errno = E2BIG;
        return NULL;
    }

    char *text = malloc((size_t)needed + 1);
    if (text)
        vsnprintf(text, (size_t)needed + 1, format, args);
    va_end(args);
    return text;
}

static int valid_confidence(double confidence)
{
    return isfinite(confidence) && (confidence < 0.0 || confidence <= 1.0);
}

static int bind_confidence(sqlite3_stmt *statement, int index,
                           double confidence)
{
    return confidence < 0.0
        ? sqlite3_bind_null(statement, index)
        : sqlite3_bind_double(statement, index, confidence);
}

static void log_structured_insert_error(const char *record_type)
{
    fprintf(stderr, "[R2 Life Log] Could not store %s: %s\n",
            record_type, log_db ? sqlite3_errmsg(log_db) : "database unavailable");
}

int64_t r2_log_inner_state(const char *state_kind,
                           const char *description,
                           const char *evidence,
                           double confidence,
                           const char *origin)
{
    if (!valid_text(state_kind) || !valid_text(description) ||
        strlen(state_kind) > 128 || strlen(description) > 8192 ||
        (evidence && strlen(evidence) > R2_LOG_MAX_TEXT) ||
        (origin && strlen(origin) > 512) || !valid_confidence(confidence))
        return -1;

    int64_t event_id = r2_log_event(
        R2_LOG_THINKING, "inner_state_recorded",
        description, evidence, origin);
    if (event_id < 0) return -1;

    sqlite3_stmt *st = NULL;
    int rc;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_inner_states "
        "(event_id,state_kind,description,evidence,confidence,origin) "
        "VALUES(?,?,?,?,?,?);", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_int64(st, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, state_kind, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, description, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 4, evidence);
        if (rc == SQLITE_OK) rc = bind_confidence(st, 5, confidence);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 6, origin);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    if (rc != SQLITE_DONE) log_structured_insert_error("inner state");
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == SQLITE_DONE ? event_id : -1;
}

int64_t r2_log_belief(const char *belief_key,
                      const char *belief,
                      const char *evidence,
                      double confidence,
                      const char *status,
                      const char *origin)
{
    if (!valid_text(belief_key) || !valid_text(belief) ||
        !valid_text(status) || strlen(belief_key) > 512 ||
        strlen(belief) > 8192 || strlen(status) > 128 ||
        (evidence && strlen(evidence) > R2_LOG_MAX_TEXT) ||
        (origin && strlen(origin) > 512) || !valid_confidence(confidence))
        return -1;

    char *details = format_record_details(
        "Status: %s\nEvidence: %s", status, evidence ? evidence : "(none)");
    if (!details) return -1;
    int64_t event_id = r2_log_event(
        R2_LOG_BELIEF, "belief_updated", belief, details, origin);
    free(details);
    if (event_id < 0) return -1;

    char utc[40];
    timestamp_pair(utc, sizeof(utc), NULL, 0);
    sqlite3_stmt *st = NULL;
    int rc;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_beliefs "
        "(belief_key,belief,evidence,confidence,status,origin,first_seen_utc,"
        "last_updated_utc,last_event_id) VALUES(?,?,?,?,?,?,?,?,?) "
        "ON CONFLICT(belief_key) DO UPDATE SET "
        "belief=excluded.belief,evidence=excluded.evidence,"
        "confidence=excluded.confidence,status=excluded.status,"
        "origin=excluded.origin,last_updated_utc=excluded.last_updated_utc,"
        "last_event_id=excluded.last_event_id;",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(st, 1, belief_key, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, belief, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 3, evidence);
        if (rc == SQLITE_OK) rc = bind_confidence(st, 4, confidence);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 5, status, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 6, origin);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 7, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 8, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 9, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    if (rc != SQLITE_DONE) log_structured_insert_error("belief");
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == SQLITE_DONE ? event_id : -1;
}

int64_t r2_log_continuity(const char *item_key,
                          const char *item_type,
                          const char *title,
                          const char *description,
                          const char *status,
                          const char *next_action,
                          const char *origin)
{
    if (!valid_text(item_key) || !valid_text(item_type) ||
        !valid_text(title) || !valid_text(status) ||
        strlen(item_key) > 512 || strlen(item_type) > 128 ||
        strlen(title) > 8192 || strlen(status) > 128 ||
        (description && strlen(description) > R2_LOG_MAX_TEXT) ||
        (next_action && strlen(next_action) > R2_LOG_MAX_TEXT) ||
        (origin && strlen(origin) > 512))
        return -1;

    char *details = format_record_details(
        "Status: %s\nDescription: %s\nNext action: %s",
        status, description ? description : "(none)",
        next_action ? next_action : "(none)");
    if (!details) return -1;
    int64_t event_id = r2_log_event(
        R2_LOG_CONTINUITY, "continuity_updated", title, details, origin);
    free(details);
    if (event_id < 0) return -1;

    char utc[40];
    timestamp_pair(utc, sizeof(utc), NULL, 0);
    sqlite3_stmt *st = NULL;
    int rc;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_continuity "
        "(item_key,item_type,title,description,status,next_action,origin,"
        "first_seen_utc,last_updated_utc,completed_utc,last_event_id) "
        "VALUES(?,?,?,?,?,?,?,?,?,"
        "CASE WHEN lower(?) IN ('completed','complete','done','closed','resolved') "
        "THEN ? ELSE NULL END,?) "
        "ON CONFLICT(item_key) DO UPDATE SET "
        "item_type=excluded.item_type,title=excluded.title,"
        "description=excluded.description,status=excluded.status,"
        "next_action=excluded.next_action,origin=excluded.origin,"
        "last_updated_utc=excluded.last_updated_utc,"
        "completed_utc=CASE "
        "WHEN lower(excluded.status) IN ('completed','complete','done','closed','resolved') "
        "THEN excluded.last_updated_utc "
        "WHEN lower(excluded.status) IN ('open','active','in_progress','pending','todo') "
        "THEN NULL ELSE r2_log_continuity.completed_utc END,"
        "last_event_id=excluded.last_event_id;",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(st, 1, item_key, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, item_type, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 4, description);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 5, status, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 6, next_action);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 7, origin);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 8, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 9, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 10, status, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 11, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 12, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    if (rc != SQLITE_DONE) log_structured_insert_error("continuity item");
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == SQLITE_DONE ? event_id : -1;
}

static int log_link_locked(int64_t from_event_id, int64_t to_event_id,
                           const char *relationship, const char *notes)
{
    char utc[40];
    timestamp_pair(utc, sizeof(utc), NULL, 0);
    sqlite3_stmt *st = NULL;
    int rc;
    rc = sqlite3_prepare_v2(log_db,
        "INSERT OR IGNORE INTO r2_log_links "
        "(from_event_id,to_event_id,relationship,notes,created_utc) "
        "SELECT ?1,?2,?3,?4,?5 "
        "WHERE EXISTS(SELECT 1 FROM r2_log_events WHERE id=?1) "
        "AND EXISTS(SELECT 1 FROM r2_log_events WHERE id=?2) "
        "AND NOT EXISTS(SELECT 1 FROM r2_log_links "
        "WHERE from_event_id=?1 AND to_event_id=?2 AND relationship=?3);",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_int64(st, 1, from_event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, to_event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, relationship, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK)
            rc = notes ? sqlite3_bind_text64(st, 4, notes, strlen(notes),
                                             SQLITE_TRANSIENT, SQLITE_UTF8)
                       : sqlite3_bind_null(st, 4);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 5, utc, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        log_structured_insert_error("event link");
        return -1;
    }

    /* DONE can mean a duplicate, a missing endpoint, or an ignored insert.
     * Accept retries only when the exact relationship actually exists. */
    st = NULL;
    rc = sqlite3_prepare_v2(log_db,
        "SELECT 1 FROM r2_log_links l "
        "JOIN r2_log_events f ON f.id=l.from_event_id "
        "JOIN r2_log_events t ON t.id=l.to_event_id "
        "WHERE l.from_event_id=?1 AND l.to_event_id=?2 AND l.relationship=?3 LIMIT 1;",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, from_event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, to_event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, relationship, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE)
        fprintf(stderr, "[R2 Life Log] Event link has no stored edge for %lld -> %lld.\n",
                (long long)from_event_id, (long long)to_event_id);
    else if (rc != SQLITE_ROW)
        log_structured_insert_error("event link verification");
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 0 : -1;
}

int r2_log_link(int64_t from_event_id, int64_t to_event_id,
                const char *relationship, const char *notes)
{
    if (from_event_id <= 0 || to_event_id <= 0 ||
        !valid_text(relationship) || strlen(relationship) > 128)
        return -1;

    pthread_mutex_lock(&log_lock);
    int rc = log_initialized && log_db
        ? log_link_locked(from_event_id, to_event_id, relationship, notes) : -1;
    pthread_mutex_unlock(&log_lock);
    return rc;
}

int64_t r2_log_complete_reflection(int64_t diary_id)
{
    if (diary_id <= 0)
        return -1;
    const char *type = "reflection_completed";
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db || exec_sql("BEGIN IMMEDIATE;") != 0) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "SELECT d.entry,p.speech_count FROM r2_diary_pending_reflections p "
        "LEFT JOIN diary_entries d ON d.id=p.diary_entry_id WHERE p.diary_entry_id=?;",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, diary_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE) {
        sqlite3_finalize(st);
        (void)exec_sql("ROLLBACK;");
        pthread_mutex_unlock(&log_lock);
        return 0; /* No pending completion to consume. */
    }
    const char *entry = rc == SQLITE_ROW ? (const char *)sqlite3_column_text(st, 0) : NULL;
    sqlite3_int64 expected = rc == SQLITE_ROW ? sqlite3_column_int64(st, 1) : -1;
    int64_t event_id = expected >= 0 && valid_text(entry) &&
        strlen(entry) == (size_t)sqlite3_column_bytes(st, 0)
        ? log_insert_event_locked(R2_LOG_THINKING, type,
                                  "Private diary reflection was written.", entry, "internal_thinking")
        : -1;
    sqlite3_finalize(st);
    st = NULL;

    int saved = event_id > 0;
    if (saved) {
        rc = sqlite3_prepare_v2(log_db,
            "INSERT INTO r2_sensory_journal_reflection_links(event_id,diary_entry_id) VALUES(?,?);",
            -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, diary_id);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        saved = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
        sqlite3_finalize(st);
    }
    if (saved) {
        st = NULL;
        rc = sqlite3_prepare_v2(log_db,
            "SELECT e.id FROM r2_diary_pending_reflection_speech p "
            "LEFT JOIN r2_log_events e ON e.id=p.speech_event_id "
            "AND e.category='sensory' AND e.event_type='speech_transcribed' "
            "WHERE p.diary_entry_id=? ORDER BY p.ordinal;", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, diary_id);
        sqlite3_int64 count = 0;
        if (rc == SQLITE_OK) {
            while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
                if (count == expected) break;
                ++count;
                if (log_link_locked(event_id, sqlite3_column_int64(st, 0),
                        "speech_was_transcribed_for_reflection",
                        "Exact locally recognized audio event; availability alone is not interpretation.") != 0)
                    break;
            }
        }
        saved = rc == SQLITE_DONE && count == expected;
        sqlite3_finalize(st);
    }
    if (saved) {
        const char *deletions[] = {
            "DELETE FROM r2_diary_pending_reflection_speech WHERE diary_entry_id=?;",
            "DELETE FROM r2_diary_pending_reflections WHERE diary_entry_id=?;"
        };
        for (size_t i = 0; saved && i < sizeof(deletions) / sizeof(deletions[0]); ++i) {
            st = NULL;
            rc = sqlite3_prepare_v2(log_db, deletions[i], -1, &st, NULL);
            if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, diary_id);
            if (rc == SQLITE_OK) rc = sqlite3_step(st);
            saved = rc == SQLITE_DONE &&
                sqlite3_changes64(log_db) == (i == 0 ? expected : 1);
            sqlite3_finalize(st);
        }
    }
    if (saved)
        saved = exec_sql("COMMIT;") == 0;
    if (!saved) {
        fprintf(stderr, "[R2 Life Log] Could not confirm reflection completion for diary %lld.\n",
                (long long)diary_id);
        if (!sqlite3_get_autocommit(log_db))
            (void)exec_sql("ROLLBACK;");
    }
    pthread_mutex_unlock(&log_lock);
    if (!saved) return -1;

    r2_sj_index_event(event_id, "thinking", type, "internal_thinking");
    return event_id;
}

int r2_log_replay_reflections(void)
{
    int64_t after = 0;
    int failed = 0;
    for (;;) {
        pthread_mutex_lock(&log_lock);
        sqlite3_stmt *st = NULL;
        int rc = log_initialized && log_db
            ? sqlite3_prepare_v2(log_db,
                "SELECT diary_entry_id FROM r2_diary_pending_reflections "
                "WHERE diary_entry_id>? ORDER BY diary_entry_id LIMIT 1;",
                -1, &st, NULL)
            : SQLITE_ERROR;
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, after);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc == SQLITE_ROW) after = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        pthread_mutex_unlock(&log_lock);
        if (rc == SQLITE_DONE) return failed ? -1 : 0;
        if (rc != SQLITE_ROW) return -1;
        if (r2_log_complete_reflection(after) < 0) failed = 1;
    }
}

int64_t r2_log_hypothetical(const char *scenario,
                            const char *assumptions,
                            const char *predicted_outcome,
                            const char *conclusion,
                            const char *origin)
{
    if (!valid_text(scenario) || strlen(scenario) > 8192 ||
        (assumptions && strlen(assumptions) > R2_LOG_MAX_TEXT) ||
        (predicted_outcome && strlen(predicted_outcome) > R2_LOG_MAX_TEXT) ||
        (conclusion && strlen(conclusion) > R2_LOG_MAX_TEXT) ||
        (origin && strlen(origin) > 512))
        return -1;

    char *details = format_record_details(
        "Assumptions: %s\nPredicted outcome: %s\nConclusion: %s",
        assumptions ? assumptions : "(none)",
        predicted_outcome ? predicted_outcome : "(none)",
        conclusion ? conclusion : "(none)");
    if (!details) return -1;
    int64_t event_id = r2_log_event(
        R2_LOG_HYPOTHETICAL, "hypothetical_recorded", scenario, details, origin);
    free(details);
    if (event_id < 0) return -1;

    sqlite3_stmt *st = NULL;
    int rc;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_hypotheticals "
        "(event_id,scenario,assumptions,predicted_outcome,conclusion,origin) "
        "VALUES(?,?,?,?,?,?);", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_int64(st, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, scenario, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 3, assumptions);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 4, predicted_outcome);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 5, conclusion);
        if (rc == SQLITE_OK) rc = bind_optional_text(st, 6, origin);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    if (rc != SQLITE_DONE) log_structured_insert_error("hypothetical");
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return rc == SQLITE_DONE ? event_id : -1;
}

/* Keep complete processing snapshots outside ordinary event recall so later
 * requests cannot recursively retrieve copies of previous snapshots. */
int64_t r2_log_archive_context_review(const char *request, const char *original,
                                       const char *notes)
{
    if (!request || !original || !notes) return -1;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return -1;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "INSERT INTO r2_log_context_reviews(request,original_context,review_notes) VALUES(?,?,?);",
        -1, &st, NULL);
    const char *fields[] = {request, original, notes};
    for (int i = 0; i < 3 && rc == SQLITE_OK; ++i)
        rc = sqlite3_bind_text64(st, i + 1, fields[i], strlen(fields[i]), SQLITE_TRANSIENT, SQLITE_UTF8);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int64_t id = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1
        ? sqlite3_last_insert_rowid(log_db) : -1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return id;
}

static int log_link_context_review_locked(int64_t archive_id, int64_t conversation_id)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "UPDATE r2_log_context_reviews SET conversation_event_id=?1 WHERE id=?2 "
        "AND (conversation_event_id IS NULL OR conversation_event_id=?1) "
        "AND EXISTS(SELECT 1 FROM r2_log_events WHERE id=?1 AND event_type='conversation_turn');",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, conversation_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, archive_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int linked = rc == SQLITE_DONE && sqlite3_changes(log_db) == 1;
    sqlite3_finalize(st);
    return linked ? 0 : -1;
}

int r2_log_link_context_review(int64_t archive_id, int64_t conversation_id)
{
    if (archive_id <= 0 || conversation_id <= 0) return -1;
    pthread_mutex_lock(&log_lock);
    int rc = log_initialized && log_db ?
        log_link_context_review_locked(archive_id, conversation_id) : -1;
    pthread_mutex_unlock(&log_lock);
    return rc;
}

char *r2_log_read_context_review(int64_t id)
{
    if (id <= 0) return NULL;
    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(log_db,
        "SELECT request,original_context,review_notes,conversation_event_id FROM r2_log_context_reviews WHERE id=?;",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    char *out = NULL;
    if (rc == SQLITE_ROW) {
        const char *fields[3];
        for (int i = 0; i < 3; ++i) {
            fields[i] = (const char *)sqlite3_column_text(st, i);
            if (!fields[i] || strlen(fields[i]) != (size_t)sqlite3_column_bytes(st, i))
                goto done;
        }
        char linked[64] = "pending";
        if (sqlite3_column_type(st, 3) != SQLITE_NULL)
            snprintf(linked, sizeof(linked), "%lld", (long long)sqlite3_column_int64(st, 3));
        if (asprintf(&out,
            "CONTEXT REVIEW SNAPSHOT\nConversation event ID: %s\nRequest:\n%s\n\n"
            "ORIGINAL HISTORICAL CONTEXT:\n%s\n\n"
            "MODEL REVIEW NOTES (interpretations, not new observations):\n%s",
            linked, fields[0], fields[1], fields[2]) < 0)
            out = NULL;
    }
done:
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return out;
}

char *r2_log_status_report(void)
{
    static const char *const tables[] = {
        "r2_log_events", "r2_log_sessions", "r2_log_conversations",
        "r2_log_inner_states", "r2_log_beliefs", "r2_log_continuity",
        "r2_log_links", "r2_log_hypotheticals"
    };
    static const char *const labels[] = {
        "Events", "Sessions", "Conversation turns", "Inner-state records",
        "Belief keys", "Continuity items", "Event links", "Hypothetical records"
    };
    char *output = NULL;
    size_t length = 0, capacity = 0;
    sqlite3_stmt *st = NULL;
    int64_t counts[sizeof(tables) / sizeof(tables[0])] = {0};
    int rc = SQLITE_OK;

    pthread_mutex_lock(&log_lock);
    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) {
        char sql[96];
        snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s;", tables[i]);
        rc = sqlite3_prepare_v2(log_db, sql, -1, &st, NULL);
        if (rc != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW) {
            sqlite3_finalize(st);
            st = NULL;
            pthread_mutex_unlock(&log_lock);
            free(output);
            return NULL;
        }
        counts[i] = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
        st = NULL;
    }

    if (append_text(&output, &length, &capacity,
                    "R2 LIFE LOG STATUS\n"
                    "==================\n"
                    "Current session ID: %" PRId64 "\n",
                    current_session_id) != 0) {
        pthread_mutex_unlock(&log_lock);
        free(output);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(tables) / sizeof(tables[0]); ++i) {
        if (append_text(&output, &length, &capacity,
                        "%s: %" PRId64 "\n", labels[i], counts[i]) != 0) {
            pthread_mutex_unlock(&log_lock);
            free(output);
            return NULL;
        }
    }

    rc = sqlite3_prepare_v2(log_db,
        "SELECT id,utc_time,category,event_type,summary "
        "FROM r2_log_events ORDER BY id DESC LIMIT 1;",
        -1, &st, NULL);
    if (rc == SQLITE_OK)
        rc = sqlite3_step(st);

    if (rc == SQLITE_ROW) {
        const char *fields[4];
        if (log_read_text_fields(st, 1, 4, fields) != 0 ||
            append_text(&output, &length, &capacity,
                        "Latest event: #%" PRId64 " at %s (%s/%s)\n"
                        "  %s\n",
                        sqlite3_column_int64(st, 0),
                        fields[0] ? fields[0] : "unknown time",
                        fields[1] ? fields[1] : "unknown",
                        fields[2] ? fields[2] : "unknown",
                        fields[3] ? fields[3] : "") != 0) {
            sqlite3_finalize(st);
            pthread_mutex_unlock(&log_lock);
            free(output);
            return NULL;
        }
    } else if (rc == SQLITE_DONE) {
        if (append_text(&output, &length, &capacity,
                        "Latest event: none\n") != 0) {
            sqlite3_finalize(st);
            pthread_mutex_unlock(&log_lock);
            free(output);
            return NULL;
        }
    } else {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&log_lock);
        free(output);
        return NULL;
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&log_lock);
    return output;
}

/* ------------------------------------------------------------
 * SYSTEM SELF-AWARENESS SNAPSHOT
 * ------------------------------------------------------------ */

int r2_log_system_snapshot(const char *reason)
{
    struct utsname uts;
    struct statvfs fs;
    char details[8192];
    char summary[512];
    char hostname[256] = "unavailable";
    const char *os_name = "unavailable";
    const char *kernel = "unavailable";
    const char *machine = "unavailable";
    uint64_t total_ram = 0;
    uint64_t free_ram = 0;
    int64_t system_uptime = -1;
    uint64_t disk_total = 0;
    uint64_t disk_free = 0;
    double load_average[3] = {0.0, 0.0, 0.0};
    int have_load_average = 0;
    int have_uts = 0;
    int have_fs = 0;
    int64_t event_id;

    if (uname(&uts) == 0) {
        have_uts = 1;
        os_name = uts.sysname;
        kernel = uts.release;
        machine = uts.machine;
    }

    if (gethostname(hostname, sizeof(hostname) - 1) != 0)
        snprintf(hostname, sizeof(hostname), "unavailable");
    hostname[sizeof(hostname) - 1] = '\0';

#ifdef __linux__
    {
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            system_uptime = (int64_t)info.uptime;
            total_ram = (uint64_t)info.totalram * info.mem_unit;
            free_ram = (uint64_t)info.freeram * info.mem_unit;
        }
    }
#endif

    if (getloadavg(load_average, 3) == 3)
        have_load_average = 1;

    if (statvfs(R2_ROOT, &fs) == 0) {
        have_fs = 1;
        disk_total = (uint64_t)fs.f_blocks * fs.f_frsize;
        disk_free = (uint64_t)fs.f_bavail * fs.f_frsize;
    }

    snprintf(summary, sizeof(summary),
             "R2 system state snapshot%s%s",
             reason && *reason ? ": " : "",
             reason && *reason ? reason : "");

    snprintf(details, sizeof(details),
             "hostname=%s\n"
             "os=%s\n"
             "kernel=%s\n"
             "machine=%s\n"
             "process_id=%ld\n"
             "process_elapsed_ms=%" PRIu64 "\n"
             "system_uptime_seconds=%" PRId64 "\n"
             "total_ram_bytes=%" PRIu64 "\n"
             "free_ram_bytes=%" PRIu64 "\n"
             "load_average_1m=%.2f\n"
             "load_average_5m=%.2f\n"
             "load_average_15m=%.2f\n"
             "load_average_available=%s\n"
             "workspace=%s\n"
             "workspace_disk_total_bytes=%" PRIu64 "\n"
             "workspace_disk_available_bytes=%" PRIu64 "\n"
             "uname_available=%s\n"
             "workspace_statvfs_available=%s\n",
             hostname, os_name, kernel, machine, (long)getpid(),
             r2_log_elapsed_ms(), system_uptime, total_ram, free_ram,
             load_average[0], load_average[1], load_average[2],
             have_load_average ? "yes" : "no",
             R2_ROOT, disk_total, disk_free,
             have_uts ? "yes" : "no", have_fs ? "yes" : "no");

    event_id = r2_log_event(R2_LOG_SYSTEM, "system_snapshot",
                            summary, details, "system_self_awareness");

    if (event_id < 0)
        return -1;

    pthread_mutex_lock(&log_lock);

    if (log_initialized && log_db) {
        sqlite3_stmt *statement = NULL;

        int rc = sqlite3_prepare_v2(
            log_db,
            "INSERT INTO r2_log_system_snapshots "
            "(event_id, hostname, operating_system, kernel_release, machine, "
            "process_id, process_elapsed_ms, system_uptime_seconds, "
            "total_ram_bytes, free_ram_bytes, workspace_total_bytes, "
            "workspace_free_bytes) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);",
            -1, &statement, NULL
        );

        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 1, event_id);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 2, hostname, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 3, os_name, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 4, kernel, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(statement, 5, machine, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 6, (int)getpid());
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(statement, 7, (sqlite3_int64)r2_log_elapsed_ms());
        if (rc == SQLITE_OK) rc = system_uptime >= 0
            ? sqlite3_bind_int64(statement, 8, system_uptime)
            : sqlite3_bind_null(statement, 8);
        if (rc == SQLITE_OK) rc = total_ram
            ? sqlite3_bind_int64(statement, 9, (sqlite3_int64)total_ram)
            : sqlite3_bind_null(statement, 9);
        if (rc == SQLITE_OK) rc = free_ram
            ? sqlite3_bind_int64(statement, 10, (sqlite3_int64)free_ram)
            : sqlite3_bind_null(statement, 10);
        if (rc == SQLITE_OK) rc = have_fs
            ? sqlite3_bind_int64(statement, 11, (sqlite3_int64)disk_total)
            : sqlite3_bind_null(statement, 11);
        if (rc == SQLITE_OK) rc = have_fs
            ? sqlite3_bind_int64(statement, 12, (sqlite3_int64)disk_free)
            : sqlite3_bind_null(statement, 12);
        if (rc == SQLITE_OK) rc = sqlite3_step(statement);

        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);

        return rc == SQLITE_DONE ? 0 : -1;
    }

    pthread_mutex_unlock(&log_lock);
    return -1;
}

/* ------------------------------------------------------------
 * SEARCH / RECENT HISTORY
 * ------------------------------------------------------------ */

static int normalize_limit(int limit)
{
    if (limit <= 0)
        return R2_LOG_DEFAULT_LIMIT;
    if (limit > R2_LOG_MAX_LIMIT)
        return R2_LOG_MAX_LIMIT;
    return limit;
}

static int event_id_seen(const int64_t *ids, size_t count, int64_t id)
{
    for (size_t i = 0; i < count; ++i)
        if (ids[i] == id) return 1;
    return 0;
}

static int log_record_span(R2LogRecordBatch *batch, int64_t id,
                             size_t start, size_t end)
{
    if (!batch) return 0;
    if (end < start || batch->count >= SIZE_MAX / sizeof(*batch->records)) return -1;
    R2LogRecordSpan *grown = realloc(batch->records,
                                    (batch->count + 1) * sizeof(*grown));
    if (!grown) return -1;
    batch->records = grown;
    batch->records[batch->count++] = (R2LogRecordSpan){id, start, end - start};
    return 0;
}

/* Retain the original pair for an archive match, including when a related
 * event has already supplied the record's position in the result. */
static int append_matching_conversation(sqlite3_stmt *archive, int64_t event_id,
                                        const char *details, char **out,
                                        size_t *length, size_t *capacity)
{
    if (!archive) return 0;
    int rc = sqlite3_reset(archive);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(archive, 1, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(archive);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_ROW) return -1;
    const char *fields[2];
    if (log_read_text_fields(archive, 0, 2, fields) != 0 || !fields[0] || !fields[1])
        return -1;
    /* Current turns already put the complete pair in event details. Legacy
       turns may only have a preview, so compare the actual stored bytes. */
    static const char user_prefix[] = "USER SAID:\n";
    static const char reply_prefix[] = "\nR2 REPLIED:\n";
    if (details && strncmp(details, user_prefix, sizeof(user_prefix) - 1) == 0) {
        const char *user = details + sizeof(user_prefix) - 1;
        size_t user_length = strlen(fields[0]);
        if (strncmp(user, fields[0], user_length) == 0) {
            const char *reply = user + user_length;
            if (strncmp(reply, reply_prefix, sizeof(reply_prefix) - 1) == 0 &&
                strcmp(reply + sizeof(reply_prefix) - 1, fields[1]) == 0)
                return 0;
        }
    }
    return append_text(out, length, capacity,
        "  Archived conversation [%" PRId64 "] (historical dialogue):\n"
        "    Original user message: %s\n"
        "    Original R2 response: %s\n",
        event_id, fields[0], fields[1]);
}

static char *query_events(const char *query, int limit, R2LogRecordBatch *batch)
{
    sqlite3_stmt *statement = NULL;
    sqlite3_stmt *link_statement = NULL;
    sqlite3_stmt *archive = NULL;
    char *output = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int64_t seen_ids[512];
    size_t seen_count = 0;
    int linked_total = 0;
    int rc;

    limit = normalize_limit(limit);

    pthread_mutex_lock(&log_lock);

    if (!log_initialized || !log_db) {
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    const char *sql_recent =
        "SELECT id, utc_time, local_time, category, event_type, summary, "
        "details, source, memory_saved FROM r2_log_events "
        "ORDER BY id DESC LIMIT ?;";

    const char *sql_search =
        "SELECT id, utc_time, local_time, category, event_type, summary, "
        "details, source, memory_saved FROM r2_log_events "
        "WHERE summary LIKE ?1 ESCAPE '\\' OR details LIKE ?1 ESCAPE '\\' "
        "OR event_type LIKE ?1 ESCAPE '\\' OR category LIKE ?1 ESCAPE '\\' "
        "OR source LIKE ?1 ESCAPE '\\' "
        "OR EXISTS (SELECT 1 FROM r2_log_conversations c WHERE c.event_id=r2_log_events.id "
        "AND (c.user_text LIKE ?1 ESCAPE '\\' OR c.assistant_text LIKE ?1 ESCAPE '\\')) "
        "ORDER BY id DESC LIMIT ?2;";

    rc = sqlite3_prepare_v2(
        log_db, query ? sql_search : sql_recent, -1, &statement, NULL
    );

    if (rc != SQLITE_OK) {
        sqlite3_finalize(statement);
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    if (query) {
        size_t qlen = strlen(query);
        if (qlen > (SIZE_MAX - 3) / 2) {
            sqlite3_finalize(statement);
            pthread_mutex_unlock(&log_lock);
            errno = E2BIG;
            return NULL;
        }
        char *pattern = malloc(qlen * 2 + 3);

        if (!pattern) {
            sqlite3_finalize(statement);
            pthread_mutex_unlock(&log_lock);
            return NULL;
        }

        size_t p = 0;
        pattern[p++] = '%';

        for (size_t i = 0; i < qlen; ++i) {
            if (query[i] == '%' || query[i] == '_' || query[i] == '\\')
                pattern[p++] = '\\';
            pattern[p++] = query[i];
        }

        pattern[p++] = '%';
        pattern[p] = '\0';

        rc = sqlite3_bind_text(statement, 1, pattern, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(statement, 2, limit);
        if (rc == SQLITE_OK)
            rc = sqlite3_prepare_v2(log_db,
                "SELECT user_text,assistant_text FROM r2_log_conversations "
                "WHERE event_id=?1 AND "
                "(user_text LIKE ?2 ESCAPE '\\' OR assistant_text LIKE ?2 ESCAPE '\\');",
                -1, &archive, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_text(archive, 2, pattern, -1, SQLITE_TRANSIENT);
        free(pattern);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(archive);
            sqlite3_finalize(statement);
            pthread_mutex_unlock(&log_lock);
            return NULL;
        }
    } else {
        rc = sqlite3_bind_int(statement, 1, limit);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(statement);
            pthread_mutex_unlock(&log_lock);
            return NULL;
        }
    }

    if (query) {
        const char *sql_links =
            "SELECT l.relationship, l.notes, e.id, e.category, e.event_type, "
            "e.summary, e.details, e.source "
            "FROM r2_log_links l JOIN r2_log_events e ON e.id = "
            "CASE WHEN l.from_event_id = ? THEN l.to_event_id ELSE l.from_event_id END "
            "WHERE l.from_event_id = ? OR l.to_event_id = ? "
            "ORDER BY l.id DESC LIMIT 4;";
        rc = sqlite3_prepare_v2(log_db, sql_links, -1, &link_statement, NULL);
        if (rc != SQLITE_OK) {
            sqlite3_finalize(statement);
            sqlite3_finalize(link_statement);
            sqlite3_finalize(archive);
            pthread_mutex_unlock(&log_lock);
            return NULL;
        }
    }

    output = malloc(4096);
    if (!output) {
        sqlite3_finalize(statement);
        sqlite3_finalize(link_statement);
        sqlite3_finalize(archive);
        pthread_mutex_unlock(&log_lock);
        return NULL;
    }

    output[0] = '\0';
    capacity = 4096;

    while ((rc = sqlite3_step(statement)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(statement, 0);
        int base_already_seen = query && event_id_seen(seen_ids, seen_count, id);
        if (query && !base_already_seen &&
            seen_count < sizeof(seen_ids) / sizeof(seen_ids[0]))
            seen_ids[seen_count++] = id;
        const char *fields[7];
        if (log_read_text_fields(statement, 1, 7, fields) != 0) break;
        int memory_saved = sqlite3_column_int(statement, 8);

        if (!base_already_seen) {
        size_t record_start = length;
        if (append_text(&output, &length, &capacity,
                        "[%" PRId64 "] %s | %s | %s/%s\n"
                        "  Summary: %s\n"
                        "  Source: %s | Memory indexed: %s\n",
                        id,
                        fields[0] ? fields[0] : "unknown UTC time",
                        fields[1] ? fields[1] : "unknown local time",
                        fields[2] ? fields[2] : "unknown",
                        fields[3] ? fields[3] : "unknown",
                        fields[4] ? fields[4] : "",
                        fields[6] ? fields[6] : "unspecified",
                        memory_saved ? "yes" : "no") != 0) {
            free(output);
            output = NULL;
            break;
        }

        if (fields[5] && *fields[5]) {
            if (append_text(&output, &length, &capacity,
                            "  Details: %s\n", fields[5]) != 0) {
                free(output);
                output = NULL;
                break;
            }
        }

        if (append_matching_conversation(archive, id, fields[5], &output, &length, &capacity) != 0 ||
            append_text(&output, &length, &capacity, "\n") != 0) {
            free(output);
            output = NULL;
            break;
        }

        if (log_record_span(batch, id, record_start, length) != 0) {
            free(output); output = NULL; break;
        }
        }

        if (query && link_statement && linked_total < 8) {
            sqlite3_reset(link_statement);
            sqlite3_clear_bindings(link_statement);
            int link_bind_rc = sqlite3_bind_int64(link_statement, 1, id);
            if (link_bind_rc == SQLITE_OK)
                link_bind_rc = sqlite3_bind_int64(link_statement, 2, id);
            if (link_bind_rc == SQLITE_OK)
                link_bind_rc = sqlite3_bind_int64(link_statement, 3, id);
            if (link_bind_rc != SQLITE_OK) {
                free(output);
                output = NULL;
                break;
            }

            int link_rc = SQLITE_OK;
            int links_for_event = 0;
            while ((link_rc = sqlite3_step(link_statement)) == SQLITE_ROW) {
                int64_t linked_id = sqlite3_column_int64(link_statement, 2);
                if (linked_id == id || event_id_seen(seen_ids, seen_count, linked_id))
                    continue;
                if (linked_total >= 8 || links_for_event >= 2)
                    continue;
                if (seen_count < sizeof(seen_ids) / sizeof(seen_ids[0]))
                    seen_ids[seen_count++] = linked_id;

                const char *edge[2], *linked_fields[5];
                if (log_read_text_fields(link_statement, 0, 2, edge) != 0 ||
                    log_read_text_fields(link_statement, 3, 5, linked_fields) != 0)
                    break;
                size_t record_start = length;

                if (append_text(&output, &length, &capacity,
                                "  Linked event [%" PRId64 "] via %s | %s/%s\n"
                                "    Summary: %s\n"
                                "    Source: %s\n",
                                linked_id,
                                edge[0] ? edge[0] : "related",
                                linked_fields[0] ? linked_fields[0] : "unknown",
                                linked_fields[1] ? linked_fields[1] : "unknown",
                                linked_fields[2] ? linked_fields[2] : "",
                                linked_fields[4] ? linked_fields[4] : "unspecified") != 0) {
                    free(output);
                    output = NULL;
                    break;
                }
                if (edge[1] && *edge[1] &&
                    append_text(&output, &length, &capacity,
                                "    Link notes: %s\n", edge[1]) != 0) {
                    free(output);
                    output = NULL;
                    break;
                }
                if (linked_fields[3] && *linked_fields[3] &&
                    append_text(&output, &length, &capacity,
                                "    Details: %s\n", linked_fields[3]) != 0) {
                    free(output);
                    output = NULL;
                    break;
                }
                if (append_matching_conversation(archive, linked_id, linked_fields[3],
                                                 &output, &length, &capacity) != 0 ||
                    append_text(&output, &length, &capacity, "\n") != 0) {
                    free(output);
                    output = NULL;
                    break;
                }
                if (log_record_span(batch, linked_id, record_start, length) != 0) {
                    free(output); output = NULL; break;
                }
                ++linked_total;
                ++links_for_event;
            }
            if (!output) break;
            if (link_rc != SQLITE_DONE) {
                free(output);
                output = NULL;
                break;
            }
        }
    }

    if (rc != SQLITE_DONE) {
        free(output);
        output = NULL;
    }

    if (output && length == 0) {
        const char *empty = query
            ? "No Life Log events matched that search.\n"
            : "The Life Log contains no events yet.\n";
        snprintf(output, capacity, "%s", empty);
    }
    sqlite3_finalize(statement);
    sqlite3_finalize(link_statement);
    sqlite3_finalize(archive);
    pthread_mutex_unlock(&log_lock);
    return output;
}

char *r2_log_recent(int limit)
{
    return query_events(NULL, limit, NULL);
}

char *r2_log_search(const char *query, int limit)
{
    if (!valid_text(query))
        return r2_log_recent(limit);

    return query_events(query, limit, NULL);
}

int r2_log_search_records(const char *query, int limit, R2LogRecordBatch *batch)
{
    if (!batch) { errno = EINVAL; return -1; }
    memset(batch, 0, sizeof(*batch));
    batch->text = query_events(valid_text(query) ? query : NULL, limit, batch);
    if (!batch->text) { r2_log_release_records(batch); return -1; }
    return 0;
}

void r2_log_release_records(R2LogRecordBatch *batch)
{
    if (!batch) return;
    free(batch->text);
    free(batch->records);
    memset(batch, 0, sizeof(*batch));
}
