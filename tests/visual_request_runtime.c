/* Real Visual pipeline, SQLite and files; synthetic encoder child and HTTP. */
#define R2_VWEBCAM_DIR R2_ROOT "/focus"
#include "../Visual.c"
#include "r2_diary.h"
#include "SensoryJournal.h"
#include <assert.h>
#include <stdarg.h>

static const pid_t encoder_pid = 727272;
static char ppm_path[PATH_MAX], jpeg_path[PATH_MAX];
static int ppm_fd = -1, fail_write, fail_close, stall_encoder, killed_encoder, reaped_encoder;
static int cancel_request, timeout_clock, clock_calls, read_mode, read_fd = -1;
static char probe_path[PATH_MAX], database[PATH_MAX], library[PATH_MAX], next_db[PATH_MAX], next_library[PATH_MAX];
static int fail_handle, fail_headers, defer_gate, gate_active, gate_begin_count, gate_end_count, perform_count, init_count, cleanup_count, restart_library;
static uint64_t fixture_preview_session = 1;
static int switch_preview_during_request;
static CURLoption failed_option;

uint64_t __wrap_r2_eyes_preview_session(void)
{
    return fixture_preview_session;
}

static CURLcode transport_code = CURLE_OK;
static long http_status = 200;
static const char *posted;
static curl_off_t posted_size;
static size_t (*write_response)(char *, size_t, size_t, void *);
static void *write_data;
static char *body;
static json_object *request_copy;
static const R2VisionFrame *overlapping_frame;
static const char *overlapping_response;
static void response(const char *text, const char *reason);

