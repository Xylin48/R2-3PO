#!/usr/bin/env python3
"""Verify Watch coordinates raw sensory input without automatic interpretation."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")

worker_start = shell.index("static void *shell_watch_worker(void *unused)")
worker_end = shell.index("static void shell_watch_start(void)", worker_start)
worker = shell[worker_start:worker_end]
start = shell.index("static void shell_watch_start(void)")
stop = shell.index("static void shell_watch_stop(void)", start)
start_body = shell[start:stop]
stop_end = shell.index("static void shell_watch(const char *argument)", stop)
stop_body = shell[stop:stop_end]

assert "pthread_create(&shell_watch_thread, NULL, shell_watch_worker, NULL)" in start_body
assert "pthread_join(shell_watch_thread, NULL)" in stop_body
assert "atomic_store(&shell_watch_stop_requested, 1)" in stop_body
# Boot only starts discovery; an unplugged microphone must not abort Watch.
assert "pthread_create(&shell_watch_thread, NULL, shell_watch_worker, NULL)" in start_body
assert "r2_ears_status() > 0" in start_body
assert "r2_ears_start()" not in start_body
assert "r2_ears_stop()" not in start_body
assert "shell_watch_start();" in shell[shell.index("int r2_shell_run(void)"):]
assert "while (!shell_watch_cancelled(NULL))" in worker
assert "r2_ears_microphone_available()" in worker
assert "r2_ears_desktop_playing()" in worker
assert "r2_vision_camera_available()" in worker
assert "r2_vision_vlc_available()" in worker
assert "r2_vision_window_available(media_window)" in worker
assert "r2_ears_start_desktop()" in worker and "r2_ears_start()" in worker
assert "if (managed_audio)" in worker and "r2_ears_stop()" in worker
assert "if (managed_eyes)" in worker and "r2_eyes_stop()" in worker
assert "r2_vision_watch_start()" in worker
assert "r2_vision_open_window(media_window)" in worker
assert "r2_ears_listen_cancelable(" not in worker
assert "shell_talk_watch(" not in worker
assert "R2_WHISPER_MODEL" not in start_body and "R2_WHISPER_CLI" not in start_body
assert "Source discovery    : %s" in shell
assert "Visual input        : %s" in shell
assert "no automatic transcription or image descriptions are generated." in shell

assert "r2_talk(message)" in shell[
    shell.index("static void shell_talk_inner"):shell.index("static void shell_talk(const char *message)")
]
ears_start = shell.index("static void shell_ears(const char *argument)")
ears_end = shell.index("WATCH COORDINATOR", ears_start)
ears_body = shell[ears_start:ears_end]
assert "if (watch_running && argument && *argument" in ears_body
assert "Watch owns continuous raw microphone capture" in ears_body
assert "r2_ears_listen_cancelable(unsigned seconds," in core
assert "int r2_watch_start(void);" in header and "int r2_watch_stop(void);" in header
assert "shell_watch_stop();" in shell[
    shell.index("static void shell_shutdown_r2"):shell.index("static void shell_restart")
]
assert "shell_watch_stop();" in shell[
    shell.index("static void shell_restart"):shell.index("static int shell_parse_amount")
]
print("Watch startup, continuous raw sensory capture, explicit-only interpretation, and shutdown joins passed.")
