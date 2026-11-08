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
static const char *failed_metadata;
static long fixture_http_status = 200;
static char *response_body;
static struct json_object *captured;
static int capture_reviews;
static char *reviewed_bytes;
static size_t reviewed_size;
static int staged_calls;
static int fail_transport_at;

int __real_json_object_object_add(struct json_object *, const char *, struct json_object *);
int __wrap_json_object_object_add(struct json_object *object, const char *key, struct json_object *value)
{
    struct json_object *existing = NULL;
    if (failed_metadata && !strcmp(key, failed_metadata)) return -1;
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
    if (perform_count == fail_transport_at) return CURLE_COULDNT_CONNECT;
    assert(posted && posted_size == (curl_off_t)strlen(posted));
    assert(write_response && write_data && response_body);
    if (captured) json_object_put(captured);
    captured = json_tokener_parse(posted); assert(captured);
    struct json_object *flag = NULL;
    assert(json_object_object_get_ex(captured, "truncate", &flag) &&
           json_object_is_type(flag, json_type_boolean) && !json_object_get_boolean(flag));
    assert(json_object_object_get_ex(captured, "shift", &flag) &&
           json_object_is_type(flag, json_type_boolean) && !json_object_get_boolean(flag));
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
    va_list ap; va_start(ap, info); *va_arg(ap, long *) = fixture_http_status; va_end(ap);
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

static int64_t unavailable_camera_events(void)
{
    sqlite3 *fixture = NULL;
    sqlite3_stmt *st = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_prepare_v2(fixture,
        "SELECT count(*) FROM r2_log_events WHERE event_type='visual_input_unavailable';",
        -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    int64_t count = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    assert(sqlite3_close(fixture) == SQLITE_OK);
    return count;
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
    answer = r2_retry_direct_answer(turn, 1, query);
    assert(answer && perform_count > before + 1);
    assert(captured_contains(query) && !captured_contains(history));
    free(answer);
    char *snapshot = r2_log_read_context_review(1);
    assert(snapshot && strstr(snapshot, history));
    free(snapshot);
    free(history);

    char *prompt = text(10000, 'p', "COMPLETE HYPOTHETICAL PROMPT: ");
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    ollama_authoritative_user_query = "outer user turn";
    ollama_audit_stage = "conversation-answer";
    int64_t camera_before = unavailable_camera_events();
    ollama_autonomous_sensory_input = 1;
    set_response("Isolated answer", "stop", 1);
    answer = r2_model_generate("Isolated contract", prompt, 700);
    assert(answer && captured_contains(prompt) && !captured_contains("outer user turn"));
    assert(unavailable_camera_events() == camera_before &&
           ollama_autonomous_sensory_input == 1 && !ollama_isolated_request);
    assert(!strcmp(ollama_authoritative_user_query, "outer user turn"));
    assert(!strcmp(ollama_audit_stage, "conversation-answer"));
    free(answer);
    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    set_response("must not send clipped prompt", "stop", 1);
    before = perform_count;
    assert(!r2_model_generate("Isolated contract", prompt, 700) && perform_count == before);
    assert(!ollama_isolated_request && ollama_autonomous_sensory_input == 1);
    ollama_authoritative_user_query = NULL;
    ollama_audit_stage = NULL;
    ollama_autonomous_sensory_input = 0;
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

static void check_foreground_context_review(void)
{
    assert(setenv("R2_OLLAMA_NUM_CTX", "4096", 1) == 0);
    char *history = text(60000, 'h', "CURRENT TURN RETRIEVED CONTEXT\n");
    strcpy(history + 59985, "FULL-RECALL-END");
    const char *query = "Explain what happened in the earlier conversation.";
    const char *policy = "Conversation policy: preserve evidence and uncertainty.";
    const char *reality = "PERSISTENT REALITY CONTEXT\nCurrent state is exact.";
    const char *handoff = "[R2_INTERNAL_TOOL_RESULT]\nAn action already completed.";
    Message turn[] = {
        {.role="system", .content=(char *)policy},
        {.role="system", .content=history},
        {.role="system", .content=(char *)reality},
        {.role="user", .content=(char *)query},
        {.role="assistant", .content="[R2_INTERNAL_TOOL_REQUEST]\nCompleted"},
        {.role="user", .content=(char *)handoff, .internal_tool_result=1},
    };
    R2TurnContextReviews context_scope = {0};
    r2_turn_context_reviews = &context_scope;
    ollama_authoritative_user_query = query;
    ollama_audit_stage = "conversation-answer";
    ollama_foreground_turn_mode = 1;
    set_response("Review notes or answer from the synthetic model.", "stop", 1);
    capture_reviews = 1;
    char *answer = r2_foreground_chat(turn, 6, "Complete contract", query, 2048, 60);
    capture_reviews = 0;
    assert(answer && staged_calls > 1 && reviewed_size > 60000 && strstr(reviewed_bytes, history));
    assert(captured_contains(query) && captured_contains(policy) &&
           captured_contains(reality) && captured_contains(handoff) &&
           captured_contains("model interpretations"));
    assert(!strcmp(turn[1].content, history));
    free(answer);
    free(reviewed_bytes); reviewed_bytes = NULL; reviewed_size = 0; staged_calls = 0;
    int64_t conversation = r2_log_conversation_turn(query, "Verified answer");
    int64_t repeated = r2_log_conversation_turn(query, "A different answer to the same text");
    assert(conversation > 0 && repeated > 0 && conversation != repeated && context_scope.count == 1);
    r2_context_turn_link(conversation);
    assert(r2_log_link_context_review(context_scope.archives[0], conversation) == 0);
    assert(r2_log_link_context_review(context_scope.archives[0], repeated) != 0);
    r2_log_shutdown();
    assert(r2_log_init() == 0);
    char *stored = r2_log_read_context_review(2);
    char link[96];
    snprintf(link, sizeof(link), "Conversation event ID: %lld", (long long)conversation);
    assert(stored && strstr(stored, history) && strstr(stored, link));
    free(stored);
    stored = r2_log_search("context_reviewed", 10);
    assert(stored && !strstr(stored, history) && strstr(stored, "context_archive_id="));
    free(stored);
    set_response("Intermediate review failure", "stop", 1);
    fail_transport_at = perform_count + 2;
    assert(!r2_foreground_chat(turn, 6, "Complete contract", query, 2048, 60));
    assert(perform_count == fail_transport_at);
    fail_transport_at = 0;
    assert(!r2_log_read_context_review(3));

    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER reject_context_archive BEFORE INSERT ON r2_log_context_reviews "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    set_response("Reviewed all input but cannot archive it.", "stop", 1);
    assert(!r2_foreground_chat(turn, 6, "Complete contract", query, 2048, 60));
    assert(!r2_log_read_context_review(3) && !captured_contains("context archive #"));
    assert(sqlite3_exec(fixture, "DROP TRIGGER reject_context_archive;", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "INSERT INTO r2_log_context_reviews(request,original_context,review_notes) "
        "VALUES('corrupt archive',CAST(x'410042' AS TEXT),'notes');", NULL, NULL, NULL) == SQLITE_OK);
    assert(!r2_log_read_context_review(3));
    assert(sqlite3_exec(fixture, "DELETE FROM r2_log_context_reviews WHERE id=3;",
                       NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(fixture);

    char *large_policy = text(60000, 'p', "Immutable policy");
    turn[0].content = large_policy;
    int before = perform_count;
    assert(!r2_foreground_chat(turn, 6, "Complete contract", query, 2048, 60) &&
           perform_count == before);
    free(large_policy);
    turn[0].content = (char *)policy;
    turn[1].content = "CURRENT TURN RETRIEVED CONTEXT\nA small complete record.";
    set_response("A direct small-context answer.", "stop", 1);
    before = perform_count;
    answer = r2_foreground_chat(turn, 6, "Complete contract", query, 512, 60);
    assert(answer && perform_count == before + 1 && captured_contains(turn[1].content));
    free(answer);
    free(history);
    free(context_scope.archives);
    r2_turn_context_reviews = NULL;
    ollama_foreground_turn_mode = 0;
    ollama_authoritative_user_query = NULL;
    ollama_audit_stage = NULL;
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
    check_foreground_context_review();
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
    const char *metadata[] = {"truncate", "shift"};
    for (size_t i = 0; i < sizeof(metadata) / sizeof(metadata[0]); ++i) {
        failed_metadata = metadata[i];
        int sent = perform_count;
        assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60) && perform_count == sent);
    }
    failed_metadata = NULL;
    set_response("must not accept a server-rejected prompt", "stop", 1);
    fixture_http_status = 400;
    int rejected_before = perform_count;
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60) &&
           perform_count == rejected_before + 1);
    fixture_http_status = 200;
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
