#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "Visual.h"
#include "Log.h"
#include "r2.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>

#define R2_VISION_DEFAULT_MODEL R2_OLLAMA_MODEL
#define R2_VISION_MAX_IMAGE_BYTES (20U * 1024U * 1024U)
#define R2_VISION_MAX_RESPONSE (1024U * 1024U)

static sqlite3 *visual_db = NULL;
static pthread_mutex_t visual_lock = PTHREAD_MUTEX_INITIALIZER;
static int visual_initialized = 0;
static uint64_t visual_generation = 0;
static char visual_directory[PATH_MAX];
static char visual_model[256];

/* Treat JSON construction as a transaction: never send a partial payload. */
static int visual_json_object_add_checked(json_object *object,
                                          const char *key,
                                          json_object *value)
{
    if (!object || !key || !value) {
        if (value) json_object_put(value);
        return -1;
    }
    if (json_object_object_add(object, key, value) != 0) {
        json_object_put(value);
        return -1;
    }
    return 0;
}

static int visual_json_array_add_checked(json_object *array, json_object *value)
{
    if (!array || !value) {
        if (value) json_object_put(value);
        return -1;
    }
    if (json_object_array_add(array, value) != 0) {
        json_object_put(value);
        return -1;
    }
    return 0;
}

#ifndef R2_VWEBCAM_DIR
#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"
#endif

static int visual_vwebcam_prepare_dir(void)
{
    struct stat st;
    if (mkdir(R2_VWEBCAM_DIR, 0700) != 0 && errno != EEXIST)
        return -1;
    if (lstat(R2_VWEBCAM_DIR, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid())
        return -1;
    if ((st.st_mode & 0777) != 0700 && chmod(R2_VWEBCAM_DIR, 0700) != 0)
        return -1;
    return 0;
}

/* Both sequence numbers and overlay publication are protected by visual_lock. */
static uint64_t visual_request_sequence;
static uint64_t visual_focus_sequence;

static void publish_focus_region_impl(json_object *focus, uint64_t preview_session)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    const char *display = getenv("DISPLAY");
    if (!display || !*display)
        return;
    int pn = snprintf(path, sizeof(path), "%s/focus.json", R2_VWEBCAM_DIR);
    int tn = snprintf(tmp, sizeof(tmp), "%s/focus-XXXXXX", R2_VWEBCAM_DIR);
    if (pn < 0 || (size_t)pn >= sizeof(path) ||
        tn < 0 || (size_t)tn >= sizeof(tmp))
        return;
    if (!focus || json_object_is_type(focus, json_type_null) ||
        !json_object_is_type(focus, json_type_object)) {
        (void)unlink(path);
        return;
    }

    json_object *xv = NULL, *yv = NULL, *wv = NULL, *hv = NULL, *lv = NULL;
    if (!json_object_object_get_ex(focus, "x", &xv) ||
        !json_object_object_get_ex(focus, "y", &yv) ||
        !json_object_object_get_ex(focus, "width", &wv) ||
        !json_object_object_get_ex(focus, "height", &hv)) {
        (void)unlink(path);
        return;
    }
    if ((!json_object_is_type(xv, json_type_double) && !json_object_is_type(xv, json_type_int)) ||
        (!json_object_is_type(yv, json_type_double) && !json_object_is_type(yv, json_type_int)) ||
        (!json_object_is_type(wv, json_type_double) && !json_object_is_type(wv, json_type_int)) ||
        (!json_object_is_type(hv, json_type_double) && !json_object_is_type(hv, json_type_int))) {
        (void)unlink(path);
        return;
    }
    double x = json_object_get_double(xv), y = json_object_get_double(yv);
    double w = json_object_get_double(wv), h = json_object_get_double(hv);
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h) ||
        x < 0.0 || y < 0.0 || w <= 0.0 || h <= 0.0 ||
        x > 1.0 || y > 1.0 || w > 1.0 || h > 1.0 ||
        x + w > 1.001 || y + h > 1.001 ||
        visual_vwebcam_prepare_dir() != 0) {
        (void)unlink(path);
        return;
    }
    (void)json_object_object_get_ex(focus, "label", &lv);
    const char *raw_label = lv && json_object_is_type(lv, json_type_string)
        ? json_object_get_string(lv) : "Selected region";
    const char *label = raw_label && *raw_label ? raw_label : "Selected region";
    if (lv && json_object_is_type(lv, json_type_string) &&
        strlen(raw_label) != (size_t)json_object_get_string_len(lv)) {
        (void)unlink(path); return;
    }

    json_object *root = json_object_new_object();
    if (!root)
        return;
    if (visual_json_object_add_checked(root, "x", json_object_new_double(x)) != 0 ||
        visual_json_object_add_checked(root, "y", json_object_new_double(y)) != 0 ||
        visual_json_object_add_checked(root, "width", json_object_new_double(w)) != 0 ||
        visual_json_object_add_checked(root, "height", json_object_new_double(h)) != 0 ||
        visual_json_object_add_checked(root, "label", json_object_new_string(label)) != 0 ||
        visual_json_object_add_checked(root, "preview_session", json_object_new_uint64(preview_session)) != 0) {
        json_object_put(root);
        return;
    }
    const char *encoded = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    int fd = encoded ? mkstemp(tmp) : -1;
    if (fd < 0) {
        json_object_put(root);
        return;
    }
    FILE *fp = fdopen(fd, "w");
    int failed = !fp;
    if (fp) {
        if (fputs(encoded, fp) < 0 || fputc('\n', fp) == EOF)
            failed = 1;
        if (fclose(fp) != 0)
            failed = 1;
    } else {
        close(fd);
    }
    json_object_put(root);
    if (failed || rename(tmp, path) != 0)
        (void)unlink(tmp);
}

/* Called under visual_lock after the current library accepts the row. */
static void publish_committed_focus(uint64_t request_sequence, uint64_t preview_session,
                                    json_object *focus)
{
    if (request_sequence <= visual_focus_sequence || !preview_session ||
        preview_session != r2_eyes_preview_session())
        return;
    visual_focus_sequence = request_sequence;
    publish_focus_region_impl(focus, preview_session);
}

/* Parse the entire supplied representation, never a valid-looking prefix. */
static json_object *visual_parse_complete_json(const char *text, size_t length)
{
    if (!text || length > INT_MAX || memchr(text, '\0', length)) return NULL;
    struct json_tokener *tok = json_tokener_new();
    if (!tok) return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tok, text, (int)length);
    size_t end = json_tokener_get_parse_end(tok);
    while (end < length && isspace((unsigned char)text[end])) ++end;
    if (json_tokener_get_error(tok) != json_tokener_success || end != length) {
        if (root) json_object_put(root);
        root = NULL;
    }
    json_tokener_free(tok);
    return root;
}

/* Return the description and an owned reference to its optional focus. */
static char *extract_visual_description(char *text, json_object **focus_out)
{
    *focus_out = NULL;
    if (!text) return NULL;
    while (isspace((unsigned char)*text)) ++text;
    size_t length = strlen(text);
    if (!strncmp(text, "```", 3)) {
        char *nl = strchr(text, '\n');
        size_t end = length;
        while (end && isspace((unsigned char)text[end - 1])) --end;
        if (!nl || end < 6 || memcmp(text + end - 3, "```", 3) ||
            (size_t)(nl + 1 - text) > end - 3) goto invalid;
        length = end - 3 - (size_t)(nl + 1 - text);
        text = nl + 1;
    }
    while (length && isspace((unsigned char)*text)) { ++text; --length; }
    if (!length) goto invalid;
    /* Retain complete legacy plain-prose observations. A structured response
       must validate; malformed/missing descriptions cannot become prose. */
    if (*text != '{' && *text != '[') {
        char *out = strndup(text, length);
        return out;
    }
    json_object *root = visual_parse_complete_json(text, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        goto invalid;
    }
    json_object *description = NULL, *focus = NULL;
    char *out = NULL;
    if (json_object_object_get_ex(root, "description", &description) &&
        json_object_is_type(description, json_type_string)) {
        const char *value = json_object_get_string(description);
        const char *nonspace = value;
        while (nonspace && isspace((unsigned char)*nonspace)) ++nonspace;
        if (nonspace && *nonspace && strlen(value) == (size_t)json_object_get_string_len(description))
            out = strdup(value);
    }
    if (!out) { json_object_put(root); goto invalid; }
    if (json_object_object_get_ex(root, "focus", &focus))
        *focus_out = json_object_get(focus);
    json_object_put(root);
    return out;
invalid:
    return NULL;
}

