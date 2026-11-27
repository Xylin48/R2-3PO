#define _GNU_SOURCE
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef R2_ROOT
#error R2_ROOT must be set to an isolated test directory
#endif

int main(void)
{
    if (mkdir(R2_ROOT, 0755) != 0 && errno != EEXIST) {
        perror("mkdir R2_ROOT");
        return 1;
    }
    if (mkdir(R2_HOME, 0755) != 0 && errno != EEXIST) {
        perror("mkdir R2_HOME");
        return 1;
    }

    assert(init_db() == 0);
    assert(save_memory("nebula spaceship telescope satellite overview", "science") == 0);
    assert(save_memory("nebula spaceship telescope observation", "science") == 0);
    assert(save_memory("nebula telescope satellite observation", "science") == 0);
    assert(save_memory("nebula quiet", "science") == 0);

    /* Equal-scoring records must prefer the newer updated_at timestamp. */
    assert(db_exec(
        "UPDATE memories SET updated_at='2020-01-01T00:00:00' "
        "WHERE memory='nebula spaceship telescope observation';"
        "UPDATE memories SET updated_at='2030-01-01T00:00:00' "
        "WHERE memory='nebula telescope satellite observation';") == 0);

    int ranked_partial = 0;
    char *result = get_relevant_memories(
        "nebula spaceship telescope satellite", 3, SIZE_MAX, &ranked_partial);
    assert(result != NULL && ranked_partial);

    const char *strongest = strstr(
        result, "nebula spaceship telescope satellite overview");
    const char *newer_tie = strstr(
        result, "nebula telescope satellite observation");
    const char *older_tie = strstr(
        result, "nebula spaceship telescope observation");
    assert(strongest != NULL);
    assert(newer_tie != NULL);
    assert(older_tie != NULL);
    assert(strongest < newer_tie);
    assert(newer_tie < older_tie);
    assert(strstr(result, "nebula quiet") == NULL);
    free(result);

    /* A full ranking window is incomplete when another matching row is
       omitted, even though the selected three are whole and correctly ranked. */
    ranked_partial = 0;
    result = get_relevant_memories("nebula", 4, SIZE_MAX, &ranked_partial);
    assert(result && !ranked_partial);
    free(result);

    /* Equal timestamps also need a stable tie-break: newest durable ID first. */
    assert(db_exec(
        "UPDATE memories SET updated_at='2025-01-01T00:00:00' "
        "WHERE memory IN ('nebula spaceship telescope observation', "
        "'nebula telescope satellite observation');") == 0);
    result = get_relevant_memories(
        "nebula spaceship telescope satellite", 3, SIZE_MAX, NULL);
    assert(result != NULL);
    const char *higher_id = strstr(
        result, "nebula telescope satellite observation");
    const char *lower_id = strstr(
        result, "nebula spaceship telescope observation");
    assert(higher_id != NULL && lower_id != NULL);
    assert(higher_id < lower_id);
    assert(strstr(result, "nebula quiet") == NULL);

    printf("PASS: bounded memory ranking orders by score, timestamp, and stable ID; excludes weaker matches.\n");
    free(result);

    assert(db_exec("UPDATE memories SET updated_at='2025-01-01T00:00:00';") == 0);
    ranked_partial = 0;
    result = get_relevant_memories("", 2, SIZE_MAX, &ranked_partial);
    assert(result && ranked_partial && strstr(result, "nebula quiet"));
    const char *recent_first = strstr(result, "nebula quiet");
    const char *recent_second = strstr(result, "nebula telescope satellite observation");
    assert(recent_second && recent_first < recent_second &&
           !strstr(result, "nebula spaceship telescope observation"));
    free(result);
    ranked_partial = 0;
    result = get_relevant_memories("", 4, SIZE_MAX, &ranked_partial);
    assert(result && !ranked_partial && strstr(result, "nebula spaceship telescope satellite overview"));
    free(result);

    char exact_cue[257];
    memset(exact_cue, 'q', sizeof(exact_cue) - 1);
    memcpy(exact_cue + 240, "exactIdentifier", sizeof("exactIdentifier"));
    char false_prefix[64];
    memcpy(false_prefix, exact_cue, sizeof(false_prefix) - 1);
    false_prefix[sizeof(false_prefix) - 1] = '\0';
    assert(save_memory(exact_cue, "exact cue") == 0);
    assert(save_memory(false_prefix, "former false match") == 0);
    result = get_relevant_memories(exact_cue, 3, SIZE_MAX, NULL);
    assert(result && strstr(result, exact_cue) && !strstr(result, "former false match"));
    free(result);

    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    char large[22000];
    memset(large, 'x', sizeof(large));
    const char prefix[] = "rhodonite whole record ";
    const char tail[] = " exact end of oversized startup memory";
    memcpy(large, prefix, sizeof(prefix) - 1);
    memcpy(large + sizeof(large) - sizeof(tail), tail, sizeof(tail));
    assert(save_memory(large, "science") == 0);
    result = get_startup_memories(100);
    assert(result && strlen(result) < 2048 &&
           strstr(result, "nebula spaceship telescope satellite overview") &&
           strstr(result, "complete text remains available") &&
           !strstr(result, prefix) && !strstr(result, tail));
    free(result);
    result = get_relevant_memories("rhodonite", 1, SIZE_MAX, NULL);
    assert(result && strstr(result, large));
    free(result);

    /* A focused input budget can omit an otherwise matching whole record.
       The empty selected text must not claim the search found no evidence. */
    int focused_partial = 0;
    result = get_relevant_memories("rhodonite", 1, 128, &focused_partial);
    assert(result && !*result && focused_partial);
    free(result);
    focused_partial = 0;
    result = get_relevant_memories("nebula", 4, 80, &focused_partial);
    assert(result && strstr(result, "nebula") && focused_partial);
    free(result);
    focused_partial = 0;
    result = get_relevant_memories("uniquartzabsent", 4, 128,
                                   &focused_partial);
    assert(result && !*result && !focused_partial);
    free(result);

    assert(db_exec("UPDATE memories SET memory='nebula prefix'||char(0)||'tail' "
                   "WHERE memory='nebula quiet';") == 0);
    assert(get_relevant_memories("nebula", 10, SIZE_MAX, NULL) == NULL);
    assert(get_relevant_memories("", 10, SIZE_MAX, NULL) == NULL);
    int partial = 0;
    result = get_relevant_memories("nebula", 10, SIZE_MAX, &partial);
    assert(result && partial && strstr(result, "nebula spaceship telescope satellite overview"));
    free(result);
    result = get_startup_memories(10);
    assert(result && strstr(result, "contain embedded NUL bytes") &&
           strstr(result, "nebula spaceship telescope satellite overview"));
    free(result);
    /* The record budget includes one NUL, not an unconditional 256-byte
       notice reservation. Exact-size rows and real notices are independent. */
    const char boundary_category[] = "startup boundary";
    const char boundary_date[] = "2099-01-01T00:00:00";
    size_t row_text = 2048U - 1U - 7U -
        strlen(boundary_category) - strlen(boundary_date);
    char *boundary = malloc(row_text + 1U);
    assert(boundary);
    memset(boundary, 'b', row_text);
    boundary[row_text] = '\0';
    assert(save_memory(boundary, boundary_category) == 0);
    assert(db_exec("UPDATE memories SET updated_at='2099-01-01T00:00:00' "
                   "WHERE category='startup boundary';") == 0);
    result = get_startup_memories(1);
    assert(result && strlen(result) == 2047U && strstr(result, boundary));
    assert(!strstr(result, "exceeded the startup space"));
    free(result);

    assert(db_exec("UPDATE memories SET memory=memory||'z' "
                   "WHERE category='startup boundary';") == 0);
    result = get_startup_memories(1);
    assert(result && strstr(result, "1 recent records exceeded") &&
           !strstr(result, boundary));
    free(result);
    assert(db_exec("UPDATE memories SET memory=substr(memory,1,length(memory)-1) "
                   "WHERE category='startup boundary';"
                   "UPDATE memories SET updated_at='2098-01-01T00:00:00' "
                   "WHERE memory LIKE 'rhodonite whole record %';"
                   "UPDATE memories SET updated_at='2097-01-01T00:00:00' "
                   "WHERE memory='nebula prefix'||char(0)||'tail';") == 0);
    result = get_startup_memories(3);
    assert(result && strlen(result) > 2047U && strstr(result, boundary) &&
           strstr(result, "1 recent records exceeded") &&
           strstr(result, "1 recent records contain embedded NUL bytes"));
    char *wrapped = r2_format_startup_memory_context(result);
    assert(wrapped);
    const char begin[] = "----- BEGIN STARTUP PERSISTENT MEMORY -----\n";
    const char *body = strstr(wrapped, begin);
    assert(body);
    body += sizeof(begin) - 1U;
    assert(!memcmp(body, result, strlen(result)));
    assert(!strncmp(body + strlen(result), "----- END STARTUP PERSISTENT MEMORY -----",
                    sizeof("----- END STARTUP PERSISTENT MEMORY -----") - 1U));

    Message selected[] = {
        {.role = "system", .content = wrapped},
        {.role = "user", .content = "hello"}
    };
    unsigned char include[2] = {0, 1};
    size_t limits[2] = {0};
    size_t wire_bytes = strlen(wrapped) + strlen("hello") + 2U + strlen("policy");
    assert(r2_preserve_inference_units(selected, 2, include, limits, 1,
                    strlen("policy"), wire_bytes, NULL) == 0);
    assert(include[0] && limits[0] == strlen(wrapped));
    assert(r2_preserve_inference_units(selected, 2, include, limits, 1,
                    strlen("policy"), wire_bytes - 1U, NULL) == 0);
    assert(!include[0] && include[1] && limits[1] == strlen("hello"));
    free(wrapped);
    free(result);
    free(boundary);

    context_cache_invalidate();
    sqlite3_close(db);
    db = NULL;
    return 0;
}
