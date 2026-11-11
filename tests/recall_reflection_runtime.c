/* Complete recall boundaries and exact reflection provenance, with real SQLite. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

static int failed_context;
char *__wrap_r2_reality_context(void) { return failed_context == 1 ? NULL : strdup("REALITY-STATE"); }
char *__wrap_r2_reward_context(void) { return failed_context == 2 ? NULL : strdup("REWARD-STATE"); }
char *__wrap_r2_addiction_report(void) { return failed_context == 3 ? NULL : strdup("INTEREST-STATE"); }

static void check_reflection_context(void)
{
    assert(!append_reality_context(NULL));
    for (failed_context = 1; failed_context <= 3; ++failed_context)
        assert(!append_reality_context(strdup("DIARY-CONTEXT")));
    failed_context = 0;
    char *complete = append_reality_context(strdup("DIARY-CONTEXT"));
    assert(complete && !strcmp(complete,
        "DIARY-CONTEXT\n\nREALITY-STATE\n\nREWARD-STATE\n\nINTEREST-STATE"));
    free(complete);
}

static int64_t scalar(sqlite3 *db, const char *sql, int64_t id)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    if (id) assert(sqlite3_bind_int64(st, 1, id) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int64_t value = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return value;
}

static void check_allocator(const char *record)
{
    char *retrieval = NULL;
    assert(asprintf(&retrieval, "CURRENT TURN RETRIEVED CONTEXT\n%s", record) > 0);
    char *optional = malloc(60001);
    assert(optional); memset(optional, 'h', 60000); optional[60000] = '\0';
    Message messages[] = {
        {"system", "============================================================\nIDENTITY AND PERSISTENT MEMORY\nComplete instructions.", 1, 0},
        {"system", retrieval, 0, 0},
        {"assistant", optional, 0, 0},
        {"user", "ACTUAL USER QUERY", 0, 0},
        {"assistant", "[R2_INTERNAL_TOOL_REQUEST]\ncompleted", 0, 0},
        {"user", "[R2_INTERNAL_TOOL_RESULT]\nCOMPLETE ACTION EVIDENCE", 0, 1},
        {"system", "HELPER CONTRACT without a conversation heading: preserve every instruction.", 0, 0}
    };
    unsigned char include[] = {1, 1, 1, 1, 1, 1, 1};
    size_t lengths[] = {10, 100, 500, 2, 5, 7, 1};
    size_t required = 0;
    assert(r2_preserve_inference_units(messages, 7, include, lengths, 3,
                                       200, 40000, &required) == 0);
    assert(required > 20000 && !include[2] && lengths[2] == 0);
    struct json_object *array = json_object_new_array();
    assert(array);
    size_t out_index = 0;
    for (size_t i = 0; i < 7; ++i) {
        if (!include[i]) continue;
        assert(lengths[i] == strlen(messages[i].content));
        assert(ollama_json_append_message(array, messages[i].role, messages[i].content,
                                           lengths[i], NULL, NULL) == 0);
        struct json_object *entry = json_object_array_get_idx(array, out_index++), *text = NULL;
        assert(json_object_object_get_ex(entry, "content", &text));
        assert(!strcmp(json_object_get_string(text), messages[i].content));
    }
    json_object_put(array);
    /* A genuinely insufficient configured window must not send a prefix. */
    unsigned char old_include[7]; size_t old_lengths[7];
    memcpy(old_include, include, sizeof(include)); memcpy(old_lengths, lengths, sizeof(lengths));
    assert(r2_preserve_inference_units(messages, 7, include, lengths, 3,
                                       200, 1000, &required) == -1 && errno == E2BIG);
    assert(!memcmp(include, old_include, sizeof(include)));
    assert(!memcmp(lengths, old_lengths, sizeof(lengths)));
    assert(strstr(retrieval, "PAYLOAD-END"));
    /* A helper's otherwise unrecognized system contract is required even
       when the user text alone would fit the window. */
    Message helper[] = {{"system", optional, 0, 0}, {"user", "helper task", 0, 0}};
    unsigned char helper_include[] = {1, 1}; size_t helper_lengths[] = {10, 11};
    assert(r2_preserve_inference_units(helper, 2, helper_include, helper_lengths,
                                       1, 0, 1000, &required) == -1 && errno == E2BIG);
    assert(required > 60000 && helper_include[0] && helper_lengths[0] == 10);
    free(optional); free(retrieval);
}

