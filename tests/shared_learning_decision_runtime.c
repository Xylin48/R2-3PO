#define _GNU_SOURCE
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <sys/stat.h>

static int scalar(const char *sql)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int value = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return value;
}

int main(void)
{
    /* Oversized originals are preserved exactly in the allocated evaluation
       input, including the final instruction that the old +64 estimate lost. */
    char *original = malloc(100001);
    assert(original);
    memset(original, 'x', 100000);
    original[100000] = '\0';
    for (int reflection = 0; reflection <= 1; ++reflection) {
        char *input = build_memory_evaluation_input(original, "RESPONSE_TAIL", reflection);
        assert(input && strstr(input, original));
        assert(strstr(input, "RESPONSE_TAIL\n\nEvaluate only this"));
        const char *suffix = reflection
            ? "Evaluate only this recorded experience and reflection."
            : "Evaluate only this exchange.";
        assert(strcmp(input + strlen(input) - strlen(suffix), suffix) == 0);
        free(input);
    }
    free(original);

    assert(mkdir(R2_ROOT, 0755) == 0 || errno == EEXIST);
    assert(init_db() == 0);
    assert(r2_log_init() == 0);
    assert(db_exec("CREATE TABLE IF NOT EXISTS diary_entries (id INTEGER PRIMARY KEY, entry TEXT NOT NULL);") == 0);
    assert(r2_sj_init() == 0);
    /* Read and durably retain a recognized transcript larger than the old
       16 KiB reader and 64 KiB event-detail restrictions. */
    const size_t transcript_n = 72000;
    char *recognized = malloc(transcript_n + 100);
    assert(recognized);
    memset(recognized, 'a', transcript_n);
    strcpy(recognized + transcript_n, "ASR_DEEP_TAIL_🌼");
    FILE *fp = tmpfile(); assert(fp);
    assert(fwrite(recognized, 1, strlen(recognized), fp) == strlen(recognized));
    char *read_back = r2_ears_read_transcript(fp); fclose(fp);
    assert(read_back && !strcmp(read_back, recognized));
    int64_t heard = r2_ears_record_transcript(read_back, 15, 1, 1);
    assert(heard > 0);
    char *details = r2_log_read_details(heard);
    assert(details && strstr(details, recognized) && strstr(details, "source=desktop"));
    free(details);
    char *recall = r2_sj_search("ASR_DEEP_TAIL_🌼", 1);
    assert(recall && strstr(recall, recognized)); free(recall);
    char *tool_output = process_tools("[SENSORY_JOURNAL] search|ASR_DEEP_TAIL_🌼|1\n");
    assert(tool_output && strstr(tool_output, recognized));
    int64_t full_tool = r2_log_event(R2_LOG_CONVERSATION, "conversation_tool_result",
        "Complete long tool result", tool_output, "r2_talk");
    assert(full_tool > 0);
    char *full_evidence = build_turn_learning_evidence("original typed message", full_tool);
    assert(full_evidence && strstr(full_evidence, recognized));
    free(full_evidence); free(tool_output);
    free(recognized); free(read_back);
    int64_t tool = r2_log_event(R2_LOG_CONVERSATION, "conversation_tool_result",
        "Chosen audio observation result", "REAL EARS RESULT: TOOL_OUTCOME_TAIL", "r2_talk");
    assert(tool > 0);
    char *evidence = build_turn_learning_evidence("CHAT_EVIDENCE_TAIL", tool);
    assert(evidence && strstr(evidence, "TOOL_OUTCOME_TAIL"));
    assert(build_turn_learning_evidence("user", INT64_MAX) == NULL);
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    int64_t chat = r2_log_conversation_turn("CHAT_EVIDENCE_TAIL", "CHAT_RESPONSE_TAIL");
    int64_t reflection = r2_log_thinking("reflection_completed", "fixture", "REFLECTION_RESPONSE_TAIL");
    assert(chat > 0 && reflection > 0);
    learn_from_recorded_experience(evidence, "CHAT_RESPONSE_TAIL", chat, 0);
    free(evidence);
    learn_from_recorded_experience("SENSORY_EVIDENCE_TAIL", "REFLECTION_RESPONSE_TAIL", reflection, 1);
    assert(scalar("SELECT count(*) FROM memories WHERE category='preference' AND memory='I prefer quiet music.'") == 1);
    assert(scalar("SELECT count(*) FROM memories WHERE category='knowledge' AND memory='I noticed the displayed flower opened.'") == 1);
    int before = scalar("SELECT count(*) FROM memories");
    learn_from_recorded_experience("NOTHING_NEW", "No new conclusion.", reflection, 1);
    assert(scalar("SELECT count(*) FROM memories") == before);
    assert(scalar("SELECT count(*) FROM r2_log_links WHERE relationship='supports_memory'") == 2);
    r2_sj_shutdown();
    r2_log_shutdown();
    sqlite3_close(db); db = NULL;
    assert(init_db() == 0);
    assert(scalar("SELECT count(*) FROM memories") == before);
    assert(scalar("SELECT count(*) FROM r2_log_links WHERE relationship='supports_memory'") == 2);
    sqlite3_close(db); db = NULL;
    curl_global_cleanup();
    puts("Shared chat/reflection learning decision runtime passed.");
    return 0;
}
