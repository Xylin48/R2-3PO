/* Actual capture/ASR/result/SQLite path; only audio devices and ASR are synthetic. */
#define main r2_program_main
#include "../r2.c"
#undef main
#include <assert.h>

/* Synthetic frames have exact, distinct payload markers. The production
 * Watch workers, focus checks and model-message assembly stay in the test. */
char *__wrap_r2_visual_frame_base64(const R2VisionFrame *frame)
{
    assert(frame && frame->size == 16U * 16U * 3U);
    for (size_t i = 1; i < frame->size; ++i) assert(frame->data[i] == frame->data[0]);
    switch (frame->data[0]) {
        case 17: return strdup("CAMERA");
        case 29: return strdup("MEDIA_A");
        case 41: return strdup("MEDIA_B");
        default: assert(!"unexpected fixture pixels"); return NULL;
    }
}

static void write_executable(const char *path, const char *contents)
{
    FILE *file = fopen(path, "wb");
    assert(file && fputs(contents, file) >= 0 && fclose(file) == 0 && chmod(path, 0700) == 0);
}

static void verify_model_images(const char *media)
{
    char *camera = r2_vision_current_frame_base64();
    assert(camera && !strcmp(camera, "CAMERA"));
    char *display = NULL;
    for (int i = 0; media && !display && i < 400; ++i) {
        display = r2_vision_media_current_frame_base64();
        if (!display) {
            struct timespec pause = {0, 5000000L};
            nanosleep(&pause, NULL);
        }
    }
    if (media) assert(display && !strcmp(display, media));
    else assert(!r2_vision_media_current_frame_base64());

    struct json_object *messages = json_object_new_array(), *images = NULL;
    assert(messages && ollama_json_append_message(messages, "user", "What is here?", 13,
                                                  camera, display) == 0);
    assert(json_object_object_get_ex(json_object_array_get_idx(messages, 0), "images", &images));
    assert(json_object_array_length(images) == (media ? 2U : 1U));
    assert(!strcmp(json_object_get_string(json_object_array_get_idx(images, 0)), "CAMERA"));
    if (media)
        assert(!strcmp(json_object_get_string(json_object_array_get_idx(images, 1)), media));
    json_object_put(messages);
    free(camera);
    free(display);
}

static void verify_visual_sources(sqlite3 *connection)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(connection,
        "SELECT count(DISTINCT source) FROM r2_log_events WHERE event_type='vision_source_opened' "
        "AND source IN ('fixture-camera','fixture-media-a','fixture-media-b')",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 3);
    sqlite3_finalize(st);
}

