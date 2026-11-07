/* Execute the real serializer, CURL setup and response handler without a server. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>
#include <stdarg.h>

static const char *posted;
static curl_off_t posted_size;
static size_t (*write_response)(void *, size_t, size_t, void *);
static void *write_data;
static int perform_count, cleanup_count;
static CURLoption failed_option;
static int fail_predict_update;
static char *response_body;
static struct json_object *captured;
static int capture_reviews;
static char *reviewed_bytes;
static size_t reviewed_size;
static int staged_calls;

int __real_json_object_object_add(struct json_object *, const char *, struct json_object *);
int __wrap_json_object_object_add(struct json_object *object, const char *key, struct json_object *value)
{
    struct json_object *existing = NULL;
    if (fail_predict_update && !strcmp(key, "num_predict") &&
        json_object_object_get_ex(object, key, &existing)) return -1;
    return __real_json_object_object_add(object, key, value);
}
CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    (void)curl;
    if (option == failed_option) return CURLE_OUT_OF_MEMORY;
    va_list ap; va_start(ap, option);
    switch (option) {
    case CURLOPT_POSTFIELDS: posted = va_arg(ap, const char *); break;
    case CURLOPT_POSTFIELDSIZE_LARGE: posted_size = va_arg(ap, curl_off_t); break;
    case CURLOPT_WRITEFUNCTION:
        write_response = va_arg(ap, size_t (*)(void *, size_t, size_t, void *)); break;
    case CURLOPT_WRITEDATA: write_data = va_arg(ap, void *); break;
    default: break;
    }
    va_end(ap);
    return CURLE_OK;
}
CURLcode __wrap_curl_easy_perform(CURL *curl)
{
    (void)curl; ++perform_count;
    assert(posted && posted_size == (curl_off_t)strlen(posted));
    assert(write_response && write_data && response_body);
    if (captured) json_object_put(captured);
    captured = json_tokener_parse(posted); assert(captured);
    if (capture_reviews) {
        struct json_object *array = NULL;
        assert(json_object_object_get_ex(captured, "messages", &array));
        size_t request_bytes = 0;
        for (size_t i = 0; i < json_object_array_length(array); ++i) {
            struct json_object *content = NULL;
            assert(json_object_object_get_ex(json_object_array_get_idx(array, i), "content", &content));
            const char *value = json_object_get_string(content);
            request_bytes += strlen(value);
            if (strncmp(value, "EVIDENCE REVIEW\n", strlen("EVIDENCE REVIEW\n"))) continue;
            ++staged_calls;
            unsigned round;
            size_t begin, end, total;
            const char *position = strstr(value, "\nRound ");
            assert(position && sscanf(position, "\nRound %u, bytes [%zu,%zu) of %zu.",
                                      &round, &begin, &end, &total) == 4);
            const char *fragment = strstr(value, "BEGIN FRAGMENT\n");
            assert(fragment);
            fragment += strlen("BEGIN FRAGMENT\n");
            assert(strlen(fragment) == end - begin);
            if (round == 0) {
                assert(begin == reviewed_size && end <= total);
                char *grown = realloc(reviewed_bytes, end + 1);
                assert(grown);
                reviewed_bytes = grown;
                memcpy(grown + begin, fragment, end - begin + 1);
                reviewed_size = end;
            }
        }
        assert(request_bytes <= (size_t)(4096 - 512 - 512) * 32U / 10U);
    }
    size_t n = strlen(response_body), split = n / 3;
    assert(write_response(response_body, 1, split, write_data) == split);
    assert(write_response(response_body + split, 1, n - split, write_data) == n - split);
    return CURLE_OK;
}
CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
    (void)curl; assert(info == CURLINFO_RESPONSE_CODE);
    va_list ap; va_start(ap, info); *va_arg(ap, long *) = 200; va_end(ap);
    return CURLE_OK;
}
void __real_curl_easy_cleanup(CURL *);
void __wrap_curl_easy_cleanup(CURL *curl)
{
    ++cleanup_count; __real_curl_easy_cleanup(curl);
}
static char *text(size_t n, char fill, const char *prefix)
{
    size_t p = strlen(prefix); assert(n >= p);
    char *out = malloc(n + 1); assert(out);
    memcpy(out, prefix, p); memset(out + p, fill, n - p); out[n] = '\0';
    return out;
}
static void set_response(const char *answer, const char *reason, int done)
{
    if (captured) json_object_put(captured);
    captured = NULL; posted = NULL; posted_size = 0; write_data = NULL; write_response = NULL;
    free(response_body); response_body = NULL;
    struct json_object *r = json_object_new_object(), *m = json_object_new_object();
    assert(r && m);
    json_object_object_add(m, "role", json_object_new_string("assistant"));
    json_object_object_add(m, "content", json_object_new_string(answer));
    json_object_object_add(m, "thinking", json_object_new_string("PRIVATE-THINKING-NEVER-ARCHIVE"));
    json_object_object_add(r, "message", m);
    json_object_object_add(r, "done", json_object_new_boolean(done));
    if (reason) json_object_object_add(r, "done_reason", json_object_new_string(reason));
    response_body = strdup(json_object_to_json_string_ext(r, JSON_C_TO_STRING_PLAIN));
    assert(response_body); json_object_put(r);
}
static void check_request(Message *messages, size_t count, const char *override,
                          int ctx, int requested, int expect_reallocation)
{
    char setting[24]; snprintf(setting, sizeof(setting), "%d", ctx);
    assert(setenv("R2_OLLAMA_NUM_CTX", setting, 1) == 0);
    const char *query = NULL;
    size_t required = override ? strlen(override) : 0;
    for (size_t i = 0; i < count; ++i) {
        required += strlen(messages[i].content);
        if (!strcmp(messages[i].role, "user") && !messages[i].internal_tool_result) query = messages[i].content;
    }
    ollama_authoritative_user_query = query;
    set_response("ACTUAL COMPLETE ANSWER", "stop", 1);
    int before = perform_count;
    char *answer = ollama_chat_with_limit(messages, count, override, requested, 60);
    assert(answer && !strcmp(answer, "ACTUAL COMPLETE ANSWER") && perform_count == before + 1);
    free(answer);
    struct json_object *options = NULL, *value = NULL, *array = NULL;
    assert(json_object_object_get_ex(captured, "options", &options));
    assert(json_object_object_get_ex(options, "num_ctx", &value) && json_object_get_int(value) == ctx);
    assert(json_object_object_get_ex(options, "num_predict", &value));
    int predict = json_object_get_int(value);
    assert(predict >= 64 && predict <= requested);
    assert(!expect_reallocation || predict < requested);
    assert(required <= (size_t)(ctx - predict - 512) * 32U / 10U);
    assert(json_object_object_get_ex(captured, "messages", &array));
    assert(json_object_array_length(array) == count + (override != NULL));
    size_t matches = 0;
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        struct json_object *entry = json_object_array_get_idx(array, i), *content = NULL, *role = NULL;
        assert(json_object_object_get_ex(entry, "content", &content));
        assert(json_object_object_get_ex(entry, "role", &role));
        const char *actual = json_object_get_string(content), *actual_role = json_object_get_string(role);
        int found = override && !strcmp(actual_role, "system") && !strcmp(actual, override);
        for (size_t j = 0; j < count; ++j)
            if (!strcmp(actual_role, messages[j].role) && !strcmp(actual, messages[j].content)) found = 1;
        assert(found); ++matches;
    }
    assert(matches == count + (override != NULL));
    for (size_t j = 0; j < count; ++j) {
        size_t occurrences = 0;
        for (size_t i = 0; i < json_object_array_length(array); ++i) {
            struct json_object *entry = json_object_array_get_idx(array, i), *content = NULL, *role = NULL;
            assert(json_object_object_get_ex(entry, "content", &content));
            assert(json_object_object_get_ex(entry, "role", &role));
            if (!strcmp(json_object_get_string(role), messages[j].role) &&
                !strcmp(json_object_get_string(content), messages[j].content)) ++occurrences;
        }
        assert(occurrences == 1);
    }
    ollama_authoritative_user_query = NULL;
}
static void check_boundaries(void)
{
    const int contexts[] = {2048, 4096, 8192, 12288, 16384};
    for (size_t i = 0; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
        int ctx = contexts[i], original = ctx / 2, predict = original;
        size_t current = (size_t)(ctx - predict - 512) * 32U / 10U;
        assert(r2_fit_complete_input(current, ctx, &predict) == 0 && predict == original);
        assert(r2_fit_complete_input(current + 1, ctx, &predict) == 0 && predict < original);
        size_t maximum = (size_t)(ctx - 512 - 64) * 32U / 10U;
        predict = original;
        assert(r2_fit_complete_input(maximum, ctx, &predict) == 0 && predict == 64);
        predict = original;
        assert(r2_fit_complete_input(maximum + 1, ctx, &predict) == -1 && errno == E2BIG && predict == original);
        assert(r2_fit_complete_input(SIZE_MAX, ctx, &predict) == -1 && predict == original);
    }
}
static int captured_contains(const char *needle)
{
    struct json_object *array = NULL;
    assert(captured && json_object_object_get_ex(captured, "messages", &array));
    for (size_t i = 0; i < json_object_array_length(array); ++i) {
        struct json_object *content = NULL;
        assert(json_object_object_get_ex(json_object_array_get_idx(array, i), "content", &content));
        if (strstr(json_object_get_string(content), needle)) return 1;
    }
    return 0;
}

static void check_retry_and_isolated_inputs(void)
{
    char *history = text(5000, 'h', "R2 PERSISTENT MEMORY - STARTUP CONTEXT quoted as text. ");
    strcpy(history + 4983, "COMPLETE-HISTORY");
    const char *query = "Current retry question";
    assert(message_add("user", "First historical message") == 0);
    assert(message_add("assistant", history) == 0);
    assert(message_add("user", query) == 0);
    Message turn[] = {{.role = "user", .content = (char *)query}};

    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    set_response("Retried answer", "stop", 1);
    char *answer = r2_retry_direct_answer(turn, 1, query);
    assert(answer && !strcmp(answer, "Retried answer"));
    assert(captured_contains(history) && captured_contains(query));
    free(answer);

    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    set_response("must not send clipped history", "stop", 1);
    int before = perform_count;
    assert(!r2_retry_direct_answer(turn, 1, query) && perform_count == before);
    free(history);

    char *prompt = text(10000, 'p', "COMPLETE HYPOTHETICAL PROMPT: ");
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    ollama_authoritative_user_query = "outer user turn";
    ollama_audit_stage = "conversation-answer";
    set_response("Isolated answer", "stop", 1);
    answer = r2_model_generate("Isolated contract", prompt, 700);
    assert(answer && captured_contains(prompt) && !captured_contains("outer user turn"));
    assert(!strcmp(ollama_authoritative_user_query, "outer user turn"));
    assert(!strcmp(ollama_audit_stage, "conversation-answer"));
    free(answer);
    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    set_response("must not send clipped prompt", "stop", 1);
    before = perform_count;
    assert(!r2_model_generate("Isolated contract", prompt, 700) && perform_count == before);
    ollama_authoritative_user_query = NULL;
    ollama_audit_stage = NULL;
    free(prompt);
}

static void check_imagination_persistence_and_feedback(void)
{
    assert(r2_altself_init() == 0 && r2_reward_init() == 0);
    char *context = text(10000, 'c', "Complete evidence snapshot");
    char *notes = text(3000, 'n', "Long feedback evidence");
    strcpy(context + 9988, "CONTEXT-TAIL");
    strcpy(notes + 2987, "FEEDBACK-TAIL");
    int64_t branch = r2_altself_create_imagination(
        "Real database fixture", "Hypothetical", "A possible scene", context);
    assert(branch > 0);
    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER ignore_new_branch BEFORE INSERT ON r2_alternate_self_branches "
        "BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_altself_create_imagination("Ignored branch", "Hypothetical", "Possible", context) < 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER ignore_new_branch;", NULL, NULL, NULL) == SQLITE_OK);
    r2_altself_shutdown();
    assert(r2_altself_init() == 0);
    char *archived = r2_altself_show(branch);
    assert(archived && strstr(archived, context));
    free(archived);

    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER deny_feedback_link BEFORE INSERT ON r2_alternate_self_event_links "
        "BEGIN SELECT RAISE(ABORT,'fixture rejects link'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    char target[96];
    snprintf(target, sizeof(target), "imagination_branch_%lld", (long long)branch);
    assert(r2_imagination_feedback(branch, "accurate", notes) == 3);
    assert(r2_reward_current_modifier(target) == 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER deny_feedback_link;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_imagination_feedback(branch, "accurate", notes) == 0);
    assert(r2_imagination_feedback(branch, "accurate", notes) == 2);
    sqlite3_stmt *read = NULL;
    assert(sqlite3_prepare_v2(fixture,
        "SELECT e.details FROM r2_log_events e JOIN r2_alternate_self_event_links l "
        "ON l.event_id=e.id WHERE l.branch_id=? AND l.relation='feedback_for_imagination';",
        -1, &read, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(read, 1, branch) == SQLITE_OK);
    int linked = 0;
    while (sqlite3_step(read) == SQLITE_ROW) {
        const char *details = (const char *)sqlite3_column_text(read, 0);
        assert(details && strstr(details, notes));
        ++linked;
    }
    assert(linked == 2);
    sqlite3_finalize(read);
    sqlite3_close(fixture);
    r2_reward_shutdown();
    r2_altself_shutdown();
    free(context);
    free(notes);
}

static void check_staged_imagination(void)
{
    assert(setenv("R2_OLLAMA_NUM_CTX", "4096", 1) == 0);
    assert(init_db() == 0 && r2_diary_init() == 0);
    assert(r2_altself_init() == 0 && r2_imagination_init() == 0);
    char *evidence = text(45000, 'e', "A historical hypothetical with complete evidence: ");
    strcpy(evidence + 44987, "EVIDENCE-TAIL");
    assert(r2_altself_create("Evidence source", "A previous possibility", evidence, NULL, NULL, 0) > 0);
    set_response("Hypothetical staged output with preserved uncertainty.", "stop", 1);
    capture_reviews = 1;
    char *result = r2_imagination_create("Imagine a grounded possibility.");
    capture_reviews = 0;
    assert(result && staged_calls > 1 && reviewed_size > 45000 && strstr(reviewed_bytes, evidence));
    free(result);
    char *latest = r2_altself_list(1);
    long long branch = 0;
    assert(latest && sscanf(latest, "Branch #%lld", &branch) == 1);
    free(latest);
    r2_altself_shutdown();
    assert(r2_altself_init() == 0);
    char *stored = r2_altself_show(branch);
    assert(stored && strstr(stored, reviewed_bytes) && strstr(stored, "IMAGINATION PROCESSING NOTES"));
    free(stored);
    free(evidence);
    free(reviewed_bytes);
    reviewed_bytes = NULL;
    r2_imagination_shutdown();
    r2_altself_shutdown();
    r2_diary_shutdown();
    r2_reward_shutdown();
    r2_addiction_shutdown();
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
}

int main(void)
{
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_log_init() == 0);
    check_boundaries();
    check_retry_and_isolated_inputs();
    char *identity = text(1000, 'i', "SYSTEM CONTRACT: ");
    char *retrieval = text(5000, 'r', "CURRENT TURN RETRIEVED CONTEXT\n");
    char *query = text(1000, 'u', "ACTUAL CURRENT USER: ");
    char *result = text(500, 't', "[R2_INTERNAL_TOOL_RESULT]\n");
    char *override = text(700, 'o', "COMPLETE OVERRIDE: ");
    Message turn[] = {{"system", identity, 0, 0}, {"system", retrieval, 0, 0},
        {"user", query, 0, 0}, {"assistant", "[R2_INTERNAL_TOOL_REQUEST]\nprocessed", 0, 0}, {"user", result, 0, 1}};
    ollama_audit_stage = "conversation-answer";
    check_request(turn, 5, override, 4096, 2048, 1);
    ollama_audit_stage = "unspecified";
    char *large_contract = text(25000, 'c', "HELPER CONTRACT: ");
    Message helper[] = {{"system", large_contract, 0, 0}, {"user", "helper task", 0, 0}};
    check_request(helper, 2, override, 16384, 8192, 1);
    char *large_user = text(18000, 'q', "LONG COMPLETE USER: ");
    Message user_turn = {"user", large_user, 0, 0};
    check_request(&user_turn, 1, override, 8192, 4096, 1);
    Message small = {"user", "small turn", 0, 0};
    check_request(&small, 1, large_contract, 16384, 8192, 1);
    check_request(&small, 1, "small contract", 4096, 2048, 0);

    char *oversized = text(60000, 'x', "CURRENT TURN RETRIEVED CONTEXT\n");
    Message impossible[] = {{"system", oversized, 0, 0}, {"user", "actual user", 0, 0}};
    set_response("should never run", "stop", 1);
    int before = perform_count;
    assert(!ollama_chat_with_limit(impossible, 2, NULL, 2048, 60) && perform_count == before);
    assert(strlen(impossible[0].content) == 60000);
    assert(!strncmp(impossible[0].content, "CURRENT TURN RETRIEVED CONTEXT\n", strlen("CURRENT TURN RETRIEVED CONTEXT\n")));
    /* Failed JSON replacement cannot send the original oversized output quota. */
    assert(setenv("R2_OLLAMA_NUM_CTX", "4096", 1) == 0);
    fail_predict_update = 1;
    before = perform_count;
    assert(!ollama_chat_with_limit(turn, 5, override, 2048, 60) && perform_count == before);
    fail_predict_update = 0;
    for (int i = 0; i < 2; ++i) {
        set_response("not sent", "stop", 1);
        failed_option = i ? CURLOPT_WRITEFUNCTION : CURLOPT_POSTFIELDS;
        before = perform_count;
        assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60) && perform_count == before);
    }
    failed_option = 0;
    char *partial = text(70000, 'p', "[RUN] this is incomplete output; never execute\n");
    set_response(partial, "length", 1);
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60));
    char *stored = r2_log_search("ollama_output_limit_reached", 10);
    assert(stored && strstr(stored, partial) && !strstr(stored, "PRIVATE-THINKING-NEVER-ARCHIVE")); free(stored);
    set_response("partial but not done", "stop", 0);
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60));
    set_response("placeholder", "stop", 1);
    struct json_object *nul_response = json_tokener_parse(response_body), *nul_message = NULL;
    assert(nul_response && json_object_object_get_ex(nul_response, "message", &nul_message));
    static const char nul_text[] = "BEFORE\0AFTER";
    json_object_object_add(nul_message, "content", json_object_new_string_len(nul_text, sizeof(nul_text) - 1));
    free(response_body); response_body = strdup(json_object_to_json_string_ext(nul_response, JSON_C_TO_STRING_PLAIN));
    assert(response_body); json_object_put(nul_response);
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60));
    stored = r2_log_search("ollama_invalid_final_text", 10);
    assert(stored && strstr(stored, "BEFORE\\u0000AFTER") && !strstr(stored, "PRIVATE-THINKING-NEVER-ARCHIVE")); free(stored);
    set_response("legacy complete response", NULL, 1);
    char *legacy = ollama_chat_with_limit(&small, 1, NULL, 512, 60);
    assert(legacy && !strcmp(legacy, "legacy complete response")); free(legacy);
    assert(cleanup_count == perform_count + 2);
    r2_log_shutdown(); assert(r2_log_init() == 0);
    stored = r2_log_search("ollama_output_limit_reached", 10);
    assert(stored && strstr(stored, partial)); free(stored);
    check_imagination_persistence_and_feedback();
    check_staged_imagination();
    r2_log_shutdown();
    free(identity); free(retrieval); free(query); free(result); free(override);
    free(large_contract); free(large_user); free(oversized); free(partial); free(response_body);
    if (captured) json_object_put(captured);
    curl_global_cleanup();
    puts("Actual request retains complete evidence within unchanged contexts; setup failures do not send and length-limited output is archived as incomplete.");
    return 0;
}