int __real_mkstemps(char *, int);
int __wrap_mkstemps(char *path, int suffix)
{
    int fd = __real_mkstemps(path, suffix);
    if (fd >= 0 && strstr(path, ".ppm")) { snprintf(ppm_path, sizeof(ppm_path), "%s", path); ppm_fd = fd; }
    if (fd >= 0 && strstr(path, ".jpg")) snprintf(jpeg_path, sizeof(jpeg_path), "%s", path);
    return fd;
}
ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *p, size_t n)
{
    if (fail_write && fd == ppm_fd) { errno = EIO; return -1; }
    if (overlapping_frame && n == 20 && !memcmp(p, "SYNTHETIC-JPEG-BYTES", n)) {
        const R2VisionFrame *newer = overlapping_frame;
        overlapping_frame = NULL;
        response(overlapping_response, "stop");
        char *result = r2_visual_analyze_frame(newer, "camera fixture", "newer focus");
        assert(result && !strcmp(result, "valid"));
        free(result);
    }
    return __real_write(fd, p, n);
}
int __real_close(int);
int __wrap_close(int fd)
{
    int rc = __real_close(fd);
    if (fail_close && (fd == ppm_fd || fd == read_fd)) { fail_close = 0; errno = EIO; return -1; }
    return rc;
}
ssize_t __real_read(int, void *, size_t);
ssize_t __wrap_read(int fd, void *p, size_t n)
{
    read_fd = fd;
    if (read_mode == 1) { read_mode = 0; return 0; }
    if (read_mode == 2) { read_mode = 0; errno = EIO; return -1; }
    if (read_mode == 3) {
        read_mode = 0;
        int append_fd = open(probe_path, O_WRONLY | O_APPEND); assert(append_fd >= 0);
        assert(__real_write(append_fd, "MORE", 4) == 4); assert(__real_close(append_fd) == 0);
    }
    return __real_read(fd, p, n);
}
pid_t __wrap_fork(void) { killed_encoder = 0; return encoder_pid; }
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    assert(pid == encoder_pid);
    if (stall_encoder && !killed_encoder) { assert(options == WNOHANG); return 0; }
    if (!killed_encoder) {
        int fd = open(jpeg_path, O_WRONLY | O_TRUNC); assert(fd >= 0);
        assert(__real_write(fd, "SYNTHETIC-JPEG-BYTES", 20) == 20); assert(__real_close(fd) == 0);
        *status = 0;
    } else *status = SIGKILL;
    ++reaped_encoder; return pid;
}
int __wrap_kill(pid_t pid, int signal_number)
{
    assert(pid == encoder_pid && signal_number == SIGKILL);
    killed_encoder = 1; return 0;
}
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *value)
{
    int rc = __real_clock_gettime(id, value);
    if (!rc && timeout_clock && id == CLOCK_MONOTONIC && clock_calls++) value->tv_sec += 31;
    return rc;
}
int __real_r2_ollama_vision_request_should_abort(void);
int __wrap_r2_ollama_vision_request_should_abort(void)
{
    return cancel_request || __real_r2_ollama_vision_request_should_abort();
}
int __real_r2_ollama_vision_request_begin(void);
int __wrap_r2_ollama_vision_request_begin(void)
{
    ++gate_begin_count;
    if (defer_gate) return 0;
    int rc = __real_r2_ollama_vision_request_begin();
    assert(rc && !gate_active); gate_active = 1; return rc;
}
void __real_r2_ollama_vision_request_end(void);
void __wrap_r2_ollama_vision_request_end(void)
{
    assert(gate_active); gate_active = 0; ++gate_end_count;
    __real_r2_ollama_vision_request_end();
}
CURL *__real_curl_easy_init(void);
CURL *__wrap_curl_easy_init(void) { if (fail_handle) return NULL; CURL *curl = __real_curl_easy_init(); if (curl) ++init_count; return curl; }
void __real_curl_easy_cleanup(CURL *);
void __wrap_curl_easy_cleanup(CURL *curl) { ++cleanup_count; __real_curl_easy_cleanup(curl); }
struct curl_slist *__real_curl_slist_append(struct curl_slist *, const char *);
struct curl_slist *__wrap_curl_slist_append(struct curl_slist *list, const char *text)
{
    return fail_headers ? NULL : __real_curl_slist_append(list, text);
}
CURLcode __wrap_curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
    (void)curl; if (option == failed_option) return CURLE_OUT_OF_MEMORY;
    va_list ap; va_start(ap, option);
    switch (option) {
    case CURLOPT_POSTFIELDS: posted = va_arg(ap, const char *); break;
    case CURLOPT_POSTFIELDSIZE_LARGE: posted_size = va_arg(ap, curl_off_t); break;
    case CURLOPT_WRITEFUNCTION: write_response = va_arg(ap, size_t (*)(char *, size_t, size_t, void *)); break;
    case CURLOPT_WRITEDATA: write_data = va_arg(ap, void *); break;
    default: break;
    }
    va_end(ap); return CURLE_OK;
}
CURLcode __wrap_curl_easy_perform(CURL *curl)
{
    (void)curl; ++perform_count;
    assert(gate_active && posted && posted_size == (curl_off_t)strlen(posted));
    assert(write_response && write_data && body);
    if (request_copy) json_object_put(request_copy);
    request_copy = visual_parse_complete_json(posted, strlen(posted)); assert(request_copy);
    json_object *flag = NULL;
    assert(json_object_object_get_ex(request_copy, "truncate", &flag) &&
           json_object_is_type(flag, json_type_boolean) && !json_object_get_boolean(flag));
    assert(json_object_object_get_ex(request_copy, "shift", &flag) &&
           json_object_is_type(flag, json_type_boolean) && !json_object_get_boolean(flag));
    if (switch_preview_during_request) {
        switch_preview_during_request = 0;
        ++fixture_preview_session;
    }
    if (restart_library) {
        restart_library = 0;
        r2_visual_shutdown(); assert(r2_visual_init(next_db, next_library) == 0);
    }
    size_t n = strlen(body), split = n / 2;
    assert(write_response(body, 1, split, write_data) == split);
    assert(write_response(body + split, 1, n - split, write_data) == n - split);
    return transport_code;
}
CURLcode __wrap_curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
    (void)curl; assert(info == CURLINFO_RESPONSE_CODE);
    va_list ap; va_start(ap, info); *va_arg(ap, long *) = http_status; va_end(ap); return CURLE_OK;
}
static void response(const char *text, const char *reason)
{
    json_object *root = json_object_new_object(), *message = json_object_new_object(); assert(root && message);
    json_object_object_add(message, "role", json_object_new_string("assistant"));
    json_object_object_add(message, "content", json_object_new_string(text));
    json_object_object_add(message, "thinking", json_object_new_string("PRIVATE-VISION-REASONING"));
    json_object_object_add(root, "message", message); json_object_object_add(root, "done", json_object_new_boolean(1));
    if (reason) json_object_object_add(root, "done_reason", json_object_new_string(reason));
    free(body); body = strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN)); assert(body); json_object_put(root);
    posted = NULL; write_response = NULL; write_data = NULL; posted_size = 0;
}
static sqlite3_int64 scalar(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *st = NULL; assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW); sqlite3_int64 n = sqlite3_column_int64(st, 0); sqlite3_finalize(st); return n;
}
static void verify_event_link(const char *source, const char *description)
{
    sqlite3_int64 id = scalar(visual_db, "SELECT life_log_event_id FROM r2_visual_experiences WHERE description LIKE '%VISUAL-COMPLETE-END'");
    assert(id > 0);
    sqlite3 *connection = NULL; sqlite3_stmt *st = NULL;
    assert(sqlite3_open(R2_DIARY_DATABASE, &connection) == SQLITE_OK);
    assert(sqlite3_prepare_v2(connection, "SELECT source,details FROM r2_log_events WHERE id=? AND event_type='visual_experience'", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_bind_int64(st, 1, id) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW);
    assert(!strcmp((const char *)sqlite3_column_text(st, 0), source));
    assert(!strcmp((const char *)sqlite3_column_text(st, 1), description));
    sqlite3_finalize(st); assert(sqlite3_close(connection) == SQLITE_OK);
}
static void temporary_files_gone(void)
{
    assert(*ppm_path && *jpeg_path && access(ppm_path, F_OK) != 0 && access(jpeg_path, F_OK) != 0);
}
static void rejected_frame(R2VisionFrame *frame)
{
    sqlite3_int64 before = scalar(visual_db, "SELECT count(*) FROM r2_visual_experiences");
    frame->frame_number++;
    char *result = r2_visual_analyze_frame(frame, "camera fixture", "observe real frame");
    assert(!result && scalar(visual_db, "SELECT count(*) FROM r2_visual_experiences") == before);
    assert(!gate_active); temporary_files_gone();
}
int main(void)
{
    assert(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
    assert(mkdir(R2_HOME, 0700) == 0 || errno == EEXIST);
    assert(r2_diary_init() == 0 && r2_log_init() == 0 && r2_sj_init() == 0);
    snprintf(database, sizeof(database), "%s/visual.db", R2_HOME); snprintf(library, sizeof(library), "%s/library", R2_HOME);
    snprintf(next_db, sizeof(next_db), "%s/next.db", R2_HOME); snprintf(next_library, sizeof(next_library), "%s/next-library", R2_HOME);
    assert(r2_visual_init(database, library) == 0);
    unsigned char rgb[] = {1, 2, 3};
    R2VisionFrame frame = {.data=rgb, .size=sizeof(rgb), .format={1,1,3,24,R2_PIXEL_RGB24}, .timestamp=1234, .frame_number=1, .preview_session=fixture_preview_session};
    size_t encoded = 0;
    fail_write = 1; assert(!encode_jpeg(&frame, &encoded)); fail_write = 0; temporary_files_gone();
    fail_close = 1; assert(!encode_jpeg(&frame, &encoded)); fail_close = 0; temporary_files_gone();
    stall_encoder = cancel_request = 1; int before_reap = reaped_encoder;
    assert(!encode_jpeg(&frame, &encoded) && errno == ECANCELED && killed_encoder && reaped_encoder == before_reap + 1);
    temporary_files_gone(); cancel_request = 0;
    timeout_clock = 1; clock_calls = 0; before_reap = reaped_encoder;
    assert(!encode_jpeg(&frame, &encoded) && errno == ETIMEDOUT && killed_encoder && reaped_encoder == before_reap + 1);
    temporary_files_gone(); timeout_clock = stall_encoder = 0;

    snprintf(probe_path, sizeof(probe_path), "%s/probe.jpg", R2_HOME);
    int fd = open(probe_path, O_WRONLY|O_CREAT|O_TRUNC, 0600); assert(fd >= 0);
    assert(__real_write(fd, "OLD", 3) == 3 && __real_close(fd) == 0);
    read_mode = 3; unsigned char *bytes = read_file(probe_path, &encoded);
    assert(bytes && encoded == 7 && !memcmp(bytes, "OLDMORE", 7)); free(bytes);
    read_mode = 1; encoded = 999; assert(!read_file(probe_path, &encoded) && encoded == 0);
    read_mode = 2; assert(!read_file(probe_path, &encoded));
    fail_close = 1; read_fd = -1; assert(!read_file(probe_path, &encoded)); fail_close = 0;

    response("{\"description\":\"one actual observation\",\"focus\":null}", "stop");
    int before_perform = perform_count, before_gate = gate_begin_count;
    fail_handle = 1; rejected_frame(&frame); fail_handle = 0;
    fail_headers = 1;
    rejected_frame(&frame); fail_headers = 0; assert(perform_count == before_perform && gate_begin_count == before_gate);
    failed_option = CURLOPT_POSTFIELDS; rejected_frame(&frame); failed_option = CURLOPT_WRITEFUNCTION; rejected_frame(&frame);
    failed_option = 0; assert(perform_count == before_perform && gate_begin_count == before_gate);
    defer_gate = 1; rejected_frame(&frame); defer_gate = 0; assert(perform_count == before_perform);

    char source[1601]; memset(source, 's', sizeof(source)-1); source[1600]='\0'; memcpy(source, "camera | ", 9);
    char *description = malloc(90030); assert(description); memset(description, 'd', 90000); strcpy(description+90000, "VISUAL-COMPLETE-END");
    json_object *observation = json_object_new_object(); json_object_object_add(observation, "description", json_object_new_string(description));
    json_object_object_add(observation, "focus", NULL);
    response(json_object_to_json_string_ext(observation, JSON_C_TO_STRING_PLAIN), "stop");
    frame.frame_number++; char *actual = r2_visual_analyze_frame(&frame, source, "deliberate question");
    assert(actual && !strcmp(actual, description)); free(actual); temporary_files_gone();
    char *history = r2_visual_search("VISUAL-COMPLETE-END", 10); assert(history && strstr(history, description) && strstr(history, source)); free(history);
    verify_event_link(source, description);
    json_object_put(observation);

    const char *invalid[] = {"{\"description\":\"prefix\"} trailing", "{\"focus\":null}", "{\"description\":\"BEFORE\\u0000AFTER\"}", "{broken", "[\"not an observation\"]", "```json\n{\"description\":\"partial\"}", "{\"description\":\"   \"}"};
    for (size_t i=0; i<sizeof(invalid)/sizeof(invalid[0]); ++i) { response(invalid[i], "stop"); rejected_frame(&frame); }
    response("partial final-channel evidence", "length"); rejected_frame(&frame);
    history = r2_log_search("vision_output_limit_reached", 10); assert(history && strstr(history, "partial final-channel evidence") && !strstr(history,"PRIVATE-VISION-REASONING")); free(history);
    response("placeholder", "stop");
    observation = json_tokener_parse(body); json_object *message = NULL;
    assert(observation && json_object_object_get_ex(observation, "message", &message));
    json_object_object_add(message, "content", json_object_new_string_len("BEFORE\0AFTER", 12));
    free(body); body = strdup(json_object_to_json_string_ext(observation, JSON_C_TO_STRING_PLAIN)); assert(body); json_object_put(observation);
    rejected_frame(&frame);
    history = r2_log_search("vision_invalid_final_text", 10);
    assert(history && strstr(history, "BEFORE\\u0000AFTER") && !strstr(history, "PRIVATE-VISION-REASONING")); free(history);
    response("placeholder", "stop"); observation = json_tokener_parse(body); assert(observation);
    json_object_object_add(observation, "done", json_object_new_boolean(0));
    free(body); body = strdup(json_object_to_json_string_ext(observation, JSON_C_TO_STRING_PLAIN)); assert(body); json_object_put(observation);
    rejected_frame(&frame);
    response("HTTP failure", "stop"); http_status = 500; rejected_frame(&frame); http_status = 200;
    response("valid prefix", "stop"); size_t body_n = strlen(body); body = realloc(body, body_n+10); assert(body); memcpy(body+body_n, " trailing", 10); rejected_frame(&frame);
    response("private server failure", "stop"); transport_code = CURLE_COULDNT_CONNECT; rejected_frame(&frame); transport_code = CURLE_OK;
    history = r2_log_search("vision_model_request_failed",10); assert(history && !strstr(history,"PRIVATE-VISION-REASONING") && !strstr(history,"private server failure")); free(history);
    transport_code = CURLE_ABORTED_BY_CALLBACK; cancel_request=1;
    assert(!call_vision_model("fixture-image", "deliberate question", R2_OLLAMA_MODEL)); assert(!gate_active);
    transport_code = CURLE_OK; cancel_request=0;

    char fenced[] = "```json\n \n{\"description\":\"text with ``` inside\",\"focus\":null}\n```\n";
    json_object *extracted_focus = NULL;
    actual=extract_visual_description(fenced, &extracted_focus); assert(actual && !strcmp(actual,"text with ``` inside")); free(actual);
    char prose[]="Complete legacy prose observation. \n\t"; actual=extract_visual_description(prose, &extracted_focus); assert(actual && !strcmp(actual,prose)); free(actual);
    assert(setenv("DISPLAY", ":fixture",1)==0);
    char label[4001]; memset(label,'L',4000); label[4000]='\0'; memcpy(label+3986,"FULL-LABEL-END",14);
    observation=json_tokener_parse("{\"description\":\"valid\",\"focus\":{\"x\":0.1,\"y\":0.2,\"width\":0.3,\"height\":0.4}}"); assert(observation);
    json_object *focus=NULL; assert(json_object_object_get_ex(observation,"focus",&focus)); json_object_object_add(focus,"label",json_object_new_string(label));
    frame.frame_number++;
    R2VisionFrame newer_frame = frame;
    newer_frame.frame_number++;
    overlapping_frame = &newer_frame;
    overlapping_response = json_object_to_json_string_ext(observation, JSON_C_TO_STRING_PLAIN);
    response("{\"description\":\"older result\",\"focus\":null}", "stop");
    actual = r2_visual_analyze_frame(&frame, "camera fixture", "older focus");
    assert(!overlapping_frame);
    assert(actual && !strcmp(actual, "older result"));
    free(actual);
    json_object_put(observation);

    /* Reuse the archived frame name to force O_EXCL failure after analysis. */
    response("{\"description\":\"archive conflict\",\"focus\":null}", "stop");
    assert(!r2_visual_analyze_frame(&frame, "camera fixture", "archive conflict"));
    frame.frame_number = newer_frame.frame_number;
    response("failed request", "length");
    rejected_frame(&frame);
    assert(sqlite3_exec(visual_db, "CREATE TEMP TRIGGER deny_overlay_insert BEFORE INSERT ON r2_visual_experiences BEGIN SELECT RAISE(ABORT, 'fixture rejection'); END", NULL, NULL, NULL) == SQLITE_OK);
    response("{\"description\":\"rejected insert\",\"focus\":null}", "stop");
    rejected_frame(&frame);
    assert(sqlite3_exec(visual_db, "DROP TRIGGER deny_overlay_insert", NULL, NULL, NULL) == SQLITE_OK);

    uint64_t focus_sequence = visual_focus_sequence;
    switch_preview_during_request = 1;
    frame.frame_number++;
    response("{\"description\":\"old camera observation\",\"focus\":null}", "stop");
    actual = r2_visual_analyze_frame(&frame, "camera fixture", "source changed during inference");
    assert(actual && !strcmp(actual, "old camera observation"));
    free(actual);
    assert(!switch_preview_during_request && visual_focus_sequence == focus_sequence);

    frame.preview_session = 0;
    frame.frame_number++;
    response("{\"description\":\"auxiliary media observation\",\"focus\":null}", "stop");
    actual = r2_visual_analyze_frame(&frame, "media fixture", "independent observation");
    assert(actual && !strcmp(actual, "auxiliary media observation"));
    free(actual);
    assert(visual_focus_sequence == focus_sequence);

    bytes = read_file(R2_VWEBCAM_DIR "/focus.json", &encoded);
    assert(bytes);
    observation = visual_parse_complete_json((char *)bytes, encoded);
    free(bytes);
    assert(observation && json_object_object_get_ex(observation, "label", &focus));
    assert(!strcmp(json_object_get_string(focus), label));
    assert(json_object_object_get_ex(observation, "preview_session", &focus));
    assert(json_object_get_uint64(focus) == fixture_preview_session - 1);
    json_object_put(observation);

    response("{\"description\":\"stale session result\",\"focus\":null}","stop"); restart_library=1;
    frame.frame_number++; assert(!r2_visual_analyze_frame(&frame,"camera fixture","old library request")); assert(scalar(visual_db,"SELECT count(*) FROM r2_visual_experiences")==0); temporary_files_gone();
    r2_visual_shutdown(); assert(r2_visual_init(database,library)==0);
    assert(scalar(visual_db,"SELECT count(*) FROM r2_visual_experiences")==5);
    assert(access(R2_VWEBCAM_DIR "/focus.json", F_OK) != 0);
    assert(unsetenv("DISPLAY")==0);
    r2_visual_shutdown(); r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    assert(r2_diary_init()==0 && r2_log_init()==0 && r2_sj_init()==0 && r2_visual_init(database,library)==0);
    history=r2_visual_search("VISUAL-COMPLETE-END",10); assert(history && strstr(history,description) && strstr(history,source)); free(history);
    verify_event_link(source, description);
    assert(!gate_active && gate_end_count==perform_count && init_count==cleanup_count);
    r2_visual_shutdown(); r2_sj_shutdown(); r2_log_shutdown(); r2_diary_shutdown();
    if (request_copy) json_object_put(request_copy);
    free(body); free(description); curl_global_cleanup();
    puts("Visual setup/response/session boundaries, complete persistence, encoder cancellation and owned-file cleanup passed with synthetic HTTP/encoder.");
    return 0;
}
