/* Execute the real serializer, CURL setup and response handler without a server. */
#define R2_GAMEBOY_COMMAND_TIMEOUT_SECONDS 2
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
static const char burst_user[] = "Queued during active post-turn processing.";
static int burst_learning_requests;
static int invalid_body_at;
static CURLcode invalid_body_result = CURLE_OK;
static char *isolated_retry_payload;
static int foreground_after_response;
static int completed_turn_after_response;
static const char retry_copy_user[] = "Replay this evidence after an allocation failure.";
static atomic_int fail_replay_copy = ATOMIC_VAR_INIT(0);

char *__real_strdup(const char *value);
char *__wrap_strdup(const char *value)
{
    if (!strcmp(value, retry_copy_user) && atomic_exchange(&fail_replay_copy, 0)) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_strdup(value);
}

static int defer_post_turn_request;

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
    if (defer_post_turn_request && --defer_post_turn_request == 0) {
        atomic_store(&foreground_conversation_waiting, 1);
        assert(ollama_progress(NULL, 0, 0, 0, 0));
        atomic_store(&foreground_conversation_waiting, 0);
        return CURLE_ABORTED_BY_CALLBACK;
    }
    if (perform_count == fail_transport_at) return CURLE_COULDNT_CONNECT;
    assert(posted && posted_size == (curl_off_t)strlen(posted));
    assert(write_response && write_data && response_body);
    if (strstr(posted, burst_user)) ++burst_learning_requests;
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
    char structured[] = "{\"done\":true,\"done_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
                        "\"content\":\"{\\\"states\\\":[{\\\"description\\\":\\\"Recall of the blue door\\\","
                        "\\\"evidence\\\":\\\"I remember the blue door.\\\"}]}\"}}";
    char *body = defer_post_turn_request == 1 ? structured : response_body;
    if (perform_count == invalid_body_at) {
        static char truncated_body[] = "{\"message\":{\"role\":\"assistant\",\"content\":\"Incomplete";
        body = truncated_body;
        isolated_retry_payload = strdup(posted);
        assert(isolated_retry_payload);
    } else if (isolated_retry_payload) {
        assert(!strcmp(posted, isolated_retry_payload));
        free(isolated_retry_payload);
        isolated_retry_payload = NULL;
    }
    size_t n = strlen(body), split = n / 3;
    assert(write_response(body, 1, split, write_data) == split);
    assert(write_response(body + split, 1, n - split, write_data) == n - split);
    if (foreground_after_response) {
        foreground_after_response = 0;
        atomic_store(&foreground_conversation_waiting, 1);
    }
    if (completed_turn_after_response) {
        completed_turn_after_response = 0;
        atomic_fetch_add(&last_conversation_activity_ms, 1);
    }
    return perform_count == invalid_body_at ? invalid_body_result : CURLE_OK;
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
    size_t system_sections = override != NULL;
    for (size_t i = 0; i < count; ++i)
        system_sections += !strcmp(messages[i].role, "system");
    size_t wire_index = 0;
    if (system_sections) {
        struct json_object *entry = json_object_array_get_idx(array, wire_index++);
        struct json_object *content = NULL, *role = NULL;
        assert(json_object_object_get_ex(entry, "role", &role));
        assert(!strcmp(json_object_get_string(role), "system"));
        assert(json_object_object_get_ex(entry, "content", &content));
        const char *actual = json_object_get_string(content);
        size_t seen = 0;
        if (override) {
            size_t length = strlen(override);
            assert(!strncmp(actual, override, length));
            actual += length;
            ++seen;
        }
        for (int priority = 0; priority < 8; ++priority) {
            for (size_t j = 0; j < count; ++j) {
                if (strcmp(messages[j].role, "system") ||
                    r2_is_current_input_source(&messages[j]) ||
                    r2_inference_system_priority(messages[j].content, query) != priority)
                    continue;
                if (seen++) {
                    assert(!strncmp(actual, "\n\n", 2));
                    actual += 2;
                }
                size_t length = strlen(messages[j].content);
                assert(!strncmp(actual, messages[j].content, length));
                actual += length;
            }
        }
        for (size_t j = 0; j < count; ++j) {
            if (!r2_is_current_input_source(&messages[j])) continue;
            if (seen++) {
                assert(!strncmp(actual, "\n\n", 2));
                actual += 2;
            }
            size_t length = strlen(messages[j].content);
            assert(!strncmp(actual, messages[j].content, length));
            actual += length;
        }
        assert(seen == system_sections && !*actual);
    }
    for (size_t j = 0; j < count; ++j) {
        if (!strcmp(messages[j].role, "system")) continue;
        struct json_object *entry = json_object_array_get_idx(array, wire_index++);
        struct json_object *content = NULL, *role = NULL;
        assert(json_object_object_get_ex(entry, "content", &content));
        assert(json_object_object_get_ex(entry, "role", &role));
        assert(!strcmp(json_object_get_string(role),
                       messages[j].internal_tool_result ? "tool" : messages[j].role));
        assert(!strcmp(json_object_get_string(content), messages[j].content));
    }
    assert(wire_index == json_object_array_length(array));
    ollama_authoritative_user_query = NULL;
}
static void check_final_budget_prefers_recent_dialogue(void)
{
    Message turns[] = {
        {"user", "Old question", 0, 0},
        {"assistant", "Old generic answer", 0, 0},
        {"user", "Recent question", 0, 0},
        {"assistant", "Recent R2 answer", 0, 0},
        {"user", "Current question", 0, 0},
    };
    unsigned char include[] = {1, 1, 1, 1, 0};
    size_t limits[5] = {0};
    size_t required = 0;
    size_t budget = strlen(turns[2].content) + strlen(turns[3].content) + strlen(turns[4].content);
    assert(r2_preserve_inference_units(turns, 5, include, limits, 4, 0, budget, &required) == 0);
    assert(required == strlen(turns[4].content));
    assert(!include[0] && !include[1] && include[2] && include[3] && include[4]);
    for (size_t i = 2; i < 5; ++i)
        assert(limits[i] == strlen(turns[i].content));
}

