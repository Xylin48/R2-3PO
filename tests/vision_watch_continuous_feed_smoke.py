#!/usr/bin/env python3
"""Verify continuous raw Eyes input reaches the conversation brain without forced analysis."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
visual_header = (ROOT / "Visual.h").read_text(encoding="utf-8")

# Eyes continuously captures and publishes a stable latest-frame snapshot.
capture_start = core.index("static void *vision_watch_capture_worker(void *unused)")
worker_start = core.index("static void *vision_watch_worker(void *unused)", capture_start)
start_start = core.index("int r2_vision_watch_start(void)", worker_start)
stop_start = core.index("int r2_vision_watch_stop(void)", start_start)
active_start = core.index("int r2_vision_watch_active(void)", stop_start)
capture = core[capture_start:worker_start]
worker = core[worker_start:start_start]
start = core[start_start:stop_start]
stop = core[stop_start:active_start]

assert "r2_eyes_capture(eyes)" in capture
assert "vision_watch_latest_frame_data" in capture
assert "memcpy(vision_watch_latest_frame_data, frame.data, frame.size)" in capture
assert "++vision_watch_frame_generation" in capture
assert "pthread_cond_broadcast(&vision_watch_frame_cond)" in capture
assert "vision_watch_capture_done = 1" in capture
assert '"vision_watch_frame_snapshot_failed"' in capture

# The observer tracks capture lifecycle only; it must not call a vision model,
# describe each frame, or persist an interpretation just because a frame arrived.
assert "r2_visual_analyze_frame(" not in worker
assert "Describe this newly captured video frame" not in worker
assert "vision_watch_observation" not in worker
assert "visual_input_available" in worker
assert "vision_watch_frame_generation == last_generation" in worker
assert "pthread_cond_wait(&vision_watch_frame_cond" in worker

# Raw images, not generated descriptions, are serialized with the active user turn.
serializer_start = core.index("static int ollama_json_append_message(")
serializer_end = core.index("\ntypedef struct {", serializer_start)
serializer = core[serializer_start:serializer_end]
assert '"images"' in serializer
assert "image_base64" in serializer
assert "i == current_user_index" in core
assert "strstr(content, ollama_authoritative_user_query)" in core
assert "ollama_autonomous_sensory_input" in serializer
assert "ollama_autonomous_sensory_input = 1;" in core
assert "If Eyes is already open, a current image may be attached as raw visual input." in core
assert "Its presence is not an instruction to describe the image" in core

# The live image comes from the latest owned frame snapshot (or a fresh frame
# from an explicitly open Eyes source), then undergoes encoding only.
helper_start = core.index("static char *r2_vision_current_frame_base64(void)\n{")
helper_end = core.index("\nstatic void *vision_watch_worker(void *unused)", helper_start)
helper = core[helper_start:helper_end]
assert "vision_watch_latest_frame_data" in helper
assert "r2_eyes_capture(eyes)" in helper
assert "r2_visual_frame_base64(&frame)" in helper
assert "r2_visual_analyze_frame(" not in helper
assert "r2_eyes_open_camera" not in helper

encoder_start = visual.index("char *r2_visual_frame_base64(")
encoder_end = visual.index("\nstatic int vision_progress(", encoder_start)
encoder = visual[encoder_start:encoder_end]
assert "encode_jpeg(frame" in encoder
assert "base64_encode(jpeg, jpeg_length)" in encoder
assert "call_vision_model(" not in encoder
assert "r2_visual_frame_base64" in visual_header

# Explicit inspection remains available as a deliberate brain-selected action.
assert "char *r2_vision_see(const char *question)" in core
assert "vision_analyze_current_frame(question, 1)" in core

assert "pthread_create(&vision_watch_capture_thread" in start
assert "pthread_create(&vision_watch_thread" in start
assert "pthread_join(vision_watch_thread, NULL)" in stop
assert "pthread_join(vision_watch_capture_thread, NULL)" in stop
assert "pthread_cond_broadcast(&vision_watch_frame_cond)" in stop
assert "No background image descriptions are generated" in shell

print("Continuous raw Eyes input, direct image-to-brain serialization, and deliberate-only visual analysis passed.")