struct response_buffer {
    char *data;
    size_t length;
    size_t capacity;
};

static size_t response_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct response_buffer *b = userdata;
    if (!b || (size && nmemb > SIZE_MAX / size)) return 0;
    size_t amount = size * nmemb;
    if (b->length > R2_VISION_MAX_RESPONSE || amount > R2_VISION_MAX_RESPONSE - b->length) return 0;
    size_t needed = b->length + amount + 1;
    if (needed > b->capacity) {
        size_t cap = b->capacity ? b->capacity : 4096;
        while (cap < needed) {
            if (cap > R2_VISION_MAX_RESPONSE / 2) {
                cap = R2_VISION_MAX_RESPONSE + 1;
                break;
            }
            cap *= 2;
        }
        char *next = realloc(b->data, cap);
        if (!next) return 0;
        b->data = next;
        b->capacity = cap;
    }
    memcpy(b->data + b->length, ptr, amount);
    b->length += amount;
    b->data[b->length] = '\0';
    return amount;
}

static int visual_sync(int fd)
{
    int rc;
    do { rc = fsync(fd); } while (rc != 0 && errno == EINTR);
    return rc;
}

static int visual_sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = visual_sync(fd);
    int error = errno;
    if (close(fd) != 0 && rc == 0) return -1;
    errno = error;
    return rc;
}

static int visual_prepare_directory(const char *path)
{
    if (mkdir(path, 0750) != 0) {
        if (errno != EEXIST) return -1;
        struct stat metadata;
        if (stat(path, &metadata) != 0) return -1;
        if (S_ISDIR(metadata.st_mode)) return 0;
        errno = ENOTDIR;
        return -1;
    }

    /* A newly created directory also needs a durable entry in its parent. */
    char parent[PATH_MAX];
    size_t length = strlen(path);
    memcpy(parent, path, length + 1);
    while (length > 1 && parent[length - 1] == '/')
        parent[--length] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash == parent) slash[1] = '\0';
    else if (slash) *slash = '\0';
    if (visual_sync_directory(slash ? parent : ".") == 0) return 0;
    int error = errno;
    (void)rmdir(path);
    errno = error;
    return -1;
}

static int visual_create_archive(const char *directory, const R2VisionFrame *frame,
                                 char *path, size_t capacity)
{
    const char *separator = directory[strlen(directory) - 1] == '/' ? "" : "/";
    for (;;) {
        uint64_t nonce;
        if (getentropy(&nonce, sizeof(nonce)) != 0) return -1;
        int length = snprintf(path, capacity, "%s%svisual_%llu_%llu_%016llx.jpg",
                              directory, separator, (unsigned long long)frame->timestamp,
                              (unsigned long long)frame->frame_number, (unsigned long long)nonce);
        if (length < 0 || (size_t)length >= capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0640);
        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) return fd;
            int error = errno;
            unlink(path);
            close(fd);
            errno = error;
            return -1;
        }
        if (errno != EEXIST) return -1;
    }
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, p + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static unsigned char *read_file(const char *path, size_t *length)
{
    if (!path || !length) return NULL;
    *length = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uint64_t)st.st_size > R2_VISION_MAX_IMAGE_BYTES) { close(fd); return NULL; }
    size_t initial = (size_t)st.st_size, cap = initial, done = 0;
    unsigned char *data = malloc(cap);
    if (!data) { close(fd); return NULL; }
    unsigned char chunk[8192];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto failed;
        if (!n) break;
        size_t add = (size_t)n;
        if (add > R2_VISION_MAX_IMAGE_BYTES - done) { errno = EFBIG; goto failed; }
        if (done + add > cap) {
            size_t next = cap > R2_VISION_MAX_IMAGE_BYTES / 2 ? R2_VISION_MAX_IMAGE_BYTES : cap * 2;
            if (next < done + add) next = done + add;
            unsigned char *grown = realloc(data, next);
            if (!grown) goto failed;
            data = grown; cap = next;
        }
        memcpy(data + done, chunk, add); done += add;
    }
    if (done < initial) { errno = EIO; goto failed; }
    if (close(fd) != 0) { free(data); return NULL; }
    *length = done;
    return data;
failed:
    close(fd); free(data); return NULL;
}

static char *base64_encode(const unsigned char *data, size_t length)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (!data || !length || length > (SIZE_MAX - 4) / 4 * 3) return NULL;
    size_t outlen = ((length + 2) / 3) * 4;
    char *out = malloc(outlen + 1);
    if (!out) return NULL;
    size_t i = 0, j = 0;
    while (i < length) {
        size_t remaining = length - i;
        uint32_t a = data[i++];
        uint32_t b = remaining > 1 ? data[i++] : 0;
        uint32_t c = remaining > 2 ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = alphabet[(triple >> 18) & 63];
        out[j++] = alphabet[(triple >> 12) & 63];
        out[j++] = remaining > 1 ? alphabet[(triple >> 6) & 63] : '=';
        out[j++] = remaining > 2 ? alphabet[triple & 63] : '=';
    }
    out[outlen] = '\0';
    return out;
}

/* Encoding is a child we own. Poll so source shutdown/foreground work can
 * cancel it, and bound a stuck encoder independently of model inference. */
static int visual_wait_encoder(pid_t pid)
{
    struct timespec start, now;
    int status = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) goto terminate;
    for (;;) {
        pid_t rc = waitpid(pid, &status, WNOHANG);
        if (rc == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (rc < 0 && errno != EINTR) return -1;
        if (r2_ollama_vision_request_should_abort()) { errno = ECANCELED; goto terminate; }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) goto terminate;
        time_t elapsed = now.tv_sec - start.tv_sec;
        if (elapsed > 30 || (elapsed == 30 && now.tv_nsec >= start.tv_nsec)) {
            errno = ETIMEDOUT; goto terminate;
        }
        struct timespec pause = {0, 25000000};
        (void)nanosleep(&pause, NULL);
    }
terminate:
    ;
    int saved_errno = errno;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    errno = saved_errno;
    return -1;
}

static unsigned char *encode_jpeg(const R2VisionFrame *frame, size_t *jpeg_length)
{
    if (!frame || !frame->data || !jpeg_length ||
        !frame->format.width || !frame->format.height ||
        frame->format.pixel_format != R2_PIXEL_RGB24)
        return NULL;

    uint64_t width = (uint64_t)frame->format.width;
    uint64_t height = (uint64_t)frame->format.height;
    if (height == 0 || width > UINT64_MAX / height / 3U)
        return NULL;
    uint64_t expected = width * height * 3U;
    if (expected != frame->size || expected > 100U * 1024U * 1024U)
        return NULL;

    char ppm_template[] = "/tmp/r2-visual-XXXXXX.ppm";
    char jpg_template[] = "/tmp/r2-visual-XXXXXX.jpg";
    int ppm_fd = mkstemps(ppm_template, 4);
    if (ppm_fd < 0) return NULL;
    int jpg_fd = mkstemps(jpg_template, 4);
    if (jpg_fd < 0) { close(ppm_fd); unlink(ppm_template); return NULL; }
    if (close(jpg_fd) != 0) {
        close(ppm_fd);
        unlink(ppm_template);
        unlink(jpg_template);
        return NULL;
    }
    /* Keep the unique file created by mkstemps; ffmpeg's -y overwrites it. */
    
    char header[128];
    int header_len = snprintf(header, sizeof(header), "P6\n%u %u\n255\n",
                              frame->format.width, frame->format.height);
    int ok = header_len > 0 && (size_t)header_len < sizeof(header) &&
             write_all(ppm_fd, header, (size_t)header_len) == 0 &&
             write_all(ppm_fd, frame->data, frame->size) == 0;
    if (close(ppm_fd) != 0) ok = 0;
    if (!ok) { unlink(ppm_template); unlink(jpg_template); return NULL; }

    pid_t pid = fork();
    if (pid == 0) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) { dup2(nullfd, STDERR_FILENO); close(nullfd); }
        execlp("ffmpeg", "ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
               "-y", "-i", ppm_template, "-frames:v", "1", "-q:v", "5",
               "-f", "image2", jpg_template, (char *)NULL);
        _exit(127);
    }

    int encoded = pid > 0 ? visual_wait_encoder(pid) : -1;
    int encoder_error = errno;
    unlink(ppm_template);
    if (encoded != 0) {
        unlink(jpg_template);
        errno = encoder_error;
        return NULL;
    }

    unsigned char *jpeg = read_file(jpg_template, jpeg_length);
    unlink(jpg_template);
    return jpeg;
}

