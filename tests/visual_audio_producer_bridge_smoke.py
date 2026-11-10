#!/usr/bin/env python3
"""Trace generic audio producer -> visible X11 window -> raw Eyes capture."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
ears = (root / "Ears.c").read_text()
ears_h = (root / "Ears.h").read_text()
audio = (root / "AudioDiscovery.c").read_text()
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
probe = audio[audio.index("static void r2_discovery_sink_input("):
              audio.index("static void r2_discovery_sink_info(", audio.index("static void r2_discovery_sink_input("))]
assert "PA_PROP_APPLICATION_PROCESS_ID" in probe
assert "info->corked" in probe
assert "info->sink" in probe
assert "r2_discovery_add_producer(scan, producer)" in probe
assert "r2_audio_discover(&sources)" in ears
vision = eyes[eyes.index("static int r2_eyes_find_window("):
              eyes.index("static FILE *r2_eyes_start_vlc(")]
assert '"xdotool", "search", "--onlyvisible", "--pid"' in vision
assert 'r2_eyes_probe_vlc_dimensions(candidate' in vision
assert 'r2_eyes_start_vlc(' not in vision
worker = shell[shell.index("static void shell_watch_media_poll("):
               shell.index("static void shell_watch_start(void)")]
assert "audio->playback_producers, audio->playback_producer_count" in worker
assert "r2_audio_discover_cancelable(" in worker
assert "r2_vision_select_playback_window(" in worker
assert "wanted_window = published" in worker
assert "r2_vision_media_open_x11_for_watch_cancelable(" in worker
assert "managed->window != wanted_window" in worker
assert "r2_vision_watch_active()" in worker
assert "r2_eyes_release_if_watch_owned(managed_camera)" in worker
assert "r2_vision_media_stop_if_watch_owned(managed_media.token)" in worker
assert "r2_visual_analyze_frame(" not in worker
assert "r2_ears_listen_cancelable(" not in worker
print("Application-independent X11 visual-source discovery and raw frame handoff passed.")
