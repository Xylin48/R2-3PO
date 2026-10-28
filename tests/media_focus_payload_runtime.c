/*
 * Runtime regression: a deliberate media view is the second image of the
 * same model message, never a replacement for R2's primary camera Eyes.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static struct json_object *message_images(struct json_object *arr, int index)
{
    struct json_object *message = json_object_array_get_idx(arr, index);
    struct json_object *images = NULL;
    assert(message != NULL);
    if (!json_object_object_get_ex(message, "images", &images))
        return NULL;
    return images;
}

int main(void)
{
    struct json_object *messages = json_object_new_array();
    assert(messages);

    /* Ordinary conversation: the camera is the only image. */
    assert(ollama_json_append_message(messages, "user", "Hello", 5,
                                      "CAMERA_FRAME", NULL) == 0);
    struct json_object *images = message_images(messages, 0);
    assert(images && json_object_array_length(images) == 1);
    assert(strcmp(json_object_get_string(json_object_array_get_idx(images, 0)),
                  "CAMERA_FRAME") == 0);

    /* R2 chose to watch a separate display. Both frames reach one turn in
       camera-first order with the exact original user message preserved. */
    const char *turn = "What is happening around me?";
    assert(ollama_json_append_message(messages, "user", turn, strlen(turn),
                                      "LIVE_CAMERA", "LIVE_MEDIA") == 0);
    struct json_object *message = json_object_array_get_idx(messages, 1);
    struct json_object *content = NULL;
    assert(json_object_object_get_ex(message, "content", &content));
    assert(strcmp(json_object_get_string(content), turn) == 0);
    images = message_images(messages, 1);
    assert(images && json_object_array_length(images) == 2);
    assert(strcmp(json_object_get_string(json_object_array_get_idx(images, 0)),
                  "LIVE_CAMERA") == 0);
    assert(strcmp(json_object_get_string(json_object_array_get_idx(images, 1)),
                  "LIVE_MEDIA") == 0);

    /* Without physical Eyes, a media-only image is not mislabeled as camera. */
    assert(ollama_json_append_message(messages, "user", "No eyes", 7,
                                      NULL, "UNATTACHED_MEDIA") == 0);
    assert(message_images(messages, 2) == NULL);

    /* Incomplete records must never be appended to the model request. */
    assert(ollama_json_append_message(messages, NULL, "bad", 3,
                                      "CAMERA", "MEDIA") != 0);
    assert(json_object_array_length(messages) == 3);
    json_object_put(messages);
    puts("Media focus dual-image payload runtime regression passed.");
    return 0;
}