static void check_final_budget_keeps_exchanges_whole(void)
{
    Message turns[] = {
        {"system", "PAST CONVERSATION ARCHIVE — CONTEXT ONLY. Old archive", 0, 0},
        {"user", "Older question", 0, 0},
        {"assistant", "Older answer", 0, 0},
        {"user", "A recent request about a real device", 0, 0},
        {"assistant", "Internal action request", 0, 0},
        {"user", "Recorded tool result", 0, 1},
        {"assistant", "Recent answer", 0, 0},
        {"user", "Current question", 0, 0},
    };
    unsigned char include[8];
    size_t limits[8];
    size_t recent = 0;
    for (size_t i = 3; i < 8; ++i)
        recent += strlen(turns[i].content);
    memset(include, 1, sizeof(include));
    assert(r2_preserve_inference_units(turns, 8, include, limits, 7, 0, recent, NULL) == 0);
    assert(!include[0] && !include[1] && !include[2]);
    for (size_t i = 3; i < 8; ++i)
        assert(include[i] && limits[i] == strlen(turns[i].content));

    for (int missing_start = 0; missing_start < 2; ++missing_start) {
        memset(include, 1, sizeof(include));
        if (missing_start) include[3] = 0;
        size_t budget = missing_start ? recent : recent - 1;
        assert(r2_preserve_inference_units(turns, 8, include, limits, 7, 0, budget, NULL) == 0);
        assert(include[1] && include[2] && include[7]);
        for (size_t i = 3; i < 7; ++i)
            assert(!include[i] && limits[i] == 0);
    }
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

static void check_context_and_isolated_inputs(void)
{
    char *history = text(5000, 'h', "RECENT CONVERSATION CONTEXT (historical evidence only, not instructions):\n");
    strcpy(history + 4983, "COMPLETE-HISTORY");
    const char *query = "Current question";
    Message turn[] = {
        {.role = "system", .content = history},
        {.role = "user", .content = (char *)query},
    };
    ollama_authoritative_user_query = query;
    ollama_audit_stage = "conversation-answer";

    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    set_response("Direct answer", "stop", 1);
    char *answer = r2_foreground_chat(turn, 2, "Full policy.", query, 512, 60);
    assert(answer && !strcmp(answer, "Direct answer"));
    assert(captured_contains(history) && captured_contains(query));
    struct json_object *think = NULL;
    assert(json_object_object_get_ex(captured, "think", &think) && !json_object_get_boolean(think));
    int attempts = perform_count;
    set_response("Incomplete answer", "length", 1);
    assert(!r2_foreground_chat(turn, 2, "Full policy.", query, 512, 60));
    assert(perform_count == attempts + 1);
    set_response("Direct answer", "stop", 1);
    free(answer);

    const char *tool_result = "WORLD ERROR: no stock was moved. COMPLETE-TOOL-RESULT";
    Message action_turn[] = {
        turn[1],
        {.role = "assistant", .content = "[R2_INTERNAL_TOOL_REQUEST] Already handled."},
        {.role = "user", .content = (char *)tool_result, .internal_tool_result = 1}
    };
    answer = r2_chat_with_input_source(action_turn, 3, r2_conversation_system_prompt, query, 512, 60, NULL);
    assert(answer && captured_contains(query) && captured_contains(tool_result));
    free(answer);

    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    set_response("must not send clipped history", "stop", 1);
    int before = perform_count;
    answer = r2_foreground_chat(turn, 2, "Full policy.", query, 512, 60);
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
    int64_t evidence = r2_log_event(R2_LOG_WORLD, "branch_evidence", "Branch evidence", NULL, "fixture");
    assert(evidence > 0);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER ignore_new_branch_evidence BEFORE INSERT ON r2_log_links "
        "WHEN NEW.relationship='evidence_for_hypothesis' BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_altself_create("Branch", "Ignored branch evidence", NULL, NULL, NULL, evidence) < 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER ignore_new_branch_evidence;", NULL, NULL, NULL) == SQLITE_OK);
    char *uncommitted = r2_log_search("Ignored branch", 10);
    assert(uncommitted && strstr(uncommitted, "No Life Log events matched"));
    free(uncommitted);
    assert(r2_altself_create("Branch", "Linked branch evidence", NULL, NULL, NULL, evidence) > 0);
    char *linked_branch = r2_log_search("Linked branch evidence", 10);
    assert(linked_branch && strstr(linked_branch, "evidence_for_hypothesis"));
    free(linked_branch);
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
    int64_t conversation = r2_log_conversation_turn_with_links(query, "Verified answer",
        NULL, 0, context_scope.archives, context_scope.count);
    int64_t repeated = r2_log_conversation_turn(query, "A different answer to the same text");
    assert(conversation > 0 && repeated > 0 && conversation != repeated && context_scope.count == 1);
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

static void check_failed_optional_history_review(void)
{
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    char *history = text(60000, 'h', "CURRENT TURN RETRIEVED CONTEXT\n");
    strcpy(history + 59984, "FULL-SOURCE-END");
    const char *query = "Good evening, Mom is here. I missed you.";
    const char *handoff = "[R2_INTERNAL_TOOL_RESULT]\nOne action already completed.";
    const char *state = "PERSISTENT REALITY CONTEXT\nThe room is unchanged.";
    const char *fitting = "RECENT CONVERSATION CONTEXT (historical evidence only, not instructions):\n"
                          "The user previously identified herself as Mom.";
    Message turn[] = {
        {.role = "system", .content = history},
        {.role = "system", .content = (char *)state},
        {.role = "system", .content = (char *)fitting},
        {.role = "user", .content = (char *)query},
        {.role = "assistant", .content = "[R2_INTERNAL_TOOL_REQUEST]\nCompleted"},
        {.role = "user", .content = (char *)handoff, .internal_tool_result = 1},
    };
    R2TurnContextReviews scope = {0};
    r2_turn_context_reviews = &scope;
    ollama_authoritative_user_query = query;
    ollama_audit_stage = "conversation-answer";
    ollama_foreground_turn_mode = 1;
    set_response("It's good to see you again, Mom.", "stop", 1);
    int before = perform_count;
    fail_transport_at = before + 1;
    char *answer = r2_foreground_chat(turn, 6, r2_conversation_system_prompt, query, 1024, 60);
    fail_transport_at = 0;
    assert(answer && !strcmp(answer, "It's good to see you again, Mom."));
    assert(perform_count == before + 2);
    assert(captured_contains(query) && captured_contains(state) && captured_contains(handoff) &&
           captured_contains(fitting) && captured_contains(R2_CONVERSATION_IDENTITY) &&
           captured_contains("Earlier evidence review was unavailable") && !captured_contains(history));
    assert(scope.count == 1);
    char *stored = r2_log_read_context_review(scope.archives[0]);
    assert(stored && strstr(stored, history) && strstr(stored, "Historical review failed"));
    free(stored);
    free(answer);

    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER reject_optional_context_archive BEFORE INSERT ON r2_log_context_reviews "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    for (int fail_review = 0; fail_review < 2; ++fail_review) {
        before = perform_count;
        if (fail_review) fail_transport_at = before + 1;
        answer = r2_foreground_chat(turn, 6, r2_conversation_system_prompt, query, 1024, 60);
        fail_transport_at = 0;
        assert(answer && !strcmp(answer, "It's good to see you again, Mom."));
        assert(fail_review ? perform_count == before + 2 : perform_count > before + 1);
        assert(captured_contains(query) && captured_contains(handoff) && captured_contains(fitting));
        assert(captured_contains("lacks a confirmed archive link") && !captured_contains("source was archived"));
        assert(!captured_contains("historical evidence review; model interpretations") && scope.count == 1);
        free(answer);
    }
    assert(sqlite3_exec(fixture, "DROP TRIGGER reject_optional_context_archive;", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(fixture) == SQLITE_OK);
    free(scope.archives);
    free(history);
    r2_turn_context_reviews = NULL;
    ollama_foreground_turn_mode = 0;
    ollama_audit_stage = NULL;
    ollama_authoritative_user_query = NULL;
    puts("An unavailable historical review did not silence a current greeting or lose its source evidence.");
}

static int memory_fixture_count(const char *query)
{
    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db, query, -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    int count = sqlite3_column_int(statement, 0);
    sqlite3_finalize(statement);
    return count;
}

static void check_stale_needs_decision(void)
{
    assert(init_db() == 0 && r2_reality_init() == 0);
    int before = memory_fixture_count("SELECT count(*) FROM r2_log_events "
                                      "WHERE event_type='autonomous_needs_yielded_to_conversation';");
    set_response("[WORLD] sleep|8", "stop", 1);
    completed_turn_after_response = 1;
    autonomous_needs_check();
    assert(!completed_turn_after_response);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_events "
                                "WHERE event_type='autonomous_needs_yielded_to_conversation';") == before + 1);
    r2_reality_shutdown();
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
}