char *r2_visual_frame_base64(const R2VisionFrame *frame)
{
    size_t jpeg_length = 0;
    unsigned char *jpeg = encode_jpeg(frame, &jpeg_length);
    if (!jpeg)
        return NULL;
    char *encoded = base64_encode(jpeg, jpeg_length);
    free(jpeg);
    return encoded;
}

static int vision_progress(void *userdata,
                            curl_off_t download_total,
                            curl_off_t download_now,
                            curl_off_t upload_total,
                            curl_off_t upload_now)
{
    (void)userdata;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    return r2_ollama_vision_request_should_abort() ? 1 : 0;
}

static char *call_vision_model(const char *image_base64, const char *question,
                               const char *model)
{
    if (!image_base64 || !model || !*model) return NULL;
    const char *asked = question && *question ? question :
        "Describe what is visibly present. Separate direct observations from uncertain interpretations. "
        "Read visible text when possible, identify objects and spatial relationships, and avoid guessing "
        "identity, intent, or emotion without visible evidence.";

    char *system_prompt = strdup(
        "You are the visual perception system of R2-3PO, a local robot. "
        "Analyze only the supplied image. Be concrete and evidence-based. "
        "Distinguish what is directly visible from inference and uncertainty. "
        "Do not claim motion or events from a single frame. Do not identify private people. "
        "Return ONLY valid JSON with keys description and focus. description is a concise natural-language " 
        "observation for R2's conversation to use as sensory evidence, not as a user-facing answer. focus is " 
        "null or an object with label, x, y, width, and height. Coordinates are normalized 0..1 fractions " 
        "of the full image; x/y are the top-left corner and width/height are the region size. Select one " 
        "visible region worth inspecting. Never invent a region; use null if no clear target exists. Do not " 
        "wrap JSON in markdown."
    );
    json_object *root = json_object_new_object();
    json_object *messages = json_object_new_array();
    json_object *sys = json_object_new_object();
    json_object *user = json_object_new_object();
    json_object *images = json_object_new_array();
    json_object *options = json_object_new_object();
    if (!root || !messages || !sys || !user || !images || !options || !system_prompt)
        goto build_failure;
    if (visual_json_object_add_checked(root, "model", json_object_new_string(model)) != 0 ||
        visual_json_object_add_checked(root, "stream", json_object_new_boolean(0)) != 0 ||
        visual_json_object_add_checked(root, "truncate", json_object_new_boolean(0)) != 0 ||
        visual_json_object_add_checked(root, "shift", json_object_new_boolean(0)) != 0 ||
        visual_json_object_add_checked(root, "think", json_object_new_boolean(0)) != 0 ||
        visual_json_object_add_checked(root, "format", json_object_new_string("json")) != 0 ||
        visual_json_object_add_checked(root, "keep_alive", json_object_new_string("10m")) != 0)
        goto build_failure;

    /*
     * Share the RAM-aware context ceiling with chat. Vision only needs a concise
     * observation, so cap generated output rather than letting the model's
     * default context/output settings compete unpredictably with conversation.
     */
    int num_ctx = r2_ollama_context_tokens();
    int num_predict = num_ctx >= 8192 ? 512 : 256;
    if (visual_json_object_add_checked(options, "num_ctx", json_object_new_int(num_ctx)) != 0 ||
        visual_json_object_add_checked(options, "num_predict", json_object_new_int(num_predict)) != 0 ||
        visual_json_object_add_checked(options, "temperature", json_object_new_double(0.7)) != 0 ||
        visual_json_object_add_checked(options, "top_p", json_object_new_double(0.8)) != 0 ||
        visual_json_object_add_checked(options, "top_k", json_object_new_int(20)) != 0 ||
        visual_json_object_add_checked(options, "min_p", json_object_new_double(0.0)) != 0 ||
        visual_json_object_add_checked(options, "presence_penalty", json_object_new_double(1.5)) != 0 ||
        visual_json_object_add_checked(options, "repeat_penalty", json_object_new_double(1.0)) != 0)
        goto build_failure;

    /* Keep vision inference on the same requested GPU/CPU layer policy as chat. */
    const char *gpu_layers_env = getenv("R2_OLLAMA_NUM_GPU");
    if (gpu_layers_env && *gpu_layers_env) {
        errno = 0;
        char *gpu_end = NULL;
        long gpu_layers = strtol(gpu_layers_env, &gpu_end, 10);
        if (!errno && gpu_end != gpu_layers_env && *gpu_end == '\0' &&
            gpu_layers >= -1 && gpu_layers <= 999 && gpu_layers != 0) {
            if (visual_json_object_add_checked(
                    options, "num_gpu", json_object_new_int((int)gpu_layers)) != 0)
                goto build_failure;
        } else if (errno || gpu_end == gpu_layers_env || *gpu_end != '\0' ||
                   gpu_layers < -1 || gpu_layers > 999) {
            fprintf(stderr, "[R2 Vision] Ignoring invalid R2_OLLAMA_NUM_GPU value.\n");
        }
    }
    int add_rc = visual_json_object_add_checked(root, "options", options);
    options = NULL; /* checked helper consumes the value on success or failure */
    if (add_rc != 0)
        goto build_failure;
    if (visual_json_object_add_checked(sys, "role", json_object_new_string("system")) != 0 ||
        visual_json_object_add_checked(sys, "content", json_object_new_string(system_prompt)) != 0 ||
        visual_json_object_add_checked(user, "role", json_object_new_string("user")) != 0 ||
        visual_json_object_add_checked(user, "content", json_object_new_string(asked)) != 0 ||
        visual_json_array_add_checked(images, json_object_new_string(image_base64)) != 0)
        goto build_failure;
    add_rc = visual_json_object_add_checked(user, "images", images);
    images = NULL;
    if (add_rc != 0)
        goto build_failure;
    add_rc = visual_json_array_add_checked(messages, sys);
    sys = NULL;
    if (add_rc != 0)
        goto build_failure;
    add_rc = visual_json_array_add_checked(messages, user);
    user = NULL;
    if (add_rc != 0)
        goto build_failure;
    add_rc = visual_json_object_add_checked(root, "messages", messages);
    messages = NULL;
    if (add_rc != 0)
        goto build_failure;
    free(system_prompt);
    system_prompt = NULL;
    goto payload_ready;

build_failure:
    if (root) json_object_put(root);
    if (messages) json_object_put(messages);
    if (sys) json_object_put(sys);
    if (user) json_object_put(user);
    if (images) json_object_put(images);
    if (options) json_object_put(options);
    free(system_prompt);
    return NULL;

payload_ready:
    ;

    const char *payload = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
    char ollama_url[128];
    if (!payload || r2_ollama_chat_url(ollama_url, sizeof(ollama_url)) != 0) {
        json_object_put(root);
        return NULL;
    }
    CURL *curl = curl_easy_init();
    struct response_buffer response = {0};
    struct curl_slist *headers = NULL;
    char *answer = NULL;
    int gate_active = 0;
    if (!curl) {
        r2_log_sensory("vision_request_setup_failed", "Vision HTTP handle could not be allocated; no request sent.", NULL, model);
        goto request_cleanup;
    }
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) {
        r2_log_sensory("vision_request_setup_failed", "Vision HTTP headers could not be allocated; no request sent.", NULL, model);
        goto request_cleanup;
    }