static char names[2][1024];
static int reroute_on_exit, read_fault;
static const char *growing_file;
ssize_t __real_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream);
ssize_t __wrap_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream)
{
    if (read_fault == 2) { errno = EIO; return -1; }
    if (read_fault == 3) {
        FILE *append = fopen(growing_file, "ab");
        assert(append && fputs("-GROWN-END", append) >= 0 && fclose(append) == 0);
    }
    ssize_t n = __real_getdelim(line, capacity, delimiter, stream);
    if (read_fault == 1 && n > 1) { n /= 2; (*line)[n] = '\0'; }
    return n;
}
int __wrap_r2_ears_get_event(R2Ears *receiver, R2HearingEvent *event)
{
    assert(receiver == ears || receiver == ears_desktop);
    memset(event, 0, sizeof(*event));
    event->source_name = strdup(names[receiver == ears_desktop]);
    return event->source_name ? 0 : -1;
}
pid_t __real_waitpid(pid_t child, int *status, int flags);
pid_t __wrap_waitpid(pid_t child, int *status, int flags)
{
    pid_t rc = __real_waitpid(child, status, flags);
    if (rc == child && reroute_on_exit)
        strcpy(names[reroute_on_exit - 1], "a.different.source.after.capture");
    return rc;
}
static void sql(sqlite3 *connection, const char *statement)
{
    char *error = NULL;
    int rc = sqlite3_exec(connection, statement, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "SQL fixture: %s\n", error ? error : "unknown");
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}
static void verify_event(sqlite3 *connection, int64_t id, const char *source, const char *text)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(connection,
        "SELECT e.source,e.details,j.event_id FROM r2_log_events e "
        "JOIN r2_sensory_journal j ON j.event_id=e.id WHERE e.id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 0), source));
    assert(!strcmp((const char *)sqlite3_column_text(st, 1), text));
    sqlite3_finalize(st);
}
static void verify_turn(sqlite3 *connection, int64_t id, const char *user, const char *reply)
{
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(connection,
        "SELECT c.user_text,c.assistant_text,e.details FROM r2_log_conversations c "
        "JOIN r2_log_events e ON e.id=c.event_id WHERE c.event_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 0), user));
    assert(!strcmp((const char *)sqlite3_column_text(st, 1), reply));
    char *details = NULL;
    assert(asprintf(&details, "USER SAID:\n%s\nR2 REPLIED:\n%s", user, reply) > 0);
    assert(!strcmp((const char *)sqlite3_column_text(st, 2), details));
    free(details);
    sqlite3_finalize(st);
}
static void test_file_reads(void)
{
    char path[] = "/tmp/r2-complete-read-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && write(fd, "complete-original", 17) == 17 && close(fd) == 0);
    for (read_fault = 1; read_fault <= 2; ++read_fault) {
        errno = 0;
        assert(read_entire_file(path) == NULL && errno == EIO);
    }
    read_fault = 3;
    growing_file = path;
    char *text = read_entire_file(path);
    assert(text && !strcmp(text, "complete-original-GROWN-END"));
    free(text);
    read_fault = 0;
    FILE *stream = fopen(path, "wb");
    assert(stream && fwrite("abc\0hidden", 1, 10, stream) == 10 && fclose(stream) == 0);
    assert(read_entire_file(path) == NULL);
    stream = fopen(path, "wb");
    assert(stream && fclose(stream) == 0);
    text = read_entire_file(path);
    assert(text && !*text);
    free(text);
    assert(unlink(path) == 0);
}
int main(void)
{
    test_file_reads();
    assert(mkdir(R2_ROOT, 0700) == 0 || errno == EEXIST);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    assert(r2_ears_init(&ears) == 0 && r2_ears_init(&ears_desktop) == 0);
    sqlite3 *connection = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    char *expected = malloc(100001), *reply = malloc(130001);
    assert(expected && reply);
    memset(expected, 'u', 100000); strcpy(expected + 99990, "USER-END!");
    memset(reply, 'r', 130000); strcpy(reply + 129989, "REPLY-END!");
    char text_path[] = "/tmp/r2-provenance-text-XXXXXX";
    int text_fd = mkstemp(text_path);
    assert(text_fd >= 0 && write(text_fd, expected, strlen(expected)) == (ssize_t)strlen(expected));
    assert(close(text_fd) == 0);
    char cli_path[] = "/tmp/r2-provenance-cli-XXXXXX";
    int cli_fd = mkstemp(cli_path);
    assert(cli_fd >= 0);
    char script[1024];
    int n = snprintf(script, sizeof(script),
        "#!/bin/sh\nwhile [ \"$#\" -gt 0 ]; do\n"
        "if [ \"$1\" = -of ]; then shift; out=$1; fi\nshift\ndone\n"
        "cp '%s' \"$out.txt\"\n", text_path);
    assert(n > 0 && (size_t)n < sizeof(script) && write(cli_fd, script, (size_t)n) == n);
    assert(fchmod(cli_fd, 0700) == 0 && close(cli_fd) == 0);
    assert(setenv("R2_WHISPER_MODEL", text_path, 1) == 0);
    assert(setenv("R2_WHISPER_CLI", cli_path, 1) == 0);
    ears_recent_length = ears_recent_write_position = 192000;
    ears_desktop_recent_length = ears_desktop_recent_write_position = 192000;
    atomic_store(&ears_capture_running, 1);
    atomic_store(&ears_desktop_capture_running, 1);
    int64_t speech_ids[3];
    char original_sources[3][1024];
    for (int mode = 0; mode < 3; ++mode) {
        int slot = mode != 0;
        memset(names[slot], mode ? 'd' : 'm', 900);
        names[slot][450] = '|';
        names[slot][900] = '\0';
        strcpy(original_sources[mode], names[slot]);
        char *wanted = strdup(names[slot]);
        assert(wanted);
        r2_ears_desktop_requested = slot;
        r2_ears_recent_requested = 1;
        r2_ears_extra_requested = mode == 2 ? wanted : NULL;
        reroute_on_exit = slot + 1;
        char *source = NULL;
        char *transcript = r2_ears_listen_source_cancelable(1, NULL, NULL, &source, &speech_ids[mode]);
        assert(transcript && source && speech_ids[mode] > 0);
        assert(!strcmp(transcript, expected) && !strcmp(source, wanted));
        assert(strcmp(source, names[slot])); /* Device changed during ASR. */
        verify_event(connection, speech_ids[mode], source, expected);
        free(transcript); free(source); free(wanted);
    }
    /* Real separate capture workers consume deterministic RGB pipes while
     * the exact independent audio route is being transcribed and stored. */
    const char *prior_path = getenv("PATH");
    char *fixture_path = NULL;
    assert(asprintf(&fixture_path, "%s:%s", R2_ROOT, prior_path ? prior_path : "") > 0);
    assert(setenv("PATH", fixture_path, 1) == 0);
    free(fixture_path);
    write_executable(R2_ROOT "/ffprobe", "#!/bin/sh\nprintf '16x16\\n'\n");
    write_executable(R2_ROOT "/ffmpeg",
        "#!/usr/bin/python3\n"
        "import os, sys, time\n"
        "source = sys.argv[sys.argv.index('-i') + 1]\n"
        "value = {'fixture-camera': 17, 'fixture-media-a': 29, 'fixture-media-b': 41}[source]\n"
        "while True:\n"
        "    os.write(1, bytes([value]) * (16 * 16 * 3))\n"
        "    time.sleep(.01)\n");
    assert(r2_visual_init(R2_DIARY_DATABASE, R2_ROOT "/visual") == 0);
    assert(r2_eyes_init(&eyes) == 0);
    r2_eyes_set_auxiliary(eyes, 1);
    core_initialized = 1;
    assert(r2_eyes_open_file(eyes, "fixture-camera") == 0);
    assert(r2_vision_watch_start() == 0);
    assert(r2_vision_open_file("fixture-media-a") == 0);
    verify_model_images(NULL);
    assert(r2_vision_media_set_attention(1) == 0);
    verify_model_images("MEDIA_A");

    /* A primary lifecycle update can stall independently. An already
     * published extra source must still reach ASR and persist its exact ID. */
    const char extra_name[] = "fixture.secondary|exact.monitor";
    R2ExtraDesktop extra = {
        .monitor = (char *)extra_name,
        .running = ATOMIC_VAR_INIT(1),
        .history_lock = PTHREAD_MUTEX_INITIALIZER,
        .length = 192000,
        .position = 192000,
        .total_bytes = 192000,
        .generation = 1
    };
    extra.recent = calloc(1, R2_EARS_RECENT_CAPACITY);
    assert(extra.recent);
    extra_desktop_head = &extra;
    r2_ears_extra_requested = extra_name;
    int64_t independent_id;
    char *independent_source = NULL;
    pthread_mutex_lock(&ears_desktop_capture_lock);
    char *available = r2_ears_desktop_available_sources();
    assert(available && strstr(available, "capture lifecycle update in progress") &&
           strstr(available, extra_name));
    free(available);
    char *independent_transcript = r2_ears_listen_source_cancelable(
        1, NULL, NULL, &independent_source, &independent_id);
    pthread_mutex_unlock(&ears_desktop_capture_lock);
    assert(independent_transcript && !strcmp(independent_transcript, expected));
    assert(independent_source && !strcmp(independent_source, extra_name) && independent_id > 0);
    verify_event(connection, independent_id, independent_source, expected);
    free(independent_transcript);
    free(independent_source);
    extra_desktop_head = NULL;
    r2_ears_extra_requested = NULL;
    pthread_mutex_destroy(&extra.history_lock);
    free(extra.recent);

    assert(r2_vision_open_file("fixture-media-b") == 0);
    assert(!r2_vision_media_attention_status());
    verify_model_images(NULL);
    assert(r2_vision_media_set_attention(1) == 0);
    verify_model_images("MEDIA_B");
    assert(r2_vision_media_stop() == 0);
    verify_model_images(NULL);
    assert(r2_vision_watch_stop() == 0);
    r2_eyes_shutdown(eyes);
    eyes = NULL;
    r2_eyes_shutdown(media_eyes);
    media_eyes = NULL;
    r2_visual_shutdown();
    core_initialized = 0;
    verify_visual_sources(connection);

    /* Tool result must include captured identity, with no 8/16/32/64 KiB cut. */
    memset(names[1], 'z', 900); names[1][900] = '\0';
    char *request = NULL;
    assert(asprintf(&request, "desktop_source_recent|%s|1", names[1]) > 0);
    R2AudioTurnEvidence evidence = {0};
    r2_audio_turn_evidence = &evidence;
    r2_audio_turn_record(speech_ids[0]);
    r2_audio_turn_record(speech_ids[0]); /* No duplicate association. */
    char *result = r2_process_ears_listen_request(request);
    assert(result && strstr(result, expected));
    assert(strstr(result, "source=zzzzzzzz") && evidence.count == 2);
    assert(!strstr(result, "source=a.different.source"));
    free(result); free(request);
    r2_ears_extra_requested = NULL;
    r2_ears_desktop_requested = r2_ears_recent_requested = 0;
    int64_t turn = r2_log_conversation_turn(expected, reply);
    assert(turn > 0);
    r2_audio_turn_link(turn, &evidence);
    r2_audio_turn_evidence = NULL;
    free(evidence.events);
    verify_turn(connection, turn, expected, reply);
    char *recalled = r2_log_search("USER-END!", 5);
    assert(recalled && strstr(recalled, expected) && strstr(recalled, reply));
    assert(strstr(recalled, "speech_was_transcribed_for_turn"));
    free(recalled);
    recalled = r2_log_search("REPLY-END!", 2);
    char *linked_needle = NULL;
    assert(asprintf(&linked_needle, "    Details: %s", expected) > 0);
    assert(recalled && strstr(recalled, linked_needle));
    free(linked_needle); free(recalled);
    /* Exact long source queries must not broaden to a shared 1024-byte prefix. */
    char query_a[1700], query_b[1700];
    memset(query_a, 'q', 1600); strcpy(query_a + 1600, "-A%_!");
    memset(query_b, 'q', 1600); strcpy(query_b + 1600, "-B%_!");
    assert(r2_log_sensory("long_query", "same summary", "same details", query_a) > 0);
    assert(r2_log_sensory("long_query", "same summary", "same details", query_b) > 0);
    recalled = r2_sj_search(query_a, 10);
    assert(recalled && strstr(recalled, query_a) && !strstr(recalled, query_b));
    free(recalled);
    char *large = malloc(2200001);
    assert(large);
    memset(large, 'b', 2200000);
    strcpy(large + 2199985, "MULTI-MIB-END!");
    int64_t large_id = r2_log_sensory("full_record_regression",
                                    "A complete large record", large, "full.source");
    assert(large_id > 0);
    recalled = r2_log_search("MULTI-MIB-END!", 1);
    assert(recalled && strstr(recalled, large));
    free(recalled);
    recalled = r2_sj_search("MULTI-MIB-END!", 1);
    assert(recalled && strstr(recalled, large));
    assert(!strstr(recalled, "output truncated"));
    free(recalled);
    free(large);

    /* Preserve the exact turn and queue the structured archive on failure. */
    sql(connection, "CREATE TRIGGER reject_archive BEFORE INSERT ON r2_log_conversations BEGIN SELECT RAISE(FAIL,'fixture'); END;");
    int64_t fallback = r2_log_conversation_turn(expected, reply);
    assert(fallback > 0);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(connection, "SELECT instr(details,'REPLY-END!') FROM r2_log_events WHERE id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, fallback) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) > 200000);
    sqlite3_finalize(st);
    assert(sqlite3_prepare_v2(connection, "SELECT COUNT(*) FROM r2_log_pending_conversations WHERE event_id=?", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, fallback) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);
    sql(connection, "DROP TRIGGER reject_archive;");
    assert(sqlite3_close(connection) == SQLITE_OK);
    connection = NULL;
    r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    verify_turn(connection, turn, expected, reply);
    verify_turn(connection, fallback, expected, reply);
    assert(sqlite3_prepare_v2(connection, "SELECT COUNT(*) FROM r2_log_pending_conversations", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 0);
    sqlite3_finalize(st);
    for (int i = 0; i < 3; ++i) verify_event(connection, speech_ids[i], original_sources[i], expected);
    verify_event(connection, independent_id, extra_name, expected);
    verify_visual_sources(connection);
    assert(sqlite3_prepare_v2(connection, "SELECT count(*) FROM r2_log_links WHERE from_event_id=? AND relationship='speech_was_transcribed_for_turn'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, turn) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 2);
    sqlite3_finalize(st);
    FILE *bad = fopen(text_path, "wb");
    assert(bad && fwrite("bad\0hidden", 1, 10, bad) == 10 && fclose(bad) == 0);
    r2_ears_desktop_requested = r2_ears_recent_requested = 1;
    long prior_count = r2_sj_count();
    char *no_source = (char *)(uintptr_t)1;
    int64_t no_event = -99;
    assert(r2_ears_listen_source_cancelable(1, NULL, NULL, &no_source, &no_event) == NULL);
    assert(no_source == NULL && no_event == 0 && r2_sj_count() == prior_count);
    r2_ears_desktop_requested = r2_ears_recent_requested = 0;
    sqlite3_close(connection);
    atomic_store(&ears_capture_running, 0); atomic_store(&ears_desktop_capture_running, 0);
    r2_ears_shutdown(ears); r2_ears_shutdown(ears_desktop); ears = ears_desktop = NULL;
    r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    assert(unlink(text_path) == 0 && unlink(cli_path) == 0);
    free(expected); free(reply);
    puts("Complete transcripts, captured source identities, exact event links and full conversation payloads survive restart; partial text reads fail.");
    return 0;
}
