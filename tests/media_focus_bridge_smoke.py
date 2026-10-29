#!/usr/bin/env python3
"""Chosen media focus carries independent live display pixels alongside camera Eyes."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
core = (root / "r2.c").read_text(encoding="utf-8")
header = (root / "r2.h").read_text(encoding="utf-8")
shell = (root / "shell.c").read_text(encoding="utf-8")

for api in ("r2_vision_media_attention_status", "r2_vision_media_set_attention"):
    assert f"int {api}(" in core
    assert f"int {api}(" in header

assert "static atomic_int media_attention_selected = ATOMIC_VAR_INIT(0);" in core
focus = core[core.index("int r2_vision_media_attention_status(void)"):
             core.index("/* Capture and inference are separate workers.")]
assert "atomic_exchange(&media_attention_selected, 1)" in focus
assert "atomic_exchange(&media_attention_selected, 0)" in focus
assert "!atomic_load(&eyes_auto_allowed) || !r2_vision_media_status()" in focus
assert "r2_log_sensory(\"media_attention_selected\"" in focus
assert "r2_log_sensory(\"media_attention_released\"" in focus
assert "r2_vision_media_status() ? 1 : 0;" in focus
media_stop = core[core.index("static int r2_vision_media_stop_locked(void)"):
                  core.index("int r2_vision_media_stop(void)")]
assert "atomic_exchange(&media_attention_selected, 0)" in media_stop
assert "media_attention_source_lost" in media_stop
assert "pthread_cond_broadcast(&media_frame_cond)" in media_stop
assert "media_frame_generation = 0;" in media_stop
assert "r2_eyes_close(media_eyes)" in media_stop
assert "r2_eyes_close(eyes)" not in media_stop

# Automatic discovery and continuous raw capture never choose R2's attention.
watch = shell[shell.index("static void *shell_watch_worker(void *unused)"):
              shell.index("static void shell_watch_start(void)")]
capture = core[core.index("static void *r2_media_capture_worker(void *unused)"):
               core.index("static int r2_vision_media_stop_locked(void)")]
assert "r2_vision_media_set_attention(" not in watch
assert "r2_vision_media_set_attention(" not in capture
assert "r2_visual_analyze_frame(" not in capture
assert "++media_frame_generation;" in capture
assert "pthread_cond_broadcast(&media_frame_cond);" in capture
assert "media_capture_done = 1;" in capture
assert "atomic_exchange(&media_attention_selected, 0)" in capture

# Deliberate current-turn focus is a separate action from a one-off frame look.
tool = core[core.index("/* R2 may choose to watch for subsequent model turns."):
            core.index("/* Deliberate hearing is an action")]
assert 'extract_marker(reply, "[MEDIA_FOCUS]"' in tool
assert "r2_process_media_focus_request(trim(request))" in tool
assert 'extract_marker(reply, "[MEDIA_SEE]"' in tool

copy = core[core.index("static int r2_vision_copy_media_frame(R2VisionFrame *frame,\n"):
            core.index("char *r2_vision_media_see(const char *question)")]
assert "pthread_mutex_lock(&media_frame_lock)" in copy
assert "pthread_cond_wait(&media_frame_cond, &media_frame_lock)" in copy
assert "media_frame_generation == 0 && !media_capture_done" in copy
assert "!media_capture_done && media_frame_generation > 0" in copy
assert "memcpy(copy, media_latest_data, media_latest_frame.size)" in copy
media = core[core.index("static char *r2_vision_media_current_frame_base64(void)\n{"):
             core.index("/* Capture and inference are separate workers.")]
assert "r2_vision_copy_media_frame(&frame, &event, &copy)" in media
assert "r2_visual_frame_base64(&frame)" in media
assert "r2_visual_analyze_frame(" not in media
assert "!r2_vision_media_attention_status()" in media

serializer = core[core.index("static int ollama_json_append_message("):
                  core.index("typedef struct {", core.index("static int ollama_json_append_message("))]
assert "const char *media_image_base64" in serializer
assert "const char *sources[2] = {image_base64, media_image_base64};" in serializer
assert 'json_object_object_add(message, "images", images) != 0' in serializer
assert "if ((image_base64 && *image_base64) ||" in serializer
assert "(media_image_base64 && *media_image_base64))" in serializer

turn = core[core.index("size_t ordered_count = r2_order_inference_messages("):
            core.index("free(message_order);", core.index("size_t ordered_count = r2_order_inference_messages("))]
assert "owned_image = r2_vision_current_frame_base64()" in turn
assert "if (atomic_load(&media_attention_selected))" in turn
assert "if (owned_image && atomic_load(&media_attention_selected))" not in turn
assert "owned_media_image = r2_vision_media_current_frame_base64()" in turn
assert "limits[i], owned_image, owned_media_image)" in turn
assert "free(owned_media_image);" in turn and "free(owned_image);" in turn

diary = core[core.index("/* Autonomous reflection can deliberately use"):
             core.index("ollama_autonomous_sensory_input = previous_sensory_input;",
                        core.index("/* Autonomous reflection can deliberately use"))]
assert 'extract_marker(reflection, "[MEDIA_FOCUS]"' in diary
assert "r2_process_media_focus_request(trim(focus_request))" in diary
assert "free(focus_request);" in diary

assert core.count("[MEDIA_FOCUS]") >= 7
assert "r2_vision_media_set_attention(0);" in core[core.index("int r2_vision_close(void)"):
                                                     core.index("int r2_eyes_start(void)")]
assert "r2_vision_media_set_attention(0);" in core[core.index("int r2_eyes_stop(void)"):
                                                     core.index("int r2_eyes_status(void)")]
print("Autonomous media focus, primary camera priority, dual-image serialization and no forced interpretation passed.")