#define R2_VISION_SETOPT(option, value) \
    do { \
        CURLcode setup_rc = curl_easy_setopt(curl, (option), (value)); \
        if (setup_rc != CURLE_OK) { \
            char failure[160]; \
            snprintf(failure, sizeof(failure), "option=%d; curl=%s; no request sent", \
                     (int)(option), curl_easy_strerror(setup_rc)); \
            r2_log_sensory("vision_request_setup_failed", "R2 could not configure visual inference.", failure, model); \
            goto request_cleanup; \
        } \
    } while (0)
    R2_VISION_SETOPT(CURLOPT_URL, ollama_url);
    R2_VISION_SETOPT(CURLOPT_HTTPHEADER, headers);
    R2_VISION_SETOPT(CURLOPT_POST, 1L);
    R2_VISION_SETOPT(CURLOPT_POSTFIELDS, payload);
    R2_VISION_SETOPT(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)strlen(payload));
    R2_VISION_SETOPT(CURLOPT_WRITEFUNCTION, response_write);
    R2_VISION_SETOPT(CURLOPT_WRITEDATA, &response);
    R2_VISION_SETOPT(CURLOPT_CONNECTTIMEOUT, 5L);
    R2_VISION_SETOPT(CURLOPT_TIMEOUT, 180L);
    R2_VISION_SETOPT(CURLOPT_NOSIGNAL, 1L);
    R2_VISION_SETOPT(CURLOPT_NOPROGRESS, 0L);
    R2_VISION_SETOPT(CURLOPT_XFERINFOFUNCTION, vision_progress);
#undef R2_VISION_SETOPT
    if (!r2_ollama_vision_request_begin()) {
        r2_log_sensory("vision_request_deferred",
                       "R2 deferred visual inference because the shared Ollama model is busy or a foreground turn is waiting.",
                       "The captured frame remains unreported rather than blocking ordinary conversation.", model);
        goto request_cleanup;
    }
    gate_active = 1;
    if (r2_ollama_vision_request_should_abort()) goto request_cleanup;
    CURLcode cc = curl_easy_perform(curl);
    int interrupted = cc == CURLE_ABORTED_BY_CALLBACK || r2_ollama_vision_request_should_abort();
    r2_ollama_vision_request_end(); gate_active = 0;
    if (interrupted) {
        r2_log_sensory("vision_request_yielded", "R2's visual inference was cancelled.",
                       "The response was discarded before any observation was published.", model);
        goto request_cleanup;
    }
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    if (cc == CURLE_OK && http_status >= 200 && http_status < 300 && response.data) {
        json_object *parsed = visual_parse_complete_json(response.data, response.length);
        json_object *message = NULL, *content = NULL, *role = NULL, *done = NULL, *reason = NULL;
        int complete_assistant_turn = parsed && json_object_is_type(parsed, json_type_object) &&
            json_object_object_get_ex(parsed, "done", &done) && done &&
            json_object_is_type(done, json_type_boolean) && json_object_get_boolean(done) &&
            json_object_object_get_ex(parsed, "message", &message) && message &&
            json_object_is_type(message, json_type_object) &&
            json_object_object_get_ex(message, "role", &role) && role &&
            json_object_is_type(role, json_type_string) && json_object_get_string_len(role) == 9 &&
            strcmp(json_object_get_string(role), "assistant") == 0;
        int has_reason = parsed && json_object_object_get_ex(parsed, "done_reason", &reason);
        if (has_reason && (!json_object_is_type(reason, json_type_string) ||
            strlen(json_object_get_string(reason)) != (size_t)json_object_get_string_len(reason)))
            complete_assistant_turn = 0;
        if (complete_assistant_turn && json_object_object_get_ex(message, "content", &content) &&
            content && json_object_is_type(content, json_type_string)) {
            const char *text = json_object_get_string(content);
            if (text && strlen(text) != (size_t)json_object_get_string_len(content)) {
                r2_log_sensory("vision_invalid_final_text", "Vision final text contained NUL; escaped evidence retained, not an observation.",
                               json_object_to_json_string_ext(content, JSON_C_TO_STRING_PLAIN), model);
            } else if (has_reason && !strcmp(json_object_get_string(reason), "length")) {
                r2_log_sensory("vision_output_limit_reached", "Vision generation stopped at its limit; partial evidence is not a visual observation.",
                               text && *text ? text : "No final-channel text was returned; private reasoning was not archived.", model);
            } else if (text && *text) answer = strdup(text);
        } else {
            r2_log_sensory("vision_invalid_completion_contract",
                           "R2 rejected an incomplete or invalid vision-model response.",
                           "Expected complete JSON, message.role=assistant, string content and boolean done=true.", model);
        }
        if (parsed) json_object_put(parsed);
    } else {
        char failure[1200];
        snprintf(failure, sizeof(failure), "curl=%s; HTTP=%ld; response_bytes=%zu; model=%s",
                 curl_easy_strerror(cc), http_status, response.length, model);
        /* Raw failed responses can contain private reasoning: retain failure
           metadata here, not an unvalidated response body. */
        r2_log_sensory("vision_model_request_failed", "R2's vision model request failed.", failure, model);
    }
request_cleanup:
    if (gate_active) r2_ollama_vision_request_end();
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    free(response.data); json_object_put(root);
    return answer;
}

static int visual_column_exists(sqlite3 *db, const char *column)
{
    sqlite3_stmt *st = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(r2_visual_experiences)",
                           -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *name = sqlite3_column_text(st, 1);
            if (name && !strcasecmp((const char *)name, column)) {
                found = 1;
                break;
            }
        }
    }
    if (st) sqlite3_finalize(st);
    return found;
}

static int visual_ensure_column(const char *column, const char *definition)
{
    if (visual_column_exists(visual_db, column)) return SQLITE_OK;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
                     "ALTER TABLE r2_visual_experiences ADD COLUMN %s %s;",
                     column, definition);
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    return sqlite3_exec(visual_db, sql, NULL, NULL, NULL);
}

static int visual_backfill(const char *target, const char *source)
{
    if (!visual_column_exists(visual_db, source)) return SQLITE_OK;
    char sql[512];
    int n = snprintf(sql, sizeof(sql),
        "UPDATE r2_visual_experiences SET %s=%s WHERE (%s IS NULL OR %s='') "
        "AND %s IS NOT NULL AND %s<>'';",
        target, source, target, target, source, source);
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    return sqlite3_exec(visual_db, sql, NULL, NULL, NULL);
}