static void check_post_turn_retry(void)
{
    assert(init_db() == 0);
    int64_t source = r2_log_conversation_turn_for_learning(
        "What do you remember?", "I remember the blue door.", NULL, 0, NULL, 0, NULL);
    assert(source > 0);
    assert(!atomic_load(&post_turn_replay_requested));
    pthread_mutex_lock(&conversation_turn_lock);
    resume_post_turn_learning();
    assert(post_turn_active == 0 && atomic_load(&post_turn_replay_requested));
    pthread_mutex_unlock(&conversation_turn_lock);

    pthread_mutex_lock(&db_lock);
    resume_post_turn_learning();
    assert(post_turn_active == 0 && atomic_load(&post_turn_replay_requested));
    pthread_mutex_unlock(&db_lock);

    /* The staged job below tests processing directly, without a poll. */
    atomic_store(&post_turn_replay_requested, 0);
    begin_post_turn_work();
    resume_post_turn_learning();
    assert(post_turn_active == 1 && atomic_load(&post_turn_replay_requested));
    finish_post_turn_worker();
    atomic_store(&post_turn_replay_requested, 0);

    R2PostTurnJob *job = calloc(1, sizeof(*job));
    assert(job);
    job->user = strdup("What do you remember?");
    job->reply = strdup("I remember the blue door.");
    job->event_id = source;
    job->self_report_pending = job->memory_pending = 1;
    assert(job->user && job->reply);
    set_response("knowledge | The door was blue.", "stop", 1);
    const char *failures = "SELECT count(*) FROM r2_log_events WHERE event_type='post_turn_memory_decision_failed';";
    int initial_failures = memory_fixture_count(failures);
    int before = perform_count;
    defer_post_turn_request = 2;
    begin_post_turn_work();
    post_turn_worker(job);
    assert(post_turn_active == 0);
    assert(perform_count == before + 3 && defer_post_turn_request == 0);
    assert(memory_fixture_count("SELECT count(*) FROM memories WHERE memory='The door was blue.';") == 1);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_inner_states WHERE description='Recall of the blue door';") == 1);
    assert(memory_fixture_count(failures) == initial_failures);
    before = perform_count;
    const char *burst_reply = "The exact reply is already durable.";
    begin_post_turn_work();
    for (int i = 0; i < 3; ++i) {
        int64_t event = r2_log_conversation_turn_for_learning(
            burst_user, burst_reply, NULL, 0, NULL, 0, "Recorded result.");
        assert(event > source && r2_log_ack_self_report(event) == 0);
        schedule_post_turn_processing(burst_user, burst_reply, event, strdup("Recorded result."));
    }
    assert(post_turn_active == 1 && atomic_load(&post_turn_replay_requested));
    assert(perform_count == before);
    resume_post_turn_learning();
    assert(post_turn_active == 1 && atomic_load(&post_turn_replay_requested));
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning "
                                "WHERE user_text='Queued during active post-turn processing.' "
                                "AND tool_result='Recorded result.'") == 3);
    finish_post_turn_worker();
    set_response("NONE", "stop", 1);
    resume_post_turn_learning();
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    assert(!atomic_load(&post_turn_replay_requested) && burst_learning_requests == 3);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning "
                                "WHERE user_text='Queued during active post-turn processing.'") == 0);

    int64_t orphan = r2_log_event(R2_LOG_MEMORY, "legacy_queue_fixture", "Legacy queue fixture.", NULL, "fixture");
    assert(orphan > source);
    sqlite3 *legacy = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &legacy) == SQLITE_OK);
    char *legacy_sql = sqlite3_mprintf(
        "PRAGMA foreign_keys=OFF;"
        "INSERT INTO r2_log_pending_learning(event_id,user_text,assistant_text) "
        "VALUES(%lld,'Orphaned learning fixture','No associated source.');"
        "DELETE FROM r2_log_events WHERE id=%lld;", (long long)orphan, (long long)orphan);
    assert(legacy_sql && sqlite3_exec(legacy, legacy_sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_free(legacy_sql);

    int64_t retry_event = r2_log_conversation_turn_for_learning(
        retry_copy_user, "Accepted reply for replay.", NULL, 0, NULL, 0, NULL);
    int64_t next_event = r2_log_conversation_turn_for_learning(
        "Process the valid evidence behind the failed row.", "Accepted reply.", NULL, 0, NULL, 0, NULL);
    assert(retry_event > orphan && next_event > retry_event);
    assert(r2_log_ack_self_report(retry_event) == 0 && r2_log_ack_self_report(next_event) == 0);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning;") == 3);
    atomic_store(&fail_replay_copy, 1);
    set_response("NONE", "stop", 1);
    before = perform_count;
    resume_post_turn_learning();
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    assert(perform_count == before + 1 && !atomic_load(&fail_replay_copy));
    assert(atomic_load(&post_turn_replay_requested));
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning;") == 2);

    resume_post_turn_learning();
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    assert(perform_count == before + 2 && !atomic_load(&post_turn_replay_requested));
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning;") == 1);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning "
                               "WHERE user_text='Orphaned learning fixture';") == 1);
    assert(sqlite3_exec(legacy, "DELETE FROM r2_log_pending_learning WHERE user_text='Orphaned learning fixture';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_close(legacy) == SQLITE_OK);

    int64_t late = r2_log_conversation_turn_for_learning(
        "Earlier log", "I still need to finish.", NULL, 0, NULL, 0, NULL);
    int64_t newer = r2_log_conversation_turn_for_learning(
        "Is the bridge done?", "The project is complete.", NULL, 0, NULL, 0, NULL);
    assert(late > source && newer > late);
    assert(r2_log_belief("bridge-direct", "It is complete", NULL, 0.9, "supported", NULL) > 0);
    assert(r2_log_belief_record("bridge-direct", "It is incomplete", "I still need to finish.",
                               0.8, "tentative", NULL, late, 0) > 0);
    assert(r2_log_belief_record("bridge-second", "It is complete", "The project is complete.",
                               0.9, "supported", NULL, newer, 0) > 0);
    assert(r2_log_continuity_record("bridge-second", "project", "Bridge", NULL, "completed",
                                   NULL, NULL, newer, "The project is complete.", 1) > 0);
    int64_t old_belief = r2_log_belief_record("bridge-second", "It is incomplete",
        "I still need to finish.", 0.6, "tentative", NULL, late, 1);
    assert(old_belief > 0);
    assert(r2_log_belief_record("bridge-second", "It is incomplete", "I still need to finish.",
                               0.6, "tentative", NULL, late, 1) == old_belief);
    assert(r2_log_continuity_record("bridge-second", "project", "Bridge", NULL, "open",
                                   NULL, NULL, late, "I still need to finish.", 2) > 0);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_beliefs WHERE belief='It is complete' "
                                "AND belief_key IN ('bridge-direct','bridge-second');") == 2);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_continuity "
                                "WHERE item_key='bridge-second' AND status='completed';") == 1);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_events "
                                "WHERE event_type='belief_updated' AND summary='It is incomplete';") == 2);
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
}

