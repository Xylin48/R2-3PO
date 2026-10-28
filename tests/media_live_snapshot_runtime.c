/*
 * Runtime regression for the auxiliary live-media handoff. A chosen view
 * must wait for the first published frame, stop without hanging on source
 * loss, and never mistake an old source for current visual input.
 */
#define main r2_program_main
#include "../r2.c"
#undef main

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void pause_short(void)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 30000000L};
    (void)nanosleep(&ts, NULL);
}

static void *publish_frame(void *unused)
{
    (void)unused;
    pause_short();
    pthread_mutex_lock(&media_frame_lock);
    media_latest_data = malloc(12U);
    assert(media_latest_data);
    media_latest_capacity = 12U;
    for (size_t i = 0; i < 12U; ++i)
        media_latest_data[i] = (unsigned char)(i + 7U);
    memset(&media_latest_frame, 0, sizeof(media_latest_frame));
    media_latest_frame.data = media_latest_data;
    media_latest_frame.size = 12U;
    memset(&media_latest_event, 0, sizeof(media_latest_event));
    snprintf(media_latest_event.source_name, sizeof(media_latest_event.source_name),
             "%s", "fixture-video");
    ++media_frame_generation;
    pthread_cond_broadcast(&media_frame_cond);
    pthread_mutex_unlock(&media_frame_lock);
    return NULL;
}

static void *finish_capture(void *unused)
{
    (void)unused;
    pause_short();
    atomic_store(&media_capture_running, 0);
    pthread_mutex_lock(&media_frame_lock);
    media_capture_done = 1;
    pthread_cond_broadcast(&media_frame_cond);
    pthread_mutex_unlock(&media_frame_lock);
    return NULL;
}

int main(void)
{
    pthread_t producer;
    R2VisionFrame frame = {0};
    R2VisionEvent event = {0};
    unsigned char *owned = NULL;

    atomic_store(&media_capture_running, 1);
    pthread_mutex_lock(&media_frame_lock);
    media_frame_generation = 0;
    media_capture_done = 0;
    pthread_mutex_unlock(&media_frame_lock);
    assert(pthread_create(&producer, NULL, publish_frame, NULL) == 0);
    assert(r2_vision_copy_media_frame(&frame, &event, &owned) == 0);
    assert(owned && frame.data == owned && frame.size == 12U);
    assert(strcmp(event.source_name, "fixture-video") == 0);
    for (size_t i = 0; i < 12U; ++i)
        assert(frame.data[i] == (unsigned char)(i + 7U));
    assert(pthread_join(producer, NULL) == 0);
    free(owned);

    /* The worker can end while its latest raw frame buffer still exists. */
    atomic_store(&media_capture_running, 0);
    pthread_mutex_lock(&media_frame_lock);
    media_capture_done = 1;
    pthread_cond_broadcast(&media_frame_cond);
    pthread_mutex_unlock(&media_frame_lock);
    owned = NULL;
    assert(r2_vision_copy_media_frame(&frame, &event, &owned) != 0 && !owned);

    /* An end-before-first-frame waiter wakes and returns no invented sight. */
    atomic_store(&media_capture_running, 1);
    pthread_mutex_lock(&media_frame_lock);
    media_frame_generation = 0;
    media_capture_done = 0;
    pthread_mutex_unlock(&media_frame_lock);
    assert(pthread_create(&producer, NULL, finish_capture, NULL) == 0);
    owned = NULL;
    assert(r2_vision_copy_media_frame(&frame, &event, &owned) != 0 && !owned);
    assert(pthread_join(producer, NULL) == 0);

    /* If FFmpeg ends spontaneously, the previous watching choice is cleared.
       There is no model inference or new media source in this fixture. */
    atomic_store(&media_attention_selected, 1);
    atomic_store(&media_capture_running, 1);
    assert(media_eyes == NULL);
    (void)r2_media_capture_worker(NULL);
    assert(!atomic_load(&media_capture_running));
    assert(!atomic_load(&media_attention_selected));
    pthread_mutex_lock(&media_frame_lock);
    assert(media_capture_done);
    pthread_mutex_unlock(&media_frame_lock);

    assert(r2_vision_media_stop() == 0);
    puts("Auxiliary media first-frame wait, ended-stream rejection and chosen-focus cleanup passed.");
    return 0;
}