int main(void)
{
    check_reflection_context();
    assert(mkdir(R2_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    int64_t second = r2_log_sensory("boundary_search", "BOUNDARY-SEARCH second", "ACTUAL-SECOND-EVENT", "second source");
    assert(second > 0);
    char *large = malloc(24001); assert(large);
    memset(large, 'p', 24000); large[24000] = '\0';
    char *payload = NULL;
    assert(asprintf(&payload,
        "BOUNDARY-SEARCH\n\nparagraph two\n\n[%lld] fabricated quoted header\n"
        "This is still the SAME event.\n\n%s\nPAYLOAD-END", (long long)second, large) > 0);
    free(large);
    int64_t first = r2_log_sensory("boundary_search", "BOUNDARY-SEARCH first", payload, "first source");
    assert(first > 0);
    R2LogRecordBatch batch = {0};
    assert(r2_log_search_records("BOUNDARY-SEARCH", 10, &batch) == 0);
    assert(batch.count == 2 && batch.records[0].event_id == first && batch.records[1].event_id == second);
    R2HistorySelection history = {0};
    assert(r2_history_append_unique_records(&history, &batch) == 0);
    assert(history.count == 2 && strstr(history.text, payload) && strstr(history.text, "ACTUAL-SECOND-EVENT"));
    size_t original_length = history.length;
    assert(r2_history_append_unique_records(&history, &batch) == 0);
    assert(history.count == 2 && history.length == original_length);
    check_allocator(history.text);
    r2_log_release_records(&batch); r2_history_release(&history); free(payload);

    int64_t speech[] = {
        r2_log_sensory("speech_transcribed", "Actual first recognized clip", "same recognized words", "physical mic"),
        r2_log_sensory("speech_transcribed", "Actual second recognized clip", "same recognized words", "movie monitor")
    };
    assert(speech[0] > 0 && speech[1] > 0);
    int64_t reflections[2], diary_ids[2];
    sqlite3 *connection = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    for (int i = 0; i < 2; ++i) {
        R2AudioTurnEvidence evidence = {.events = &speech[i], .count = 1};
        assert(r2_store_reflection("DUPLICATE_REFLECTION_TEXT", &evidence, &reflections[i]) == 0);
        assert(reflections[i] > 0);
        diary_ids[i] = scalar(connection,
            "SELECT diary_entry_id FROM r2_sensory_journal_reflection_links WHERE event_id=?", reflections[i]);
        assert(diary_ids[i] > 0);
        assert(scalar(connection,
            "SELECT to_event_id FROM r2_log_links WHERE from_event_id=? AND relationship='speech_was_transcribed_for_reflection'",
            reflections[i]) == speech[i]);
        assert(r2_sj_link_reflection(speech[i], diary_ids[i]) == -1);
    }
    /* An exact existing link cannot be redirected to another diary row
       with identical text. */
    assert(r2_sj_link_reflection(reflections[0], diary_ids[1]) == -1);

    /* Failure on the second speech edge must roll back completion, the
       diary association and the first edge. The diary itself still exists. */
    assert(sqlite3_exec(connection,
        "CREATE TRIGGER fail_reflection_edge BEFORE INSERT ON r2_log_links "
        "WHEN NEW.relationship='speech_was_transcribed_for_reflection' "
        "AND NEW.to_event_id=(SELECT MAX(id) FROM r2_log_events WHERE event_type='speech_transcribed') "
        "BEGIN SELECT RAISE(FAIL,'reflection evidence fixture'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    R2AudioTurnEvidence both = {.events = speech, .count = 2};
    int64_t incomplete = -1;
    assert(r2_store_reflection("REFLECTION_WITH_FAILED_EDGE", &both, &incomplete) == 0);
    assert(incomplete == 0);
    assert(scalar(connection, "SELECT COUNT(*) FROM diary_entries WHERE entry='REFLECTION_WITH_FAILED_EDGE'", 0) == 1);
    assert(scalar(connection, "SELECT COUNT(*) FROM r2_log_events WHERE event_type='reflection_completed'", 0) == 2);
    assert(scalar(connection, "SELECT COUNT(*) FROM r2_sensory_journal_reflection_links", 0) == 2);
    assert(scalar(connection, "SELECT COUNT(*) FROM r2_log_links WHERE relationship='speech_was_transcribed_for_reflection'", 0) == 2);
    assert(sqlite3_exec(connection, "DROP TRIGGER fail_reflection_edge;", NULL, NULL, NULL) == SQLITE_OK);

    /* Journal projection can be offline. Completion and its exact diary
       association must commit anyway and be discoverable after backfill. */
    r2_sj_shutdown();
    int64_t offline_reflection = 0;
    assert(r2_store_reflection("DUPLICATE_REFLECTION_TEXT", NULL, &offline_reflection) == 0);
    assert(offline_reflection > 0);
    int64_t offline_diary = scalar(connection,
        "SELECT diary_entry_id FROM r2_sensory_journal_reflection_links WHERE event_id=?", offline_reflection);
    assert(offline_diary > 0 && offline_diary != diary_ids[0] && offline_diary != diary_ids[1]);
    assert(scalar(connection, "SELECT COUNT(*) FROM r2_sensory_journal WHERE event_id=?", offline_reflection) == 0);
    assert(diary_ids[0] != diary_ids[1]);
    int64_t absent = -1;
    assert(r2_diary_write_with_id(" \n\t", &absent) == -1 && absent == 0);
    assert(sqlite3_close(connection) == SQLITE_OK);
    r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    assert(scalar(connection, "SELECT COUNT(*) FROM r2_sensory_journal WHERE event_id=?", offline_reflection) == 1);
    assert(scalar(connection,
        "SELECT diary_entry_id FROM r2_sensory_journal_reflection_links WHERE event_id=?", offline_reflection) == offline_diary);
    for (int i = 0; i < 2; ++i) {
        assert(scalar(connection,
            "SELECT diary_entry_id FROM r2_sensory_journal_reflection_links WHERE event_id=?", reflections[i]) == diary_ids[i]);
        assert(scalar(connection,
            "SELECT to_event_id FROM r2_log_links WHERE from_event_id=? AND relationship='speech_was_transcribed_for_reflection'",
            reflections[i]) == speech[i]);
        char needle[80]; snprintf(needle, sizeof(needle), "Diary link: %lld", (long long)diary_ids[i]);
        char *result = r2_sj_search("DUPLICATE_REFLECTION_TEXT", 10);
        assert(result && strstr(result, needle)); free(result);
    }
    assert(sqlite3_close(connection) == SQLITE_OK);
    r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    puts("Structured recall preserves blank lines/quoted IDs; complete inference units and exact speech/reflection/diary links survive restart.");
    return 0;
}
