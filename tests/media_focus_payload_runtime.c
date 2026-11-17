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
    const char *turn = "  What is happening around me? é\n";
    json_object *request = json_object_new_array();
    assert(request && ollama_json_append_message_with_image_sources(
        request, "user", turn, strlen(turn), camera, media) == 0);
    assert(json_object_array_length(request) == (expected_map ? 2U : 1U));

    if (expected_map) {
        json_object *map = json_object_array_get_idx(request, 0);
        assert(!strcmp(message_field(map, "role"), "system"));
        assert(!strcmp(message_field(map, "content"), expected_map));
    }
    json_object *message = json_object_array_get_idx(request, expected_map ? 1 : 0);
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

int main(void)
{
    verify_images(NULL, NULL, NULL);
    verify_images("CAMERA", NULL,
        "CURRENT IMAGE SOURCE MAP: Image 1 is camera Eyes.");
    verify_images("CAMERA", "MEDIA",
        "CURRENT IMAGE SOURCE MAP: Image 1 is camera Eyes. Image 2 is the chosen media display.");
    verify_images(NULL, "MEDIA",
        "CURRENT IMAGE SOURCE MAP: Image 1 is the chosen media display. No camera frame is attached.");

    json_object *request = json_object_new_array();
    assert(request && ollama_json_append_message(request, NULL, "invalid", 7, "CAMERA", "MEDIA") != 0);
    assert(json_object_array_length(request) == 0);
    json_object_put(request);
    puts("Media focus labels camera-only, display-only and dual-source input and preserves exact user text.");
    return 0;
}
