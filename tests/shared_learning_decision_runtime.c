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
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    int64_t chat = r2_log_conversation_turn("CHAT_EVIDENCE_TAIL", "CHAT_RESPONSE_TAIL");
    int64_t reflection = r2_log_thinking("reflection_completed", "fixture", "REFLECTION_RESPONSE_TAIL");
    assert(chat > 0 && reflection > 0);
    learn_from_recorded_experience("CHAT_EVIDENCE_TAIL", "CHAT_RESPONSE_TAIL", chat, 0);
    learn_from_recorded_experience("SENSORY_EVIDENCE_TAIL", "REFLECTION_RESPONSE_TAIL", reflection, 1);
    assert(scalar("SELECT count(*) FROM memories WHERE category='preference' AND memory='I prefer quiet music.'") == 1);
    assert(scalar("SELECT count(*) FROM memories WHERE category='knowledge' AND memory='I noticed the displayed flower opened.'") == 1);
    int before = scalar("SELECT count(*) FROM memories");
    learn_from_recorded_experience("NOTHING_NEW", "No new conclusion.", reflection, 1);
    assert(scalar("SELECT count(*) FROM memories") == before);
    assert(scalar("SELECT count(*) FROM r2_log_links WHERE relationship='supports_memory'") == 2);
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
