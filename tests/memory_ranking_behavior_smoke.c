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

    char *result = get_relevant_memories(
        "nebula spaceship telescope satellite", 3, SIZE_MAX, NULL);
    assert(result != NULL);

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
    result = get_relevant_memories("no matching memory exists", 4, 128,
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
    context_cache_invalidate();
    sqlite3_close(db);
    db = NULL;
    return 0;
}