static int visual_migrate_schema(void)
{
    const char *columns[][2] = {
        {"created_utc", "TEXT NOT NULL DEFAULT ''"},
        {"source", "TEXT NOT NULL DEFAULT 'unknown'"},
        {"image_path", "TEXT NOT NULL DEFAULT ''"},
        {"model", "TEXT NOT NULL DEFAULT 'unknown'"},
        {"prompt", "TEXT"},
        {"description", "TEXT NOT NULL DEFAULT ''"},
        {"width", "INTEGER NOT NULL DEFAULT 0"},
        {"height", "INTEGER NOT NULL DEFAULT 0"},
        {"frame_number", "INTEGER NOT NULL DEFAULT 0"},
        {"frame_timestamp", "INTEGER NOT NULL DEFAULT 0"},
        {"life_log_event_id", "INTEGER"},
        {"life_log_archive_key", "TEXT"}
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); ++i) {
        if (visual_ensure_column(columns[i][0], columns[i][1]) != SQLITE_OK)
            return SQLITE_ERROR;
    }

    /* Copy older names into canonical fields only when the new field is empty. */
    if (visual_backfill("created_utc", "created_at") != SQLITE_OK ||
        visual_backfill("created_utc", "timestamp") != SQLITE_OK ||
        visual_backfill("image_path", "file_path") != SQLITE_OK ||
        visual_backfill("image_path", "path") != SQLITE_OK)
        return SQLITE_ERROR;

    if (sqlite3_exec(visual_db,
        "UPDATE r2_visual_experiences SET created_utc=CURRENT_TIMESTAMP "
        "WHERE created_utc IS NULL OR created_utc='';",
        NULL, NULL, NULL) != SQLITE_OK)
        return SQLITE_ERROR;
    return sqlite3_exec(visual_db,
        "CREATE INDEX IF NOT EXISTS r2_visual_experiences_created_idx "
        "ON r2_visual_experiences(created_utc);"
        "CREATE INDEX IF NOT EXISTS r2_visual_pending_log_idx ON r2_visual_experiences(id) "
        "WHERE life_log_archive_key IS NOT NULL AND life_log_event_id IS NULL;",
        NULL, NULL, NULL);
}


/* Old visual-library schemas may retain required created_at/timestamp and
   file_path/path columns after the canonical columns are added. Populate
   those aliases on every insert so an upgrade remains writable for old DBs. */
static int visual_insert_compat(sqlite3 *db, const char *created, const char *source,
                                const char *image_path, const char *model,
                                const char *prompt, const char *description,
                                int width, int height, sqlite3_int64 frame_number,
                                sqlite3_int64 frame_timestamp, int64_t event_id,
                                const char *archive_key)
{
    enum { VISUAL_TEXT = 1, VISUAL_INTEGER = 2, VISUAL_NULL = 3 };
    struct visual_value {
        const char *column;
        int type;
        const char *text;
        sqlite3_int64 integer;
    } values[20];
    size_t count = 0;
#define ADD_TEXT(column_name, value) do { \
    values[count++] = (struct visual_value){(column_name), VISUAL_TEXT, (value), 0}; \
} while (0)
#define ADD_INTEGER(column_name, value) do { \
    values[count++] = (struct visual_value){(column_name), VISUAL_INTEGER, NULL, (sqlite3_int64)(value)}; \
} while (0)
#define ADD_NULL(column_name) do { \
    values[count++] = (struct visual_value){(column_name), VISUAL_NULL, NULL, 0}; \
} while (0)

    ADD_TEXT("created_utc", created);
    ADD_TEXT("source", source ? source : "Eyes");
    ADD_TEXT("image_path", image_path);
    ADD_TEXT("model", model);
    if (prompt && *prompt) ADD_TEXT("prompt", prompt);
    else ADD_NULL("prompt");
    ADD_TEXT("description", description);
    ADD_INTEGER("width", width);
    ADD_INTEGER("height", height);
    ADD_INTEGER("frame_number", frame_number);
    ADD_INTEGER("frame_timestamp", frame_timestamp);
    if (event_id > 0) ADD_INTEGER("life_log_event_id", event_id);
    else ADD_NULL("life_log_event_id");
    if (archive_key) ADD_TEXT("life_log_archive_key", archive_key);

    if (visual_column_exists(db, "created_at")) ADD_TEXT("created_at", created);
    if (visual_column_exists(db, "timestamp")) ADD_TEXT("timestamp", created);
    if (visual_column_exists(db, "file_path")) ADD_TEXT("file_path", image_path);
    if (visual_column_exists(db, "path")) ADD_TEXT("path", image_path);
#undef ADD_TEXT
#undef ADD_INTEGER
#undef ADD_NULL

    char sql[2048];
    size_t used = 0;
    int n = snprintf(sql, sizeof(sql), "INSERT INTO r2_visual_experiences (");
    if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_TOOBIG;
    used = (size_t)n;
    for (size_t i = 0; i < count; ++i) {
        n = snprintf(sql + used, sizeof(sql) - used, "%s%s",
                     i ? "," : "", values[i].column);
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    n = snprintf(sql + used, sizeof(sql) - used, ") VALUES(");
    if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
    used += (size_t)n;
    for (size_t i = 0; i < count; ++i) {
        n = snprintf(sql + used, sizeof(sql) - used, "%s?",
                     i ? "," : "");
        if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;
        used += (size_t)n;
    }
    n = snprintf(sql + used, sizeof(sql) - used, ")");
    if (n < 0 || (size_t)n >= sizeof(sql) - used) return SQLITE_TOOBIG;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) return rc;
    for (size_t i = 0; i < count && rc == SQLITE_OK; ++i) {
        if (values[i].type == VISUAL_TEXT)
            rc = sqlite3_bind_text64(st, (int)i + 1, values[i].text,
                                     strlen(values[i].text), SQLITE_TRANSIENT, SQLITE_UTF8);
        else if (values[i].type == VISUAL_INTEGER)
            rc = sqlite3_bind_int64(st, (int)i + 1, values[i].integer);
        else
            rc = sqlite3_bind_null(st, (int)i + 1);
    }
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1)
        rc = SQLITE_CONSTRAINT;
    sqlite3_finalize(st);
    return rc;
}

typedef struct {
    const R2VisionFrame *frame;
    const char *created;
    const char *source;
    const char *image_path;
    const char *model;
    const char *question;
    const char *description;
    int recovering;
    int cancelled;
} VisualExperience;

static const char *visual_experience_summary =
    "R2's vision model interpreted a real captured frame.";

static int visual_drop_pending(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db,
        "DELETE FROM r2_visual_pending_archives WHERE image_path=?", -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
    sqlite3_finalize(st);
    return rc;
}

static int visual_finish_transaction(sqlite3 *db, int rc)
{
    if (rc == SQLITE_DONE) {
        int commit = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        if (commit == SQLITE_OK) return SQLITE_DONE;
        rc = commit;
    }
    if (!sqlite3_get_autocommit(db))
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    return rc;
}

static int visual_stage_archive(sqlite3 *db, const VisualExperience *experience,
                                const unsigned char *jpeg, size_t length)
{
    const R2VisionFrame *frame = experience->frame;
    const char *fields[] = {experience->image_path, experience->created,
        experience->source, experience->model, experience->question, experience->description};
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO r2_visual_pending_archives(image_path,created_utc,source,model,prompt,"
        "description,width,height,frame_number,frame_timestamp,jpeg) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
        -1, &st, NULL);
    for (int i = 0; i < 6 && rc == SQLITE_OK; ++i) {
        rc = fields[i] ? sqlite3_bind_text64(st, i + 1, fields[i], strlen(fields[i]),
                                            SQLITE_STATIC, SQLITE_UTF8)
                       : sqlite3_bind_null(st, i + 1);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 7, frame->format.width);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 8, frame->format.height);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 9, (sqlite3_int64)frame->frame_number);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 10, (sqlite3_int64)frame->timestamp);
    if (rc == SQLITE_OK) rc = sqlite3_bind_blob64(st, 11, jpeg, length, SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_DONE && sqlite3_changes(db) != 1) rc = SQLITE_CONSTRAINT;
    sqlite3_finalize(st);
    return visual_finish_transaction(db, rc);
}

/* Each file-backed request keeps its original connection across a Visual
 * close/reopen. A private in-memory database has no persistent cleanup backlog. */