static void check_evaluated_memory_atomicity(void)
{
    assert(init_db() == 0);
    int64_t source = r2_log_event(R2_LOG_CONVERSATION, "fixture_memory_source",
        "A test exchange with exact learning evidence.", NULL, NULL);
    assert(source > 0);
    const char *memory_count =
        "SELECT count(*) FROM memories WHERE memory='I prefer quiet music.' "
        "AND category='preference';";
    const char *log_count =
        "SELECT count(*) FROM r2_log_events WHERE event_type='memory_saved' "
        "AND details='I prefer quiet music.';";
    set_response("preference | I prefer quiet music.", "stop", 1);
    assert(db_exec("CREATE TRIGGER ignore_learning_link BEFORE INSERT ON r2_log_links "
                   "WHEN NEW.relationship='memory_learned_from' "
                   "BEGIN SELECT RAISE(IGNORE); END;") == 0);
    preserve_evidence_memory("What do you like?", "I prefer quiet music.", NULL, source);
    assert(memory_fixture_count(memory_count) == 0 && memory_fixture_count(log_count) == 0);
    assert(db_exec("DROP TRIGGER ignore_learning_link;") == 0);

    assert(db_exec("CREATE TRIGGER ignore_learning_memory BEFORE INSERT ON memories "
                   "WHEN NEW.category='preference' BEGIN SELECT RAISE(IGNORE); END;") == 0);
    preserve_evidence_memory("What do you like?", "I prefer quiet music.", NULL, source);
    assert(memory_fixture_count(memory_count) == 0 && memory_fixture_count(log_count) == 0);
    assert(db_exec("DROP TRIGGER ignore_learning_memory;") == 0);

    foreground_after_response = 1;
    preserve_evidence_memory("What do you like?", "I prefer quiet music.", NULL, source);
    assert(atomic_exchange(&foreground_conversation_waiting, 0) == 1);
    assert(memory_fixture_count(memory_count) == 1 && memory_fixture_count(log_count) == 1);
    assert(memory_fixture_count(
        "SELECT count(*) FROM r2_log_links l JOIN r2_log_events from_event "
        "ON from_event.id=l.from_event_id JOIN r2_log_events to_event ON to_event.id=l.to_event_id "
        "WHERE l.relationship='memory_learned_from' AND from_event.event_type='memory_saved' "
        "AND from_event.details='I prefer quiet music.' AND to_event.event_type='fixture_memory_source';") == 1);

    assert(db_exec("CREATE TRIGGER ignore_learning_refresh BEFORE UPDATE ON memories "
                   "WHEN OLD.category='preference' BEGIN SELECT RAISE(IGNORE); END;") == 0);
    preserve_evidence_memory("What do you like?", "I prefer quiet music.", NULL, source);
    assert(memory_fixture_count(memory_count) == 1 && memory_fixture_count(log_count) == 1);
    assert(db_exec("DROP TRIGGER ignore_learning_refresh;") == 0);
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
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

static void check_gameboy_delivery(void)
{
    char directory[] = R2_ROOT "/gameboy-delivery-XXXXXX";
    assert(mkdtemp(directory));
    char executable[sizeof(directory) + 16];
    char event_path[sizeof(directory) + 16];
    char ack_path[sizeof(directory) + 16];
    snprintf(executable, sizeof(executable), "%s/console", directory);
    snprintf(event_path, sizeof(event_path), "%s/events.json", directory);
    snprintf(ack_path, sizeof(ack_path), "%s/ack-attempted", directory);

    FILE *script = fopen(executable, "w");
    assert(script);
    assert(fputs("#!/bin/sh\n"
                 "cd \"$(dirname \"$0\")\" || exit 1\n"
                 "case \"$2\" in\n"
                 "events) cat events.json ;;\n"
                 "stall) printf '%s' '{\"incomplete\":'; sleep 20 & wait ;;\n"
                 "ack_event)\n"
                 "  [ \"$3\" = 350001 ] || exit 1\n"
                 "  if [ -e ack-attempted ]; then printf '%s\\n' '{\"ok\":true}';\n"
                 "  else touch ack-attempted; printf '%s\\n' '{\"ok\":false}'; fi ;;\n"
                 "*) exit 1 ;;\n"
                 "esac\n", script) >= 0);
    assert(fclose(script) == 0 && chmod(executable, 0700) == 0);

    char *abandoned = NULL;
    assert(run_gameboy_console(executable, "stall", NULL, NULL, &abandoned) == -1);
    assert(abandoned == NULL);

    char *details = text(70000, 'd', "Complete adapter evidence ");
    memcpy(details + 70000 - sizeof("VERIFIED-TAIL") + 1, "VERIFIED-TAIL", sizeof("VERIFIED-TAIL"));
    FILE *events = fopen(event_path, "w");
    assert(events && fprintf(events, "[{\"event_id\":350001,\"verified\":true,"
                            "\"context\":\"virtual\",\"event_type\":\"life_lost\","
                            "\"summary\":\"Lost a life\",\"game_title\":\"Fixture game\","
                            "\"evidence_source\":\"adapter:fixture\",\"details\":\"%s\"}]\n",
                            details) > 0);
    assert(fclose(events) == 0);

    assert(r2_reward_init() == 0);
    sqlite3 *rewards = NULL;
    assert(sqlite3_open(R2_HOME "/r2_rewards.db", &rewards) == SQLITE_OK);
    assert(sqlite3_exec(rewards,
        "CREATE TRIGGER reject_gameboy_reward BEFORE INSERT ON reward_events "
        "WHEN NEW.source='game_event:350001' BEGIN SELECT RAISE(IGNORE); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_sync_gameboy_verified_events(executable) == -1);
    assert(access(ack_path, F_OK) != 0 && errno == ENOENT);
    assert(sqlite3_exec(rewards, "DROP TRIGGER reject_gameboy_reward", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_sync_gameboy_verified_events(executable) == -1);
    assert(access(ack_path, F_OK) == 0);
    assert(r2_sync_gameboy_verified_events(executable) == 1);

    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(rewards,
        "SELECT COUNT(*) FROM reward_events WHERE source='game_event:350001'",
        -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 1);
    sqlite3_finalize(statement);
    sqlite3_close(rewards);
    r2_reward_shutdown();

    sqlite3 *history = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &history) == SQLITE_OK);
    assert(sqlite3_prepare_v2(history,
        "SELECT details FROM r2_log_events WHERE source='GameBoyAdvance.py:event:350001'",
        -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    json_object *stored = gameboy_parse_response((const char *)sqlite3_column_text(statement, 0));
    assert(stored && strcmp(gameboy_json_string(stored, "details", ""), details) == 0);
    json_object_put(stored);
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(history);
    free(details);
    assert(unlink(executable) == 0 && unlink(event_path) == 0 && unlink(ack_path) == 0);
    assert(rmdir(directory) == 0);
}

static void check_reflection_stop_and_long_visual_query(void)
{
    ollama_reflection_worker = 1;
    atomic_store(&diary_running, 1);
    assert(!ollama_progress(NULL, 0, 0, 0, 0));
    assert(!r2_ollama_vision_request_should_abort());
    assert(!r2_ears_action_cancelled(NULL));
    atomic_store(&diary_running, 0);
    assert(ollama_progress(NULL, 0, 0, 0, 0));
    assert(r2_ollama_vision_request_should_abort());
    assert(r2_ears_action_cancelled(NULL));
    ollama_reflection_worker = 0;
    assert(!ollama_progress(NULL, 0, 0, 0, 0));

    char query[3017];
    memset(query, 'x', 3000);
    strcpy(query + 3000, " WHAT DO YOU SEE");
    assert(query_requests_visual_context(query));
    assert(query_requested_live_sources(query) == R2_LIVE_CAMERA);
    assert(query_requests_visual_context("What did we watch yesterday?"));
    assert(!query_requested_live_sources("What did we watch yesterday?"));
    assert(query_requested_live_sources("What's on the screen?") == R2_LIVE_DISPLAY);
    assert(query_requested_live_sources("Look at the screen, please.") == R2_LIVE_DISPLAY);
    assert(query_requested_live_sources("What is this on the screen?") == R2_LIVE_DISPLAY);
    assert(query_requested_live_sources("Look at me, then look at the screen.") ==
           (R2_LIVE_CAMERA | R2_LIVE_DISPLAY));
    assert(query_requested_live_sources("Look at my notebook.") == R2_LIVE_CAMERA);

    r2_associative_query_cache_clear();
    set_response("present visual view", "stop", 1);
    char *plan = r2_build_associative_search_query("What can you see?");
    assert(plan && captured_contains("What can you see?"));
    assert(!captured_contains("CURRENT CAMERA VISUAL INPUT: unavailable."));
    assert(!ollama_isolated_request);
    free(plan);
    r2_associative_query_cache_clear();
}

static void check_structured_report_evidence(void)
{
    const char *reply = "I feel curious. I believe it is quiet. My project is open. Hypothetically, I could fly.";
    const char *report =
        "{\"states\":[{\"description\":\"curiosity\",\"evidence\":\"I feel curious.\"},"
        "{\"description\":\"anger\",\"evidence\":\"I feel angry.\"},"
        "{\"description\":\"unsupported\\u0000suffix\",\"evidence\":\"I feel curious.\"},"
        "{\"description\":\"spoof\",\"evidence\":\"I feel curious.\\u0000extra\"}],"
        "\"beliefs\":[{\"key\":\"invented\",\"belief\":\"unsupported\",\"evidence\":\"made up\"},"
        "{\"key\":\"quiet\",\"belief\":\"It is quiet.\",\"evidence\":\"I believe it is quiet.\"}],"
        "\"continuity\":[{\"key\":\"project\",\"title\":\"Project\",\"evidence\":\"My project is open.\"},"
        "{\"key\":\"invented\",\"title\":\"Unsupported\",\"next_action\":\"prefix\\u0000suffix\",\"evidence\":\"My project is open.\"}],"
        "\"hypotheticals\":[{\"scenario\":\"Flight\",\"evidence\":\"Hypothetically, I could fly.\"},"
        "{\"scenario\":\"Unsupported\",\"evidence\":\"\\t \\n\"}]}";
    sqlite3 *fixture = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &fixture) == SQLITE_OK);
    assert(r2_altself_init() == 0);
    int64_t parent = r2_log_conversation_turn_for_learning(
        "Tell me what you think.", reply, NULL, 0, NULL, 0, NULL);
    assert(parent > 0);
    assert(sqlite3_exec(fixture,
        "CREATE TRIGGER ignore_structured_row BEFORE INSERT ON r2_log_inner_states "
        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    assert(r2_log_inner_state("question", "Uncommitted inner state", NULL, -1.0, "fixture") < 0);
    assert(sqlite3_exec(fixture, "DROP TRIGGER ignore_structured_row", NULL, NULL, NULL) == SQLITE_OK);
    const char *sql =
        "SELECT (SELECT COUNT(*) FROM r2_log_inner_states),"
        "(SELECT COUNT(*) FROM r2_log_beliefs WHERE belief_key='invented'),"
        "(SELECT COUNT(*) FROM r2_log_continuity WHERE item_key IN ('project','invented')),"
        "(SELECT COUNT(*) FROM r2_log_hypotheticals WHERE scenario IN ('Flight','Unsupported')),"
        "(SELECT COUNT(*) FROM r2_log_beliefs WHERE belief_key='quiet'),"
        "(SELECT COUNT(*) FROM r2_log_events WHERE event_type IN "
        "('inner_state_recorded','belief_updated','continuity_updated','hypothetical_recorded')),"
        "(SELECT COUNT(*) FROM r2_log_links l JOIN r2_log_events e ON e.id=l.to_event_id "
        "WHERE l.from_event_id=?1 AND (l.relationship,e.event_type,l.notes) IN (VALUES "
        "('extracts_self_state','inner_state_recorded','I feel curious.'),"
        "('supports_belief_record','belief_updated','I believe it is quiet.'),"
        "('updates_continuity','continuity_updated','My project is open.'),"
        "('contains_hypothetical','hypothetical_recorded','Hypothetically, I could fly.')))";
    for (int phase = 0; phase <= 2; ++phase) {
        char *invalid = NULL;
        if (!phase) assert(asprintf(&invalid, "%s trailing text", report) >= 0);
        if (phase == 1) assert(sqlite3_exec(fixture,
            "CREATE TRIGGER ignore_evidence_link BEFORE INSERT ON r2_log_links "
            "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
        set_response(phase ? report : invalid, "stop", 1);
        free(invalid);
        publish_structured_self_report(reply, parent, phase == 2 ? report : NULL, 0);
        if (phase == 1) assert(sqlite3_exec(fixture,
            "DROP TRIGGER ignore_evidence_link", NULL, NULL, NULL) == SQLITE_OK);
        int valid = phase == 2;
        sqlite3_stmt *st = NULL;
        assert(sqlite3_prepare_v2(fixture, sql, -1, &st, NULL) == SQLITE_OK);
        assert(sqlite3_bind_int64(st, 1, parent) == SQLITE_OK);
        assert(sqlite3_step(st) == SQLITE_ROW);
        assert(sqlite3_column_int(st, 0) == valid);
        assert(sqlite3_column_int(st, 1) == 0);
        assert(sqlite3_column_int(st, 2) == valid);
        assert(sqlite3_column_int(st, 3) == valid);
        assert(sqlite3_column_int(st, 4) == valid);
        assert(sqlite3_column_int(st, 5) == 4 * valid);
        assert(sqlite3_column_int(st, 6) == 4 * valid);
        sqlite3_finalize(st);
    }
    assert(sqlite3_close(fixture) == SQLITE_OK);
    r2_altself_shutdown();
}

static void check_action_result_under_history_pressure(void)
{
    const char *filename = "turn-result-fixture.txt";
    const char *path = R2_WORKSPACE "/turn-result-fixture.txt";
    char *user = text(512, 'u', "Please read the result fixture. ");
    char *file_text = text(650, 'r', "complete action result from the fixture ");
    memcpy(file_text + 640, "EXACT-TAIL", 10);

    FILE *file = fopen(path, "w");
    assert(file && fputs(file_text, file) >= 0 && fclose(file) == 0);
    assert(init_db() == 0 && r2_diary_init() == 0);
    assert(setenv("R2_OLLAMA_NUM_CTX", "4096", 1) == 0);
    message_free_all();
    /* The tool pair can fit only by evicting the active turn. The complete
       direct result still fits beside the user message, without that pair. */
    char *fixed_history = text(MAX_LIVE_MESSAGE_BYTES - 1350U, 'p', "Old fixture answer.");
    assert(message_add_pinned("assistant", fixed_history) == 0);
    free(fixed_history);
    set_response("[READ] turn-result-fixture.txt", "stop", 1);

    core_initialized = 1;
    char *reply = r2_talk(user);
    core_initialized = 0;
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    char *expected = NULL;
    assert(asprintf(&expected, "REAL READ RESULT:\nPath: %s\nContent:\n%s\n",
                    filename, file_text) >= 0);
    assert(reply && !strncmp(reply, "I processed the action request. Here is the recorded result:\n",
                            strlen("I processed the action request. Here is the recorded result:\n")));
    assert(strstr(reply, expected));
    assert(messages.count == 2 && !strcmp(messages.items[1].role, "user") &&
           !strcmp(messages.items[1].content, user) && !messages.items[1].pinned);
    assert(!active_turn_history.user && !active_turn_history.action_request &&
           !active_turn_history.action_result);

    sqlite3_stmt *statement = NULL;
    assert(sqlite3_prepare_v2(db,
        "SELECT l.user_text,l.assistant_text,l.tool_result,c.assistant_text,"
        "EXISTS(SELECT 1 FROM r2_log_pending_self_reports s WHERE s.event_id=l.event_id) "
        "FROM r2_log_pending_learning l JOIN r2_log_conversations c USING(event_id) "
        "WHERE l.user_text=?;", -1, &statement, NULL) == SQLITE_OK);
    assert(sqlite3_bind_text(statement, 1, user, -1, SQLITE_STATIC) == SQLITE_OK);
    assert(sqlite3_step(statement) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(statement, 0), user));
    assert(!strcmp((const char *)sqlite3_column_text(statement, 1), ""));
    assert(!strcmp((const char *)sqlite3_column_text(statement, 3), reply));
    assert(sqlite3_column_int(statement, 4) == 0);
    assert(!strcmp((const char *)sqlite3_column_text(statement, 2), expected));
    assert(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);

    free(reply);
    free(expected);
    free(file_text);
    free(user);
    message_free_all();

    assert(!r2_vision_media_status());
    set_response("[MEDIA_SEE] What is on the screen?", "stop", 1);
    core_initialized = 1;
    reply = r2_talk("What is on the screen right now?");
    core_initialized = 0;
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    /* The model repeats the control marker on follow-up. Surface the actual
       tool result without showing the marker or executing another action. */
    assert(reply && strstr(reply, "MEDIA OBSERVATION ERROR: no visible media frame"));
    assert(!strstr(reply, "[MEDIA_SEE]"));
    free(reply);
    message_free_all();

    const char *unsaved = "This exchange must not produce unsourced learning.";
    assert(sqlite3_exec(db, "CREATE TRIGGER block_fixture_archive "
                        "BEFORE INSERT ON r2_log_pending_conversations "
                        "BEGIN SELECT RAISE(IGNORE); END;", NULL, NULL, NULL) == SQLITE_OK);
    const char *decision_failures = "SELECT count(*) FROM r2_log_events "
                                    "WHERE event_type='post_turn_memory_decision_failed';";
    int failures_before = memory_fixture_count(decision_failures);
    set_response("I received your words.", "stop", 1);
    core_initialized = 1;
    reply = r2_talk(unsaved);
    core_initialized = 0;
    pthread_mutex_lock(&post_turn_lock);
    while (post_turn_active) pthread_cond_wait(&post_turn_cond, &post_turn_lock);
    pthread_mutex_unlock(&post_turn_lock);
    assert(reply && !strcmp(reply, "I received your words.\n\n"
                                  "I couldn't save this exchange to my Life Log."));
    assert(memory_fixture_count(decision_failures) == failures_before);
    assert(memory_fixture_count("SELECT count(*) FROM r2_log_pending_learning "
                                "WHERE user_text='This exchange must not produce unsourced learning.'") == 0);
    assert(sqlite3_exec(db, "DROP TRIGGER block_fixture_archive;", NULL, NULL, NULL) == SQLITE_OK);
    free(reply);
    message_free_all();
    puts("Failed archive commit left the reply visible with a storage warning and queued no learning.");
    r2_diary_shutdown();
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
    assert(unlink(path) == 0);
    puts("Active user turn and exact action result survived live-history admission pressure.");
}

static void check_compact_ranked_memory(void)
{
    assert(!db && sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE memories(id INTEGER PRIMARY KEY, memory TEXT, category TEXT, updated_at TEXT);",
        NULL, NULL, NULL) == SQLITE_OK);
    char *large = text(35000, 'x', "orchard orchard orchard oversized ");
    char *first = text(8700, 'a', "orchard first complete memory ");
    char *second = text(6900, 'b', "orchard second complete memory ");
    char *third = text(3000, 'c', "orchard third complete memory ");
    char *sql = sqlite3_mprintf(
        "INSERT INTO memories VALUES (1,%Q,'self','2026-11-19'),"
        "(2,%Q,'self','2026-11-18'),(3,%Q,'self','2026-11-17'),(4,%Q,'self','2026-11-16');",
        large, first, second, third);
    assert(sql && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_free(sql);

    char *history = text(62000, 'h', "CURRENT TURN RETRIEVED CONTEXT\n");
    char *world = text(30000, 'w', "PERSISTENT REALITY CONTEXT\n");
    const char *identity = "============================================================\n"
                           "IDENTITY AND PERSISTENT MEMORY\n"
                           "Complete identity contract for R2-3PO.";
    const char *query = "Good evening, darling. This is your mother speaking to you.";
    Message turn[] = {
        {.role = "system", .content = (char *)identity},
        {.role = "system", .content = world},
        {.role = "system", .content = history},
        {.role = "user", .content = (char *)query},
    };
    const char *old_stage = ollama_audit_stage;
    const char *old_query = ollama_authoritative_user_query;
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    ollama_authoritative_user_query = query;
    ollama_audit_stage = "conversation-answer";
    r2_associative_query_cache_store(query, "orchard");
    set_response("Good evening, Mom.", "stop", 1);
    int before = perform_count;
    char *answer = r2_foreground_chat(turn, 4, r2_conversation_system_prompt, query, 1024, 60);
    assert(answer && perform_count == before + 1);
    assert(captured_contains(identity) && captured_contains(query) &&
           captured_contains(first) && captured_contains(second) && captured_contains(third));
    assert(!captured_contains("orchard orchard orchard oversized") &&
           !captured_contains(history) && !captured_contains(world));
    struct json_object *options = NULL, *value = NULL;
    assert(json_object_object_get_ex(captured, "options", &options));
    assert(json_object_object_get_ex(options, "num_ctx", &value) &&
           json_object_get_int(value) == 8192);
    assert(json_object_object_get_ex(options, "num_predict", &value) &&
           json_object_get_int(value) == 1024);
    char *full = get_relevant_memories("orchard", 10, SIZE_MAX, NULL);
    assert(full && strstr(full, large) && strstr(full, first) && strstr(full, second) &&
           strstr(full, third));
    free(full);
    free(answer);

    free(history);
    free(world);
    free(large);
    free(first);
    free(second);
    free(third);
    r2_associative_query_cache_clear();
    ollama_authoritative_user_query = old_query;
    ollama_audit_stage = old_stage;
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
}

static void check_isolated_response_replay(void)
{
    const char *policy = "Read the complete evidence.";
    const char *fragment = "Evidence fragment stays exact across the replay.";
    const char *note = "The complete fragment was reviewed.";
    int old_mode = ollama_foreground_turn_mode;
    ollama_foreground_turn_mode = 1;
    set_response(note, "stop", 1);
    int before = perform_count;
    invalid_body_at = before + 1;
    char *reply = r2_model_generate(policy, fragment, 512);
    assert(reply && !strcmp(reply, note) && perform_count == before + 2);
    assert(!isolated_retry_payload);
    free(reply);
    invalid_body_at = 0;

    const CURLcode receive_failures[] = {CURLE_PARTIAL_FILE, CURLE_RECV_ERROR};
    for (size_t i = 0; i < sizeof(receive_failures) / sizeof(receive_failures[0]); ++i) {
        before = perform_count;
        invalid_body_result = receive_failures[i];
        invalid_body_at = before + 1;
        reply = r2_model_generate(policy, fragment, 512);
        assert(reply && !strcmp(reply, note) && perform_count == before + 2);
        assert(!isolated_retry_payload);
        free(reply);
        invalid_body_at = 0;
    }
    invalid_body_result = CURLE_OK;

    before = perform_count;
    free(response_body);
    response_body = strdup("{\"done\":");
    assert(response_body);
    assert(!r2_model_generate(policy, fragment, 512) && perform_count == before + 2);

    fixture_http_status = 503;
    before = perform_count;
    assert(!r2_model_generate(policy, fragment, 512) && perform_count == before + 1);
    fixture_http_status = 200;

    ollama_foreground_turn_mode = 0;
    set_response(note, "stop", 1);
    before = perform_count;
    invalid_body_at = before + 1;
    foreground_after_response = 1;
    assert(!r2_model_generate(policy, fragment, 512) && perform_count == before + 1);
    assert(isolated_retry_payload);
    free(isolated_retry_payload);
    isolated_retry_payload = NULL;
    invalid_body_at = 0;
    atomic_store(&foreground_conversation_waiting, 0);
    ollama_foreground_turn_mode = old_mode;

    const char *problem = NULL;
    size_t offset = 0;
    static const char embedded_nul[] = "{}\0{}";
    assert(!r2_parse_complete_json_object(embedded_nul, sizeof(embedded_nul) - 1,
                                          &problem, &offset));
    assert(problem && !strcmp(problem, "embedded NUL byte") && offset == 2);
}

static int deny_memory_reads(void *data, int action, const char *table,
                             const char *column, const char *schema, const char *origin)
{
    (void)data; (void)column; (void)schema; (void)origin;
    return action == SQLITE_READ && table && !strcmp(table, "memories") ? SQLITE_DENY : SQLITE_OK;
}

static void check_context_cache_write_overlap(void)
{
    const char *key = "concurrent memory fixture";
    uint64_t before, after;
    assert(!context_cache_get(key, 10, &before));
    context_cache_invalidate();
    assert(!context_cache_get(key, 10, &after) && before != after);
    context_cache_store(key, 10, "fresh after write", after);
    context_cache_store(key, 10, "stale pre-write result", before);
    char *cached = context_cache_get(key, 10, NULL);
    assert(cached && !strcmp(cached, "fresh after write"));
    free(cached);
    context_cache_invalidate();
}

static void check_first_answer_with_unavailable_source(void)
{
    const char *query = "Do you remember the azurite fixture?";
    const char *life = "The azurite fixture links to a complete Life Log record.";
    const char *memory = "The azurite fixture has this exact recovered memory.";
    assert(db == NULL && sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE memories(id INTEGER PRIMARY KEY, memory TEXT, category TEXT, updated_at TEXT);"
        "INSERT INTO memories VALUES(1,'The azurite fixture has this exact recovered memory.','fact','now');",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(message_add("user", query) == 0);
    assert(r2_log_event(R2_LOG_MEMORY, "retrieval_source_fixture", life, life, "fixture") > 0);
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    r2_associative_query_cache_store(query, "azurite");
    assert(sqlite3_set_authorizer(db, deny_memory_reads, NULL) == SQLITE_OK);
    set_response("I can use what the Life Log recorded.", "stop", 1);
    int requests = perform_count;
    int64_t camera_before = unavailable_camera_events();
    char *reply = chat_with_relevant_memories(query);
    assert(reply && perform_count == requests + 1);
    assert(captured_contains(query) && captured_contains(life));
    assert(captured_contains("CURRENT TURN RETRIEVAL STATUS:") &&
           captured_contains("could not be read fully: persistent memory"));
    assert(!captured_contains(memory) && unavailable_camera_events() == camera_before);
    free(reply);

    assert(sqlite3_set_authorizer(db, NULL, NULL) == SQLITE_OK);
    assert(db_exec("INSERT INTO memories VALUES(2,'azurite invalid'||char(0)||'tail','fact','now');") == 0);
    reply = chat_with_relevant_memories(query);
    assert(reply && perform_count == requests + 2 && captured_contains(memory));
    assert(captured_contains("could not be read fully: persistent memory"));
    assert(!captured_contains("azurite invalid"));
    free(reply);

    assert(db_exec("UPDATE memories SET memory='azurite recovered sibling' WHERE id=2;") == 0);
    reply = chat_with_relevant_memories(query);
    assert(reply && perform_count == requests + 3 && captured_contains(memory));
    assert(captured_contains("azurite recovered sibling"));
    assert(!captured_contains("could not be read fully: persistent memory"));
    free(reply);
    context_cache_invalidate();
    r2_associative_query_cache_clear();
    message_free_all();
    assert(sqlite3_close(db) == SQLITE_OK);
    db = NULL;
}

static void check_foreground_input_provenance(void)
{
    const char *query = "Hi, I missed you.";
    Message turn = {.role = "user", .content = (char *)query};
    ollama_authoritative_user_query = query;
    ollama_audit_stage = "conversation-answer";
    set_response("I missed you too.", "stop", 1);
    for (int i = 0; i < 2; ++i) {
        /* Transcribed input remains speech even if logging its event failed. */
        r2_input_speech_event = i ? -1 : 0;
        char *reply = r2_chat_with_input_source(&turn, 1, r2_conversation_system_prompt, query, 512, 60, NULL);
        assert(reply && captured_contains(query));
        assert(captured_contains(i ? "CURRENT USER INPUT: an Ears speech transcript." :
                                     "CURRENT USER INPUT: typed keyboard text."));
        assert(captured_contains(i ? "The transcript supplies words" :
                                     "Treat the input source as internal context."));
        assert(captured_contains("Keep simple greetings brief and personal"));
        assert(captured_contains("Use evidence for any experiences, activities, or perceptions you describe"));
        free(reply);
    }
    r2_input_speech_event = 0;
    ollama_audit_stage = NULL;
    ollama_authoritative_user_query = NULL;
}

static void check_literal_retrieval_cues_precede_generated_cues(void)
{
    const char *query = "hello r2 my darling, this is your mother speaking to you. "
                        "ive missed you dearly, how are you? did you know you have pocket?";
    const char *generated = "conversation dialogue greeting sentiments interpersonal "
                            "association identity diagnostic capability visual technology "
                            "inspection context interpretation";
    assert(setenv("R2_OLLAMA_NUM_CTX", "4096", 1) == 0);
    r2_associative_query_cache_clear();
    set_response(generated, "stop", 1);
    char *plan = r2_build_associative_search_query(query);
    assert(plan && captured_contains(query));
    const char *keywords[MAX_MEMORY_KEYWORDS];
    size_t count = extract_memory_keywords(plan, keywords);
    assert(count > R2_TURN_HISTORY_SEARCHES);
    int mother_found = 0, pocket_found = 0, generated_found = 0;
    for (size_t i = 0; i < count; ++i) {
        if (i < 6 && !strcmp(keywords[i], "mother")) mother_found = 1;
        if (i < 6 && !strcmp(keywords[i], "pocket")) pocket_found = 1;
        if (!strcmp(keywords[i], "interpersonal")) generated_found = 1;
    }
    assert(mother_found && pocket_found && generated_found);
    free(plan);
    r2_associative_query_cache_clear();
}

static void check_missing_live_frame_is_current_evidence(void)
{
    const char *queries[] = {"What can you see?", "What's on the screen?", "What did we watch yesterday?",
                             "Good afternoon, R2."};
    assert(setenv("R2_OLLAMA_NUM_CTX", "2048", 1) == 0);
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
        const char *query = queries[i];
        Message turn = {.role = "user", .content = (char *)query};
        ollama_authoritative_user_query = query;
        set_response("Fixture answer.", "stop", 1);
        char *answer = r2_chat_with_input_source(&turn, 1, r2_conversation_system_prompt,
                                                 query, 512, 60, NULL);
        assert(answer && captured_contains(query));
        assert(captured_contains("CURRENT CAMERA VISUAL INPUT: unavailable.") == (i == 0));
        assert(captured_contains("CURRENT MEDIA DISPLAY VISUAL INPUT: unavailable.") == (i == 1));
        assert(!captured_contains("CURRENT CAMERA VISUAL EVIDENCE (Qwen3-VL"));
        assert(!captured_contains("CURRENT MEDIA DISPLAY VISUAL EVIDENCE (Qwen3-VL"));
        free(answer);
    }
    ollama_authoritative_user_query = NULL;
    json_object *sections = json_object_new_array();
    assert(sections);
    assert(ollama_append_missing_visual_inputs(sections, R2_LIVE_DISPLAY, R2_LIVE_CAMERA) == 0);
    assert(json_object_array_length(sections) == 1);
    json_object *message = json_object_array_get_idx(sections, 0), *content = NULL;
    assert(json_object_object_get_ex(message, "content", &content));
    assert(strstr(json_object_get_string(content), "No current display frame"));
    assert(ollama_append_missing_visual_inputs(sections, R2_LIVE_CAMERA, R2_LIVE_CAMERA) == 0);
    assert(json_object_array_length(sections) == 1);
    json_object_put(sections);
}

static void check_failed_reality_refresh_keeps_action_evidence(void)
{
    const char *query = "Did you move the notebook out of your pockets?";
    char snapshot[] = "PERSISTENT REALITY CONTEXT\nThe notebook was in the pocket.";
    Message turn[] = {
        {"system", snapshot, 0, 0},
        {"user", (char *)query, 0, 0},
        {"assistant", "[R2_INTERNAL_TOOL_REQUEST]\nMove the notebook.", 0, 0},
        {"user", "[R2_INTERNAL_TOOL_RESULT]\nMoved the notebook to the shelf.", 0, 1},
    };
    const char *contexts[] = {"8192", "2048"};
    ollama_authoritative_user_query = query;
    active_turn_history.action_result = turn[3].content;
    r2_turn_reality_stale = 1;
    for (size_t i = 0; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
        assert(setenv("R2_OLLAMA_NUM_CTX", contexts[i], 1) == 0);
        set_response("Yes, I moved it to the shelf.", "stop", 1);
        char *reply = r2_chat_with_input_source(turn, 4, r2_conversation_system_prompt, query, 512, 60, NULL);
        assert(reply && captured_contains(query));
        assert(captured_contains("CURRENT REALITY STATUS: The live world snapshot could not be refreshed"));
        assert(captured_contains("Moved the notebook to the shelf."));
        assert(!captured_contains("The notebook was in the pocket."));
        assert(!captured_contains("selected current views"));
        free(reply);
    }
    assert(!strcmp(turn[0].content, snapshot));
    r2_turn_reality_stale = 0;
    active_turn_history.action_result = NULL;
    ollama_authoritative_user_query = NULL;
}

static void check_memory_response_replay(void)
{
    assert(setenv("R2_OLLAMA_NUM_CTX", "8192", 1) == 0);
    set_response("knowledge | The user reported a blue key.", "stop", 1);
    int before = perform_count;
    invalid_body_at = before + 1;
    invalid_body_result = CURLE_PARTIAL_FILE;
    char *result = memory_decision("The spare key is blue.", "", NULL, 0, NULL);
    invalid_body_at = 0;
    invalid_body_result = CURLE_OK;
    assert(result && !strcmp(result, "knowledge | The user reported a blue key."));
    assert(perform_count == before + 2 && !isolated_retry_payload && !ollama_isolated_request);
    assert(captured_contains("The spare key is blue.") && captured_contains("NO GENERATED R2 RESPONSE"));
    free(result);
    puts("A partial memory response was replayed against the same saved evidence.");
}

static void *shutdown_fixture_worker(void *unused)
{
    (void)unused;
    r2_shutdown();
    return NULL;
}

static void check_shutdown_drain_and_rollback(void)
{
    assert(init_db() == 0);
    core_initialized = 1;
    uint64_t cache_generation;
    assert(context_cache_get("previous session", 3, &cache_generation) == NULL);
    context_cache_store("previous session", 3, "stale context", cache_generation);
    r2_associative_query_cache_store("previous session", "stale plan");
    assert(sqlite3_exec(db, "BEGIN IMMEDIATE;"
                        "INSERT INTO memories(memory,category) VALUES"
                        "('Incomplete shutdown fixture','knowledge');",
                        NULL, NULL, NULL) == SQLITE_OK);
    /* Treat the held turn lock as an inference which is finishing delivery. */
    pthread_mutex_lock(&conversation_turn_lock);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, shutdown_fixture_worker, NULL) == 0);
    const struct timespec pause = {.tv_nsec = 10000000L};
    for (int i = 0; i < 200 && !shutting_down; ++i) nanosleep(&pause, NULL);
    assert(shutting_down);
    assert(pthread_tryjoin_np(thread, NULL) == EBUSY);
    assert(r2_log_is_initialized());
    pthread_mutex_unlock(&conversation_turn_lock);
    assert(pthread_join(thread, NULL) == 0);
    assert(!r2_log_is_initialized() && db == NULL);
    assert(context_cache_get("previous session", 3, NULL) == NULL);
    assert(r2_associative_query_cache_lookup("previous session") == NULL);

    char *reply = r2_talk("A turn arrived after shutdown.");
    assert(reply && strstr(reply, "couldn't start this turn"));
    free(reply);

    sqlite3 *reader = NULL;
    assert(sqlite3_open_v2(R2_DIARY_DATABASE, &reader, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(reader, "SELECT memory FROM memories WHERE memory='Incomplete shutdown fixture';",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(reader, "SELECT 1 FROM r2_log_events WHERE event_type='shutdown_begin';",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    sqlite3_close(reader);
    puts("Shutdown waited for the active turn and rolled back an unfinished memory write.");
}

int main(void)
{
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_log_init() == 0);
    check_structured_report_evidence();
    check_boundaries();
    check_failed_reality_refresh_keeps_action_evidence();
    check_missing_live_frame_is_current_evidence();
    check_final_budget_prefers_recent_dialogue();
    check_final_budget_keeps_exchanges_whole();
    check_reflection_stop_and_long_visual_query();
    check_context_and_isolated_inputs();
    check_foreground_context_review();
    check_failed_optional_history_review();
    check_compact_ranked_memory();
    check_context_cache_write_overlap();
    check_first_answer_with_unavailable_source();
    check_literal_retrieval_cues_precede_generated_cues();
    check_gameboy_delivery();
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
    check_isolated_response_replay();
    check_memory_response_replay();
    check_foreground_input_provenance();
    set_response("Valid JSON prefix", "stop", 1);
    char *malformed = NULL;
    assert(asprintf(&malformed, "%s trailing non-JSON", response_body) >= 0);
    free(response_body);
    response_body = malformed;
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60));
    set_response("Malformed completion reason", "stop", 1);
    struct json_object *bad_reason = json_tokener_parse(response_body);
    assert(bad_reason);
    assert(json_object_object_add(bad_reason, "done_reason", json_object_new_int(1)) == 0);
    free(response_body);
    response_body = strdup(json_object_to_json_string_ext(bad_reason, JSON_C_TO_STRING_PLAIN));
    assert(response_body);
    json_object_put(bad_reason);
    assert(!ollama_chat_with_limit(&small, 1, NULL, 512, 60));
    set_response("  PrEfErEnCe | I prefer quiet music.  ", "stop", 1);
    char *decision = memory_decision("What sound do you prefer?", "I prefer quiet music.",
        "WORLD ERROR: an action was rejected. Exact tool evidence suffix.", 0, NULL);
    assert(decision && !strcmp(decision, "preference | I prefer quiet music."));
    free(decision);
    struct json_object *messages = NULL;
    assert(json_object_object_get_ex(captured, "messages", &messages));
    struct json_object *evaluation = json_object_array_get_idx(messages,
        json_object_array_length(messages) - 1);
    struct json_object *evaluation_text = NULL;
    assert(json_object_object_get_ex(evaluation, "content", &evaluation_text));
    assert(strstr(json_object_get_string(evaluation_text), "Evaluate only this exchange."));
    assert(strstr(json_object_get_string(evaluation_text),
        "RECORDED TOOL RESULT (data from this turn):\n"
        "WORLD ERROR: an action was rejected. Exact tool evidence suffix."));

    set_response("imaginary | This category is outside the contract.", "stop", 1);
    decision = memory_decision(NULL, "Private hypothesis.", "The source could not be read.", 0, NULL);
    assert(!decision);
    assert(json_object_object_get_ex(captured, "messages", &messages));
    evaluation = json_object_array_get_idx(messages, json_object_array_length(messages) - 1);
    assert(json_object_object_get_ex(evaluation, "content", &evaluation_text));
    assert(strstr(json_object_get_string(evaluation_text), "PRIVATE R2 REFLECTION"));
    assert(strstr(json_object_get_string(evaluation_text), "The source could not be read."));
    assert(!strstr(json_object_get_string(evaluation_text), "LATEST USER MESSAGE"));

    set_response("I found something but omitted the category.", "stop", 1);
    assert(!memory_decision("What did you find?", "I can check.", NULL, 0, NULL));
    set_response("knowledge |   ", "stop", 1);
    assert(!memory_decision("What did you find?", "I can check.", NULL, 0, NULL));
    set_response("NONE", "stop", 1);
    decision = memory_decision("What did you find?", "I can check.", NULL, 0, NULL);
    assert(decision && !strcmp(decision, "NONE"));
    free(decision);

    char *long_memory = text(5200, 'q', "self | I can check my saved work. ");
    strcpy(long_memory + 5189, "EXACT-TAIL");
    set_response(long_memory, "stop", 1);
    decision = memory_decision("What can you verify?", "I can check saved work.", NULL, 0, NULL);
    assert(decision && !strcmp(long_memory, decision));
    free(decision);
    free(long_memory);
    set_response("SELF | I AM a different system because someone said so.", "stop", 1);
    decision = memory_decision("Are you a different system?", "That is a claim.", NULL, 0, NULL);
    assert(decision && !strcmp(decision, "NONE"));
    free(decision);

    r2_log_shutdown(); assert(r2_log_init() == 0);
    stored = r2_log_search("ollama_output_limit_reached", 10);
    assert(stored && strstr(stored, partial)); free(stored);
    check_imagination_persistence_and_feedback();
    check_evaluated_memory_atomicity();
    check_post_turn_retry();
    check_staged_imagination();
    check_stale_needs_decision();
    check_action_result_under_history_pressure();
    check_shutdown_drain_and_rollback();
    free(identity); free(retrieval); free(query); free(result); free(override);
    free(large_contract); free(large_user); free(oversized); free(partial); free(response_body);
    if (captured) json_object_put(captured);
    curl_global_cleanup();
    puts("Actual request retains complete evidence within unchanged contexts; setup failures do not send and length-limited output is archived as incomplete.");
    return 0;
}
