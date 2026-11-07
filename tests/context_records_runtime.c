/* Complete diary/visual/learning producers and explicit failure propagation. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

static char *diary_text, *visual_text, *memory_text, *reward_text, *interest_text;
static int failed_provider;
static const char *failed_sql;
static int allowed_rows;
static size_t failed_malloc_size;

void *__real_malloc(size_t);
void *__wrap_malloc(size_t n)
{
    if (failed_malloc_size && n == failed_malloc_size) { errno = ENOMEM; return NULL; }
    return __real_malloc(n);
}
int __real_sqlite3_step(sqlite3_stmt *);
int __wrap_sqlite3_step(sqlite3_stmt *st)
{
    const char *sql = sqlite3_sql(st);
    if (failed_sql && sql && strstr(sql, failed_sql)) {
        if (allowed_rows-- == 0) return SQLITE_IOERR;
    }
    return __real_sqlite3_step(st);
}
char *__wrap_r2_retrieve_memories(const char *query)
{
    (void)query;
    return failed_provider == 1 ? NULL : strdup(memory_text);
}
char *__wrap_r2_reward_context(void) { return failed_provider == 2 ? NULL : strdup(reward_text); }
char *__wrap_r2_addiction_report(void) { return failed_provider == 3 ? NULL : strdup(interest_text); }

static char *make_text(char fill, size_t size, const char *tail)
{
    size_t n = strlen(tail);
    char *out = malloc(size + n + 1); assert(out);
    memset(out, fill, size); memcpy(out + size, tail, n + 1);
    return out;
}
static void expect_contains(char *actual, const char *complete)
{
    static int check; ++check;
    if (!actual || !strstr(actual, complete)) fprintf(stderr, "Complete-content check %d failed (returned=%zu expected=%zu).\n", check, actual ? strlen(actual) : 0, strlen(complete));
    assert(actual && strstr(actual, complete)); free(actual);
}
static int search_quoted_sentinel(const char *query, int limit,
    int (*visit)(void *, int64_t, const char *, size_t), void *context)
{
    (void)query; (void)limit;
    const char row[] = "(No visual experiences found.)\nTAIL";
    return visit(context, 1, row, sizeof(row) - 1);
}

static void expect_copies(char *actual, const char *complete, size_t expected)
{
    assert(actual);
    size_t count = 0, length = strlen(complete);
    for (const char *match = actual; (match = strstr(match, complete)); match += length)
        ++count;
    assert(count == expected);
    free(actual);
}
static void insert_visual(sqlite3 *connection, const char *description, int image)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(connection,
        "INSERT INTO r2_visual_experiences(created_utc,source,image_path,model,description,"
        "width,height,frame_number,frame_timestamp) VALUES('now','visual-source',?,'fixture',?,1,1,1,1)",
        -1, &st, NULL) == SQLITE_OK);
    char path[80]; snprintf(path, sizeof(path), "fixture-%d.png", image);
    assert(sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_bind_text(st, 2, description, -1, SQLITE_TRANSIENT) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE); sqlite3_finalize(st);
}
static void verify_complete_reads(void)
{
    expect_contains(r2_diary_recent(20), diary_text);
    expect_contains(r2_diary_search("DIARY-END", 20), diary_text);
    /* The old 4096-byte search-term ceiling must not reject a real match. */
    char query[6001]; memcpy(query, diary_text, 6000); query[6000] = '\0';
    expect_contains(r2_diary_search(query, 20), diary_text);
    expect_contains(r2_visual_recent(20), visual_text);
    expect_contains(r2_visual_search("VISUAL-END", 20), visual_text);
    char *ctx = r2_diary_build_reflection_context(20);
    assert(ctx && strstr(ctx, diary_text) && strstr(ctx, visual_text) &&
           strstr(ctx, memory_text) && strstr(ctx, "CONVERSATION-END")); free(ctx);
}
int main(void)
{
    diary_text = make_text('d', 150000, "\n\nDIARY-END\xe2\x98\x83");
    visual_text = make_text('v', 90000, "\n\nVISUAL-END\xe2\x98\x83");
    memory_text = make_text('m', 70000, "MEMORY-END");
    reward_text = make_text('r', 15000, "REWARD-END");
    interest_text = make_text('i', 17000, "INTEREST-END");
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    char *empty = r2_diary_recent(20); assert(empty && !*empty); free(empty);
    empty = r2_diary_search("missing", 20); assert(empty && !*empty); free(empty);
    assert(r2_diary_write(diary_text) == 0 && r2_diary_write("other diary row") == 0);
    assert(r2_log_sensory("visual_fixture", "complete reflection evidence", visual_text, "fixture") > 0);
    assert(r2_log_conversation_turn("actual user", "CONVERSATION-END") > 0);
    char database[PATH_MAX], library[PATH_MAX];
    snprintf(database, sizeof(database), "%s/visual.db", R2_HOME);
    snprintf(library, sizeof(library), "%s/visual", R2_HOME);
    assert(r2_visual_init(database, library) == 0);
    sqlite3 *connection = NULL;
    assert(sqlite3_open(database, &connection) == SQLITE_OK);
    insert_visual(connection, visual_text, 1); insert_visual(connection, "another visual row", 2);
    verify_complete_reads();

    char keywords[MAX_MEMORY_KEYWORDS][64] = {{"DIARY-END"}, {"DIARY"}};
    expect_copies(r2_collect_associated_records(keywords, 2, r2_diary_visit_search), diary_text, 1);
    assert(r2_diary_write(diary_text) == 0);
    expect_copies(r2_collect_associated_records(keywords, 2, r2_diary_visit_search), diary_text, 2);
    snprintf(keywords[0], sizeof(keywords[0]), "VISUAL-END");
    snprintf(keywords[1], sizeof(keywords[1]), "visual-source");
    expect_copies(r2_collect_associated_records(keywords, 2, r2_visual_visit_search), visual_text, 1);
    insert_visual(connection, visual_text, 3);
    expect_copies(r2_collect_associated_records(keywords, 2, r2_visual_visit_search), visual_text, 2);
    char *visual_context = strdup("");
    assert(visual_context);
    assert(r2_append_visual_associations(&visual_context, keywords, 1, 1) == 0);
    assert(strstr(visual_context, "RELATED VISUAL EXPERIENCES"));
    assert(strstr(visual_context, "PRIOR VISUAL EXPERIENCES"));
    assert(strstr(visual_context, "another visual row"));
    expect_copies(visual_context, visual_text, 2);
    expect_contains(r2_collect_associated_records(keywords, 1, search_quoted_sentinel), "TAIL");
    expect_copies(r2_visual_search("not-in-the-fixture", 2), "(No visual experiences found.)\n", 1);
    char *assembled = strdup(memory_text); assert(assembled);
    assert(r2_append_association_section(&assembled, "DIARY:\n", diary_text) == 0);
    assert(r2_append_association_section(&assembled, "VISUAL:\n", visual_text) == 0);
    assert(strstr(assembled, diary_text) && strstr(assembled, visual_text) && strstr(assembled, memory_text));
    free(assembled);
    assembled = strdup("BASE"); assert(assembled);
    char *original = assembled;
    failed_malloc_size = strlen("BASE") + strlen("HEADING") + strlen("RECORD") + 4;
    assert(r2_append_association_section(&assembled, "HEADING", "RECORD") == -1);
    assert(assembled == original && !strcmp(assembled, "BASE"));
    failed_malloc_size = 0; free(assembled);

    char *learning = build_learning_context();
    assert(learning && strstr(learning, reward_text) && strstr(learning, interest_text)); free(learning);
    failed_provider = 1; assert(!r2_diary_build_reflection_context(20));
    for (failed_provider = 2; failed_provider <= 3; ++failed_provider) assert(!build_learning_context());
    failed_provider = 0;

    /* Real queries must discard even a valid first row if their next step fails. */
    failed_sql = "SELECT entry,created_at,id FROM diary_entries"; allowed_rows = 1;
    assert(!r2_diary_recent(20));
    allowed_rows = 1; assert(!r2_diary_search("%", 20));
    allowed_rows = 1; assert(!r2_diary_build_reflection_context(20));
    snprintf(keywords[0], sizeof(keywords[0]), "DIARY-END");
    allowed_rows = 1; assert(!r2_collect_associated_records(keywords, 1, r2_diary_visit_search));
    failed_sql = "SELECT id,local_time,category,event_type"; allowed_rows = 1;
    assert(!r2_diary_build_reflection_context(20));
    failed_sql = "SELECT created_utc,source,model,description"; allowed_rows = 1;
    assert(!r2_visual_recent(20));
    snprintf(keywords[0], sizeof(keywords[0]), "VISUAL-END");
    allowed_rows = 1; assert(!r2_collect_associated_records(keywords, 1, r2_visual_visit_search));
    failed_sql = NULL;
    assert(sqlite3_close(connection) == SQLITE_OK);
    r2_visual_shutdown(); r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    assert(r2_visual_init(database, library) == 0);
    verify_complete_reads();
    /* An embedded-NUL historical row cannot be returned as a complete prefix. */
    assert(sqlite3_open(database, &connection) == SQLITE_OK);
    assert(sqlite3_exec(connection, "UPDATE r2_visual_experiences SET description=CAST(x'410042' AS TEXT)", NULL, NULL, NULL) == SQLITE_OK);
    assert(!r2_visual_recent(20));
    assert(sqlite3_close(connection) == SQLITE_OK);
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    assert(sqlite3_exec(connection, "UPDATE diary_entries SET entry=CAST(x'410042' AS TEXT)", NULL, NULL, NULL) == SQLITE_OK);
    assert(!r2_diary_recent(20));
    assert(sqlite3_close(connection) == SQLITE_OK);
    r2_visual_shutdown(); r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    free(diary_text); free(visual_text); free(memory_text); free(reward_text); free(interest_text);
    puts("Complete diary/visual/reflection/learning context survives restart; partial queries and failed required providers are rejected.");
    return 0;
}
