/* Source labels and image order must match the actual encoded inputs. */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>

static const char *message_field(json_object *message, const char *key)
{
    json_object *value = NULL;
    assert(message && json_object_object_get_ex(message, key, &value));
    return json_object_get_string(value);
}

static void verify_images(const char *camera, const char *media,
                          const char *expected_map)
{
    const char *identity = "You are R2-3PO.";
    const char *turn = "  What is happening around me? é\n";
    json_object *sections = json_object_new_array();
    assert(sections && ollama_json_append_message(
        sections, "system", identity, strlen(identity), NULL, NULL) == 0);
    assert(ollama_json_append_message_with_image_sources(
        sections, "user", turn, strlen(turn), camera, media) == 0);
    json_object *request = ollama_json_chat_messages(sections);
    json_object_put(sections);
    assert(request && json_object_array_length(request) == 2);

    json_object *system = json_object_array_get_idx(request, 0);
    assert(!strcmp(message_field(system, "role"), "system"));
    const char *content = message_field(system, "content");
    assert(!strncmp(content, identity, strlen(identity)));
    content += strlen(identity);
    if (expected_map) {
        assert(!strncmp(content, "\n\n", 2));
        assert(!strcmp(content + 2, expected_map));
    } else {
        assert(!*content);
    }
    json_object *message = json_object_array_get_idx(request, 1);
    assert(!strcmp(message_field(message, "role"), "user"));
    assert(!strcmp(message_field(message, "content"), turn));
    json_object *images = NULL;
    if (!expected_map) {
        assert(!json_object_object_get_ex(message, "images", &images));
    } else {
        assert(json_object_object_get_ex(message, "images", &images));
        assert(json_object_array_length(images) == (camera && media ? 2U : 1U));
        if (camera)
            assert(!strcmp(json_object_get_string(json_object_array_get_idx(images, 0)), camera));
        if (media)
            assert(!strcmp(json_object_get_string(json_object_array_get_idx(images, camera ? 1 : 0)), media));
    }
    json_object_put(request);
}

static void verify_current_observations(void)
{
    atomic_uint_fast64_t camera_revision = ATOMIC_VAR_INIT(3);
    atomic_uint_fast64_t media_revision = ATOMIC_VAR_INIT(5);
    R2VisionRequestSource camera = {.revision = &camera_revision, .expected = 3};
    R2VisionRequestSource media = {.revision = &media_revision, .expected = 5};
    atomic_store(&eyes_auto_allowed, 1);
    atomic_store(&media_attention_selected, 1);
    media.expected_attention = atomic_load(&media_attention_revision);
    const char *turn = "What is visible?  é";

    json_object *sections = json_object_new_array();
    assert(sections);
    assert(ollama_append_visual_observation(sections, "CAMERA", "jpeg", "Blue square.", &camera) == 0);
    assert(ollama_append_visual_observation(sections, "MEDIA DISPLAY", "jpeg", "Red star.", &media) == 0);
    assert(ollama_json_append_message(sections, "user", turn, strlen(turn), NULL, NULL) == 0);
    json_object *request = ollama_json_chat_messages(sections);
    json_object_put(sections);
    assert(request && json_object_array_length(request) == 2);
    json_object *system = json_object_array_get_idx(request, 0);
    const char *text = message_field(system, "content");
    const char *camera_label = strstr(text, "CURRENT CAMERA VISUAL EVIDENCE");
    const char *media_label = strstr(text, "CURRENT MEDIA DISPLAY VISUAL EVIDENCE");
    assert(camera_label && media_label && camera_label < media_label);
    assert(strstr(camera_label, "Blue square.") && strstr(media_label, "Red star."));
    json_object *user = json_object_array_get_idx(request, 1), *images = NULL;
    assert(!strcmp(message_field(user, "role"), "user"));
    assert(!strcmp(message_field(user, "content"), turn));
    assert(!json_object_object_get_ex(user, "images", &images));
    json_object_put(request);

    sections = json_object_new_array();
    assert(sections);
    /* Camera changed and media attention was released while inference ran. */
    atomic_fetch_add(&camera_revision, 1);
    atomic_store(&media_attention_selected, 0);
    assert(ollama_append_visual_observation(sections, "CAMERA", "jpeg", "Stale camera", &camera) == 0);
    assert(ollama_append_visual_observation(sections, "MEDIA DISPLAY", "jpeg", "Stale display", &media) == 0);
    assert(json_object_array_length(sections) == 0);
    camera.expected = atomic_load(&camera_revision);
    assert(ollama_append_visual_observation(sections, "CAMERA", "jpeg", NULL, &camera) == 0);
    assert(json_object_array_length(sections) == 1);
    const char *unavailable = message_field(json_object_array_get_idx(sections, 0), "content");
    assert(strstr(unavailable, "visual interpretation was unavailable"));
    json_object_put(sections);

    sections = json_object_new_array();
    assert(sections);
    atomic_store(&media_attention_selected, 1);
    media.expected_attention = atomic_load(&media_attention_revision);
    atomic_store(&media_capture_running, 1);
    assert(r2_vision_media_set_attention(0) == 0);
    assert(r2_vision_media_set_attention(1) == 0);
    assert(atomic_load(&media_revision) == media.expected);
    assert(r2_vision_source_changed(&media));
    assert(!r2_vision_source_changed(&camera));
    assert(ollama_append_visual_observation(sections, "MEDIA DISPLAY", "jpeg", "Before looking away", &media) == 0);
    assert(json_object_array_length(sections) == 0);
    media.expected_attention = atomic_load(&media_attention_revision);
    assert(ollama_append_visual_observation(sections, "MEDIA DISPLAY", "jpeg", "After looking back", &media) == 0);
    assert(json_object_array_length(sections) == 1);
    assert(r2_vision_media_set_attention(0) == 0);
    atomic_store(&media_capture_running, 0);
    json_object_put(sections);
}

