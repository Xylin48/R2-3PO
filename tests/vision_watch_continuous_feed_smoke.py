#!/usr/bin/env python3
"""Regression checks for continuous capture independent of image-model inference."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")

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

assert "pthread_cond_wait(&vision_watch_frame_cond" in worker
assert "memcpy(frame_copy, vision_watch_latest_frame_data" in worker
assert "frame.data = frame_copy" in worker
assert "r2_visual_analyze_frame(" in worker
assert worker.index("pthread_mutex_unlock(&vision_watch_frame_lock)") < worker.index("r2_visual_analyze_frame(")
assert "vision_watch_frame_generation == last_generation" in worker
assert "previous_observation[1024]" in worker
assert "Previous frame observation:" in worker
assert "strcmp(previous_source, frame_source) != 0" in worker
assert "interval=approximately 15 seconds" not in worker
assert "elapsed_seconds >= 15" not in worker

assert "pthread_create(&vision_watch_capture_thread" in start
assert "pthread_create(&vision_watch_thread" in start
assert "pthread_join(vision_watch_thread, NULL)" in stop
assert "pthread_join(vision_watch_capture_thread, NULL)" in stop
assert "pthread_cond_broadcast(&vision_watch_frame_cond)" in stop
assert "Eyes captures continuously and Qwen analyzes successive fresh frames" in shell

print("Continuous Eyes capture, latest-frame handoff, nonblocking Qwen analysis, and dual-worker shutdown passed.")