static sqlite3 *visual_open_archive_db(int *owned)
{
    *owned = 0;
    const char *path = sqlite3_db_filename(visual_db, "main");
    if (!path || !*path) return visual_db;
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_busy_timeout(db, 5000);
    if (rc == SQLITE_OK) rc = sqlite3_exec(db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    *owned = 1;
    return db;
}

static int visual_withdraw_archive(sqlite3 *db, const char *path)
{
    int rc = sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    return visual_finish_transaction(db, visual_drop_pending(db, path));
}

static int visual_save_experience(sqlite3 *db, int64_t event_id, void *context)
{
    VisualExperience *experience = context;
    const R2VisionFrame *frame = experience->frame;
    if (!experience->recovering && r2_ollama_vision_request_should_abort()) {
        experience->cancelled = 1;
        return SQLITE_ABORT;
    }
    int rc = visual_insert_compat(db, experience->created, experience->source,
                                experience->image_path, experience->model,
                                experience->question, experience->description,
                                (int)frame->format.width, (int)frame->format.height,
                                (sqlite3_int64)frame->frame_number,
                                (sqlite3_int64)frame->timestamp, event_id,
                                event_id > 0 ? NULL : experience->image_path);
    if (rc == SQLITE_DONE && !experience->recovering && r2_ollama_vision_request_should_abort()) {
        experience->cancelled = 1;
        return SQLITE_ABORT;
    }
    return rc == SQLITE_DONE ? visual_drop_pending(db, experience->image_path) : rc;
}

static int visual_link_detached(sqlite3_int64 row_id, const char *key,
                                const char *description, const char *source)
{
    int64_t event_id = r2_log_save_visual_once(key, description, source);
    if (event_id <= 0) return -1;

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(visual_db,
        "UPDATE r2_visual_experiences SET life_log_event_id=? WHERE id=? "
        "AND life_log_archive_key=? AND life_log_event_id IS NULL",
        -1, &st, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, event_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, row_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 3, key, -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int linked = rc == SQLITE_DONE && sqlite3_changes(visual_db) == 1;
    sqlite3_finalize(st);
    return linked ? 0 : -1;
}

/* Save the replay identity in the same transaction as the authoritative row. */
static int visual_save_separate(VisualExperience *experience)
{
    int rc = sqlite3_exec(visual_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = visual_save_experience(visual_db, 0, experience);
    sqlite3_int64 row_id = sqlite3_last_insert_rowid(visual_db);
    rc = visual_finish_transaction(visual_db, rc);
    if (rc != SQLITE_DONE) return rc;

    if (visual_link_detached(row_id, experience->image_path, experience->description,
                             experience->source) != 0)
        fprintf(stderr, "[R2 Visual] Life Log link for row %lld is pending.\n", (long long)row_id);
    return SQLITE_DONE;
}

/* Use one publication path for live observations and verified restart replay. */
static int visual_publish_archive(VisualExperience *experience)
{
    int shared = r2_log_save_shared_sensory(visual_db, "visual_experience",
                     visual_experience_summary, experience->description,
                     experience->source, visual_save_experience, experience);
    return shared == 1 ? visual_save_separate(experience)
           : shared == 0 ? SQLITE_DONE : SQLITE_ERROR;
}

static int visual_managed_archive_path(const char *path, const R2VisionFrame *frame)
{
    if (!path) return 0;
    char prefix[PATH_MAX];
    size_t directory_length = strlen(visual_directory);
    const char *separator = visual_directory[directory_length - 1] == '/' ? "" : "/";
    int length = snprintf(prefix, sizeof(prefix), "%s%svisual_%llu_%llu_",
                          visual_directory, separator,
                          (unsigned long long)frame->timestamp,
                          (unsigned long long)frame->frame_number);
    if (length < 0 || (size_t)length >= sizeof(prefix) ||
        strncmp(path, prefix, (size_t)length) != 0)
        return 0;
    const char *suffix = path + length;
    return strspn(suffix, "0123456789abcdef") == 16 && !strcmp(suffix + 16, ".jpg");
}

/* A pending row is evidence only when the complete, locked JPEG agrees. */
static int visual_verify_pending_image(const char *path, const unsigned char *jpeg, size_t length)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    int valid = flock(fd, LOCK_EX | LOCK_NB) == 0 && fstat(fd, &st) == 0 &&
        S_ISREG(st.st_mode) && st.st_size == (off_t)length;
    for (size_t offset = 0; valid && offset < length;) {
        unsigned char buffer[8192];
        size_t chunk = length - offset < sizeof(buffer) ? length - offset : sizeof(buffer);
        ssize_t read_size = pread(fd, buffer, chunk, (off_t)offset);
        if (read_size < 0 && errno == EINTR) continue;
        valid = read_size > 0 && !memcmp(buffer, jpeg + offset, (size_t)read_size);
        if (valid) offset += (size_t)read_size;
    }
    if (valid && visual_sync(fd) == 0 && visual_sync_directory(visual_directory) == 0) {
        struct stat current;
        if (lstat(path, &current) == 0 && current.st_dev == st.st_dev && current.st_ino == st.st_ino &&
            current.st_size == st.st_size)
            return fd;
    }
    close(fd);
    return -1;
}

static void visual_retry_pending_archives(void)
{
    sqlite3_int64 after = 0;
    int pending = 0;
    for (;;) {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(visual_db,
            "SELECT id,image_path,created_utc,source,model,prompt,description,width,height,"
            "frame_number,frame_timestamp,jpeg FROM r2_visual_pending_archives "
            "WHERE id>? ORDER BY id LIMIT 1", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, after);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            if (rc != SQLITE_DONE) pending = 1;
            sqlite3_finalize(st);
            break;
        }
        after = sqlite3_column_int64(st, 0);
        char *fields[6] = {0};
        int valid = 1;
        for (int i = 0; i < 6; ++i) {
            if (i == 4 && sqlite3_column_type(st, i + 1) == SQLITE_NULL) continue;
            const char *value = (const char *)sqlite3_column_text(st, i + 1);
            if (value && strlen(value) == (size_t)sqlite3_column_bytes(st, i + 1))
                fields[i] = strdup(value);
            if (!fields[i]) valid = 0;
        }
        sqlite3_int64 width = sqlite3_column_int64(st, 7);
        sqlite3_int64 height = sqlite3_column_int64(st, 8);
        R2VisionFrame frame = {0};
        if (width <= 0 || width > INT_MAX || height <= 0 || height > INT_MAX) valid = 0;
        frame.format.width = (unsigned int)width;
        frame.format.height = (unsigned int)height;
        frame.frame_number = (uint64_t)sqlite3_column_int64(st, 9);
        frame.timestamp = (uint64_t)sqlite3_column_int64(st, 10);
        const void *blob = sqlite3_column_blob(st, 11);
        int length = sqlite3_column_bytes(st, 11);
        int fd = -1;
        if (valid && blob && length > 0 && (unsigned int)length <= R2_VISION_MAX_IMAGE_BYTES &&
            visual_managed_archive_path(fields[0], &frame))
            fd = visual_verify_pending_image(fields[0], blob, (size_t)length);
        /* Release SQLite's read statement before taking the publication transaction. */
        sqlite3_finalize(st);
        if (fd >= 0) {
            VisualExperience experience = {&frame, fields[1], fields[2], fields[0],
                                            fields[3], fields[4], fields[5], 1, 0};
            if (visual_publish_archive(&experience) != SQLITE_DONE) pending = 1;
            close(fd);
        } else {
            pending = 1;
        }
        for (int i = 0; i < 6; ++i) free(fields[i]);
    }
    if (pending)
        fprintf(stderr, "[R2 Visual] Some archived observations remain pending.\n");
}

static void visual_retry_detached_links(void)
{
    if (!r2_log_is_initialized()) return;
    sqlite3_int64 after = 0;
    int pending = 0;
    for (;;) {
        sqlite3_stmt *st = NULL;
        int rc = sqlite3_prepare_v2(visual_db,
            "SELECT id,life_log_archive_key,description,source FROM r2_visual_experiences "
            "WHERE id>? AND life_log_archive_key IS NOT NULL AND life_log_event_id IS NULL "
            "ORDER BY id LIMIT 1", -1, &st, NULL);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 1, after);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) {
            if (rc != SQLITE_DONE) pending = 1;
            sqlite3_finalize(st);
            break;
        }

        after = sqlite3_column_int64(st, 0);
        char *fields[3] = {0};
        for (int i = 0; i < 3; ++i) {
            const char *value = (const char *)sqlite3_column_text(st, i + 1);
            if (value && strlen(value) == (size_t)sqlite3_column_bytes(st, i + 1))
                fields[i] = strdup(value);
        }
        /* End the read before Life Log or Visual opens a write transaction. */
        sqlite3_finalize(st);
        if (!fields[0] || !fields[1] || !fields[2] ||
            visual_link_detached(after, fields[0], fields[1], fields[2]) != 0)
            pending = 1;
        for (int i = 0; i < 3; ++i) free(fields[i]);
    }
    if (pending)
        fprintf(stderr, "[R2 Visual] Some detached Life Log links remain pending.\n");
}

