#!/usr/bin/env python3
"""Trace generic audio producer -> visible X11 window -> raw Eyes capture."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ears = (root / "Ears.c").read_text()
ears_h = (root / "Ears.h").read_text()
eyes = (root / "Eyes.c").read_text()
eyes_h = (root / "Eyes.h").read_text()
core = (root / "r2.c").read_text()
core_h = (root / "r2.h").read_text()
shell = (root / "shell.c").read_text()
for symbol, source, hdr in (
    ("r2_ears_playback_pid", ears, ears_h),
    ("r2_eyes_window_for_pid", eyes, eyes_h),
    ("r2_vision_window_for_pid", core, core_h),
):
    assert symbol in source and symbol in hdr
probe = ears[ears.index("long r2_ears_playback_pid(void)"):
             ears.index("/* ---------------------------------------------------------\n * File process handling")]
assert 'pactl list sink-inputs' in probe
assert 'application.process.id' in probe
assert 'Corked: no' in probe
assert 'strtol(key, &end, 10)' in probe
vision = eyes[eyes.index("unsigned long r2_eyes_window_for_pid(long pid)"):
              eyes.index("static FILE *r2_eyes_start_vlc(")]
assert 'xdotool search --onlyvisible --pid' in vision
assert 'r2_eyes_probe_vlc_dimensions(candidate' in vision
assert 'r2_eyes_start_vlc(' not in vision
worker = shell[shell.index("static void *shell_watch_worker(void *unused)"):
               shell.index("static void shell_watch_start(void)")]
assert "r2_ears_playback_pid()" in worker
assert "r2_vision_window_for_pid(playback_pid)" in worker
assert "wanted_eyes == 4" in worker
assert "r2_vision_open_window(wanted_window_id)" in worker
assert "managed_window_id != wanted_window_id" in worker
assert "r2_vision_watch_active()" in worker
assert "r2_visual_analyze_frame(" not in worker
assert "r2_ears_listen_cancelable(" not in worker
print("Application-independent X11 visual-source discovery and raw frame handoff passed.")