static void verify_display_source_end(void)
{
    R2VisionRequestSource origin = {
        .revision = &media_source_revision,
        .expected = atomic_load(&media_source_revision),
    };
    atomic_store(&media_capture_running, 1);
    assert(!r2_vision_source_changed(&origin));
    r2_vision_media_end_capture();
    assert(!atomic_load(&media_capture_running));
    assert(r2_vision_source_changed(&origin));
    uint_fast64_t ended = atomic_load(&media_source_revision);
    r2_vision_media_end_capture();
    assert(atomic_load(&media_source_revision) == ended);
}

static void verify_camera_watch_stop(void)
{
    R2VisionRequestSource camera = {
        .revision = &eyes_source_revision,
        .expected = atomic_load(&eyes_source_revision),
    };
    R2VisionRequestSource display = {
        .revision = &media_source_revision,
        .expected = atomic_load(&media_source_revision),
    };
    atomic_store(&eyes_auto_allowed, 1);
    assert(!r2_vision_source_changed(&camera));
    atomic_store(&eyes_source_changing, 1);
    assert(r2_vision_source_changed(&camera));
    assert(!r2_vision_source_changed(&display));
    atomic_store(&eyes_source_changing, 0);

    atomic_store(&vision_watch_running, 1);
    assert(r2_vision_watch_stop() == 0);
    assert(r2_vision_source_changed(&camera));
    assert(!r2_vision_source_changed(&display));
    camera.expected = atomic_load(&eyes_source_revision);
    assert(r2_vision_watch_stop() == 0);
    assert(!r2_vision_source_changed(&camera));

    atomic_store(&vision_watch_running, 1);
    /* With no Eyes receiver, the production capture worker ends on its own. */
    assert(vision_watch_capture_worker(NULL) == NULL);
    assert(vision_watch_capture_done && r2_vision_source_changed(&camera));
    assert(!r2_vision_source_changed(&display));
    camera.expected = atomic_load(&eyes_source_revision);
    assert(r2_vision_watch_stop() == 0);
    assert(!r2_vision_source_changed(&camera));
}

int main(void)
{
    verify_images(NULL, NULL, NULL);
    verify_images("CAMERA", NULL,
        "CURRENT IMAGE SOURCE MAP: Image 1 is camera Eyes.");
    verify_images("CAMERA", "MEDIA",
        "CURRENT IMAGE SOURCE MAP: Image 1 is camera Eyes. Image 2 is the chosen media display.");
    verify_images(NULL, "MEDIA",
        "CURRENT IMAGE SOURCE MAP: Image 1 is the chosen media display. No camera frame is attached.");
    verify_current_observations();
    verify_display_source_end();
    verify_camera_watch_stop();

    json_object *request = json_object_new_array();
    assert(request && ollama_json_append_message(request, NULL, "invalid", 7, "CAMERA", "MEDIA") != 0);
    assert(json_object_array_length(request) == 0);
    json_object_put(request);
    puts("Media focus labels camera-only, display-only and dual-source input and preserves exact user text.");
    return 0;
}