int r2_visual_init(const char *database_path, const char *library_directory)
{
    if (!database_path || !*database_path || !library_directory ||
        !*library_directory || strlen(library_directory) >= sizeof(visual_directory))
        return -1;
    pthread_mutex_lock(&visual_lock);
    if (visual_initialized) { pthread_mutex_unlock(&visual_lock); return 0; }
    if (visual_generation == UINT64_MAX) { pthread_mutex_unlock(&visual_lock); errno = EOVERFLOW; return -1; }
    snprintf(visual_directory, sizeof(visual_directory), "%s", library_directory);
    const char *configured = getenv("R2_VISION_MODEL");
    if (configured && *configured &&
        strcmp(configured, R2_VISION_DEFAULT_MODEL) != 0) {
        fprintf(stderr,
                "[R2 Vision] Ignoring R2_VISION_MODEL=%s; this build uses the unified model %s.\n",
                configured, R2_VISION_DEFAULT_MODEL);
    }
    snprintf(visual_model, sizeof(visual_model), "%s", R2_VISION_DEFAULT_MODEL);
    if (visual_prepare_directory(visual_directory) != 0) {
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    if (!realpath(library_directory, visual_directory)) {
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    if (sqlite3_open(database_path, &visual_db) != SQLITE_OK) {
        if (visual_db) sqlite3_close(visual_db);
        visual_db = NULL;
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS r2_visual_experiences ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_utc TEXT NOT NULL,"
        "source TEXT NOT NULL,"
        "image_path TEXT NOT NULL,"
        "model TEXT NOT NULL,"
        "prompt TEXT,"
        "description TEXT NOT NULL,"
        "width INTEGER NOT NULL,"
        "height INTEGER NOT NULL,"
        "frame_number INTEGER NOT NULL,"
        "frame_timestamp INTEGER NOT NULL,"
        "life_log_event_id INTEGER,"
        "UNIQUE(image_path));"
        "CREATE TABLE IF NOT EXISTS r2_visual_pending_archives ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,image_path TEXT NOT NULL UNIQUE,"
        "created_utc TEXT NOT NULL,source TEXT NOT NULL,model TEXT NOT NULL,"
        "prompt TEXT,description TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,"
        "frame_number INTEGER NOT NULL,frame_timestamp INTEGER NOT NULL,jpeg BLOB NOT NULL);";
    int rc = sqlite3_busy_timeout(visual_db, 5000);
    if (rc == SQLITE_OK) rc = sqlite3_exec(visual_db, "PRAGMA synchronous=FULL", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_exec(visual_db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_exec(visual_db, schema, NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = visual_migrate_schema();
    if (rc == SQLITE_OK) rc = sqlite3_exec(visual_db, "COMMIT", NULL, NULL, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[R2 Visual] Could not initialize the visual database schema: %s\n",
                sqlite3_errmsg(visual_db));
        if (!sqlite3_get_autocommit(visual_db))
            (void)sqlite3_exec(visual_db, "ROLLBACK", NULL, NULL, NULL);
        sqlite3_close(visual_db);
        visual_db = NULL;
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }
    visual_retry_detached_links();
    visual_retry_pending_archives();
    ++visual_generation;
    visual_initialized = 1;
    pthread_mutex_unlock(&visual_lock);
    r2_log_sensory("visual_library_initialized",
                   "R2 Visual Experience Library initialized.",
                   library_directory, R2_VISION_DEFAULT_MODEL);
    return 0;
}

void r2_visual_shutdown(void)
{
    pthread_mutex_lock(&visual_lock);
    if (visual_db) sqlite3_close(visual_db);
    visual_db = NULL;
    visual_initialized = 0;
    publish_focus_region_impl(NULL, 0);
    if (visual_generation != UINT64_MAX) ++visual_generation;
    pthread_mutex_unlock(&visual_lock);
}

int r2_visual_set_model(const char *model)
{
    if (!model || !*model || strlen(model) >= sizeof(visual_model))
        return -1;
    for (const unsigned char *p = (const unsigned char *)model; *p; ++p) {
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == ':' || *p == '_' ||
               *p == '-' || *p == '.' || *p == '/'))
            return -1;
    }
    if (strcmp(model, R2_VISION_DEFAULT_MODEL) != 0) {
        fprintf(stderr,
                "[R2 Vision] Rejected model %s; conversation and vision must share %s.\n",
                model, R2_VISION_DEFAULT_MODEL);
        return -1;
    }
    pthread_mutex_lock(&visual_lock);
    snprintf(visual_model, sizeof(visual_model), "%s", R2_VISION_DEFAULT_MODEL);
    pthread_mutex_unlock(&visual_lock);
    r2_log_sensory("vision_model_selected",
                   "R2's visual perception model was selected.",
                   model, "Visual.c");
    return 0;
}

int r2_visual_is_initialized(void)
{
    pthread_mutex_lock(&visual_lock);
    int result = visual_initialized;
    pthread_mutex_unlock(&visual_lock);
    return result;
}

const char *r2_visual_model_name(void)
{
    static _Thread_local char model_snapshot[sizeof(visual_model)];
    pthread_mutex_lock(&visual_lock);
    snprintf(model_snapshot, sizeof(model_snapshot), "%s",
             visual_model[0] ? visual_model : R2_VISION_DEFAULT_MODEL);
    pthread_mutex_unlock(&visual_lock);
    return model_snapshot;
}

char *r2_visual_analyze_frame(const R2VisionFrame *frame,
                              const char *source,
                              const char *question)
{
    if (!frame || !frame->data || r2_ollama_vision_request_should_abort()) return NULL;

    char model_snapshot[sizeof(visual_model)];
    char directory_snapshot[sizeof(visual_directory)];
    uint64_t generation_snapshot, request_sequence;
    pthread_mutex_lock(&visual_lock);
    if (!visual_initialized || !visual_db || visual_request_sequence == UINT64_MAX) {
        pthread_mutex_unlock(&visual_lock);
        return NULL;
    }
    generation_snapshot = visual_generation;
    request_sequence = ++visual_request_sequence;
    snprintf(model_snapshot, sizeof(model_snapshot), "%s", visual_model);
    snprintf(directory_snapshot, sizeof(directory_snapshot), "%s", visual_directory);
    pthread_mutex_unlock(&visual_lock);

    size_t jpeg_length = 0;
    unsigned char *jpeg = encode_jpeg(frame, &jpeg_length);
    if (!jpeg) {
        if (r2_ollama_vision_request_should_abort()) return NULL;
        r2_log_sensory("visual_encoding_failed",
                       "R2 could not encode a captured frame for vision inference.",
                       "FFmpeg conversion from RGB24 to JPEG failed.", source);
        return NULL;
    }
    char *base64 = base64_encode(jpeg, jpeg_length);
    if (!base64) { free(jpeg); return NULL; }
    char *model_text = r2_ollama_vision_request_should_abort() ? NULL
        : call_vision_model(base64, question, model_snapshot);
    free(base64);
    if (!model_text || r2_ollama_vision_request_should_abort()) {
        free(model_text);
        free(jpeg);
        return NULL;
    }
    pthread_mutex_lock(&visual_lock);
    int same_library = visual_initialized && visual_db && visual_generation == generation_snapshot;
    pthread_mutex_unlock(&visual_lock);
    if (!same_library) {
        r2_log_sensory("visual_library_session_changed", "Visual library changed during analysis; no result was indexed into its replacement.", NULL, source);
        free(jpeg); free(model_text); return NULL;
    }
    json_object *focus = NULL;
    char *description = extract_visual_description(model_text, &focus);
    if (!description)
        r2_log_sensory("vision_invalid_description", "R2 rejected malformed visual description data.", model_text, source);
    free(model_text);
    if (!description) { free(jpeg); return NULL; }

    time_t now = time(NULL);
    struct tm utc;
    char created[40] = "unknown";
    if (gmtime_r(&now, &utc))
        strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &utc);

    char image_path[PATH_MAX];
    VisualExperience experience = {frame, created, source ? source : "Eyes",
                                   image_path, model_snapshot, question, description, 0, 0};
    int image_fd = -1;
    sqlite3 *archive_db = NULL;
    int archive_db_owned = 0;
    int staged = 0;
    int publication_attempted = 0;
    const char *failure = NULL;
    const char *failure_event = "visual_experience_store_failed";

    pthread_mutex_lock(&visual_lock);
    if (visual_initialized && visual_db && visual_generation == generation_snapshot &&
        !r2_ollama_vision_request_should_abort()) {
        archive_db = visual_open_archive_db(&archive_db_owned);
        if (archive_db)
            image_fd = visual_create_archive(directory_snapshot, frame, image_path, sizeof(image_path));
        if (!archive_db) {
            failure = "Could not retain the original archive database.";
        } else if (image_fd < 0) {
            failure_event = "visual_image_archive_failed";
            failure = "Could not create and claim the JPEG path.";
        } else if (visual_stage_archive(archive_db, &experience, jpeg, jpeg_length) != SQLITE_DONE) {
            failure = "Could not save the pending interpretation.";
        } else {
            staged = 1;
        }
    }
    pthread_mutex_unlock(&visual_lock);
    if (!staged) goto failed;

    /* No bytes reach the archive before their interpretation is durable. Keep
     * the file claim until publication or cleanup finishes. */
    if (write_all(image_fd, jpeg, jpeg_length) != 0 || visual_sync(image_fd) != 0 ||
        visual_sync_directory(directory_snapshot) != 0) {
        failure_event = "visual_image_archive_failed";
        failure = "Could not write and synchronize the JPEG and archive directory.";
        goto failed;
    }

    pthread_mutex_lock(&visual_lock);
    int rc = SQLITE_ABORT;
    if (visual_initialized && visual_db && visual_generation == generation_snapshot &&
        !r2_ollama_vision_request_should_abort()) {
        publication_attempted = 1;
        rc = visual_publish_archive(&experience);
        if (rc == SQLITE_DONE)
            publish_committed_focus(request_sequence, frame->preview_session, focus);
        else if (!r2_ollama_vision_request_should_abort())
            failure = "Could not publish the archived interpretation.";
    }
    pthread_mutex_unlock(&visual_lock);
    if (rc != SQLITE_DONE) goto failed;

    close(image_fd);
    if (archive_db_owned) sqlite3_close(archive_db);
    free(jpeg);
    json_object_put(focus);
    return description;

failed:
    if (image_fd >= 0) {
        int discard_image = !publication_attempted || experience.cancelled;
        if (staged) {
            if (archive_db_owned) {
                if (visual_withdraw_archive(archive_db, image_path) == SQLITE_DONE)
                    discard_image = 1;
            } else {
                pthread_mutex_lock(&visual_lock);
                if (visual_db && visual_generation == generation_snapshot &&
                    visual_withdraw_archive(visual_db, image_path) == SQLITE_DONE)
                    discard_image = 1;
                pthread_mutex_unlock(&visual_lock);
            }
        }
        /* If publication may have run, a failed withdrawal is not evidence
         * that no record refers to the file. Leave it for restart or audit. */
        if (discard_image) {
            if (unlink(image_path) == 0) (void)visual_sync_directory(directory_snapshot);
        } else {
            fprintf(stderr, "[R2 Visual] Retained an archive after an uncertain save outcome.\n");
        }
        close(image_fd);
    }
    if (archive_db_owned) sqlite3_close(archive_db);
    if (failure)
        r2_log_sensory(failure_event,
                       "R2 analyzed a frame but could not save the observation.",
                       failure, experience.source);
    free(jpeg);
    json_object_put(focus);
    free(description);
    return NULL;
}

static int visual_query(const char *query, int limit,
    int (*visit)(void *, int64_t, const char *, size_t), void *context)
{
    if (limit < 1) limit = 1;
    if (limit > 100) limit = 100;

    const char *sql = query
        ? "SELECT created_utc,source,model,description,image_path,id "
          "FROM r2_visual_experiences WHERE instr(lower(description),lower(?1))>0 "
          "OR instr(lower(source),lower(?1))>0 ORDER BY id DESC LIMIT ?2"
        : "SELECT created_utc,source,model,description,image_path,id "
          "FROM r2_visual_experiences ORDER BY id DESC LIMIT ?";
    int failed = 0;
    pthread_mutex_lock(&visual_lock);
    if (!visual_initialized || !visual_db) {
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }

    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(visual_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }

    if (query) {
        rc = sqlite3_bind_text(st, 1, query, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_bind_int(st, 2, limit);
    } else {
        rc = sqlite3_bind_int(st, 1, limit);
    }

    if (rc != SQLITE_OK) {
        sqlite3_finalize(st);
        pthread_mutex_unlock(&visual_lock);
        return -1;
    }

    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *created = sqlite3_column_text(st, 0);
        const unsigned char *source = sqlite3_column_text(st, 1);
        const unsigned char *model = sqlite3_column_text(st, 2);
        const unsigned char *desc = sqlite3_column_text(st, 3);
        const unsigned char *path = sqlite3_column_text(st, 4);
        const char *fields[] = {(const char *)created, (const char *)source,
            (const char *)model, (const char *)desc, (const char *)path};
        for (int i = 0; i < 5; ++i) {
            if ((!fields[i] && sqlite3_column_type(st, i) != SQLITE_NULL) ||
                (fields[i] && strlen(fields[i]) != (size_t)sqlite3_column_bytes(st, i))) {
                failed = 1; break;
            }
        }
        if (failed) break;
        char *line = NULL;
        int n = asprintf(&line, "[%s] source=%s model=%s image=%s\n%s\n\n",
            created ? (const char *)created : "?", source ? (const char *)source : "?",
            model ? (const char *)model : "?", path ? (const char *)path : "?",
            desc ? (const char *)desc : "(no description)");
        if (n < 0) { failed = 1; break; }
        int append_rc = visit(context, sqlite3_column_int64(st, 5), line, (size_t)n);
        free(line);
        if (append_rc != 0) { failed = 1; break; }
    }

    if (rc != SQLITE_DONE)
        failed = 1;
    sqlite3_finalize(st);
    pthread_mutex_unlock(&visual_lock);

    return failed ? -1 : 0;
}

static int visual_collect_row(void *context, int64_t id, const char *row, size_t length)
{
    (void)id;
    struct response_buffer *text = context;
    if (length > SIZE_MAX - text->length - 1) return -1;
    char *next = realloc(text->data, text->length + length + 1);
    if (!next) return -1;
    text->data = next;
    memcpy(text->data + text->length, row, length + 1);
    text->length += length;
    return 0;
}

int r2_visual_visit_search(const char *query, int limit,
    int (*visit)(void *, int64_t, const char *, size_t), void *context)
{
    if (!query || !*query || !visit) return -1;
    return visual_query(query, limit, visit, context);
}

int r2_visual_visit_recent(int limit,
    int (*visit)(void *, int64_t, const char *, size_t), void *context)
{
    if (!visit) return -1;
    return visual_query(NULL, limit, visit, context);
}

char *r2_visual_search(const char *query, int limit)
{
    struct response_buffer text = {0};
    if (visual_query(query && *query ? query : NULL, limit, visual_collect_row, &text) != 0) {
        free(text.data);
        return NULL;
    }
    return text.data ? text.data : strdup("(No visual experiences found.)\n");
}

char *r2_visual_recent(int limit)
{
    return r2_visual_search(NULL, limit);
}
