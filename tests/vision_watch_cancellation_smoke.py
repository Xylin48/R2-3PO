#!/usr/bin/env python3
"""Continuous Eyes capture must stop cleanly without launching background inference."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")

worker_start = core.index("static void *vision_watch_worker(void *unused)")
worker_end = core.index("\nint r2_vision_watch_start(void)", worker_start)
worker = core[worker_start:worker_end]
assert "vision_watch_worker_context = 1;" in worker
assert "vision_watch_worker_context = 0;" in worker
assert "pthread_cond_wait(&vision_watch_frame_cond" in worker
assert "r2_visual_analyze_frame(" not in worker
assert "call_vision_model(" not in worker

stop_start = core.index("int r2_vision_watch_stop(void)")
stop_end = core.index("\nint r2_vision_watch_active(void)", stop_start)
stop = core[stop_start:stop_end]
assert stop.index("atomic_store(&vision_watch_running, 0)") < stop.index("pthread_join(vision_watch_thread, NULL)")
assert stop.index("atomic_store(&vision_watch_running, 0)") < stop.index("pthread_join(vision_watch_capture_thread, NULL)")
assert "pthread_cond_broadcast(&vision_watch_frame_cond)" in stop
assert "r2_visual_analyze_frame(" in core[
    core.index("char *r2_vision_see(const char *question)"):
    core.index("char *r2_vision_recent(int limit)")
]
assert 'json_object_get_boolean(done)' in visual
assert 'strcmp(json_object_get_string(role), "assistant") == 0' in visual
assert '"vision_invalid_completion_contract"' in visual

print("Continuous Eyes shutdown joins capture/consumer workers; only deliberate visual inspection invokes inference.")
