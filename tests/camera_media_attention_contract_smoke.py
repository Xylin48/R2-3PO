#!/usr/bin/env python3
"""R2's camera is his eyesight; a playing video is an auxiliary observed display."""
from pathlib import Path
p = Path(__file__).resolve().parents[1]
core = (p / "r2.c").read_text()
eyes = (p / "Eyes.c").read_text()
shell = (p / "shell.c").read_text()
header = (p / "r2.h").read_text()
for api in ("r2_vision_media_open_window", "r2_vision_media_stop",
            "r2_vision_media_see", "r2_vision_media_status",
            "r2_eyes_release_source", "r2_sense_auto_allowed",
            "r2_sense_set_auto_allowed"):
    assert api in core and api in header, api
assert "static R2Eyes *eyes = NULL;" in core
assert "static R2Eyes *media_eyes = NULL;" in core
assert "static void *r2_media_capture_worker(void *unused)" in core
media = core[core.index("static void *r2_media_capture_worker("):
             core.index("/* Capture and inference are separate workers.")]
assert "r2_eyes_capture(media_eyes)" in media
assert "media_latest_frame" in media and "media_latest_event" in media
assert "r2_visual_analyze_frame" in media[media.index("char *r2_vision_media_see("):]
assert "r2_visual_analyze_frame" not in media[:media.index("char *r2_vision_media_see(")]
assert "media_visual_change_notice" in media
assert "r2_eyes_set_auxiliary(media_eyes, 1)" in media
assert "int auxiliary_display;" in eyes
assert "if (!eyes->auxiliary_display) r2_vwebcam_publish_frame(eyes)" in eyes
assert "if (!eyes->auxiliary_display) r2_vwebcam_stop_viewer()" in eyes

camera = core[core.index("static void *vision_watch_capture_worker("):
              core.index("static char *r2_vision_current_frame_base64(void)", core.index("static void *vision_watch_capture_worker("))]
assert "r2_eyes_capture(eyes)" in camera
assert "camera_visual_change_notice" in camera
assert "r2_visual_analyze_frame" not in camera
current_frame = core[core.index("static char *r2_vision_current_frame_base64(void)\n{"):
                     core.index("static void *vision_watch_worker(")]
assert "r2_eyes_capture(eyes)" in current_frame
assert "r2_eyes_capture(media_eyes)" not in current_frame

worker = shell[shell.index("static void *shell_watch_worker("):
               shell.index("static void shell_watch_start(void)")]
assert "int managed_camera = 0;" in worker
assert "int managed_media = 0;" in worker
assert "r2_eyes_start() == 0" in worker
assert "r2_vision_media_open_window(wanted_window)" in worker
assert "r2_vision_media_stop()" in worker
assert "r2_eyes_release_source()" in worker
assert "r2_eyes_stop()" not in worker, "auto source loss cannot imply chosen closure"
for channel in ("1", "2", "3"):
    assert f"r2_sense_auto_allowed({channel})" in worker
assert "r2_eyes_open_window(eyes," not in core, "media must not replace camera"
assert "r2_eyes_open_vlc(eyes)" not in core
assert "r2_eyes_open_file(eyes," not in core
assert "r2_eyes_open_window(media_eyes," in media

tool = core[core.index("/* Decisions to open/close sensors override Watch discovery. */"):
            core.index("/* Deliberate hearing is an action")]
assert 'extract_marker(reply, "[SENSE]"' in tool
assert 'extract_marker(reply, "[MEDIA_SEE]"' in tool
assert "r2_process_media_see_request" in tool
diary = core[core.index("/* Autonomous reflection can deliberately use"):
             core.index("ollama_autonomous_sensory_input = previous_sensory_input;", core.index("/* Autonomous reflection can deliberately use"))]
for name in ("[SENSE]", "[MEDIA_SEE]", "[EARS_LISTEN]"):
    assert name in diary
assert "r2_process_sense_request" in diary
assert "r2_process_media_see_request" in diary
shutdown = core[core.index("void r2_shutdown(void)"): ]
assert "r2_vision_media_stop();" in shutdown
assert "r2_eyes_shutdown(media_eyes);" in shutdown
assert "r2_eyes_shutdown(eyes);" in shutdown
assert "conversation_with_media_display_available" in core
assert "media_display_was_available" in core
assert "closed senses remain closed" in core.lower()
print("Primary Eyes, auxiliary video, raw change awareness and voluntary sensor decisions passed.")
