#!/usr/bin/env python3
"""Regression checks for the display-only V-Webcam integration."""
import json
from pathlib import Path
import runpy
import sys
from tempfile import TemporaryDirectory
from types import ModuleType
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def verify_session_matching() -> None:
    # Exercise the actual file readers and refresh path without an X server.
    tk = ModuleType("tkinter")
    tk.ttk = Mock()
    tk.TclError = RuntimeError
    tk.PhotoImage = Mock(side_effect=lambda **kwargs: kwargs["data"])
    with patch.dict(sys.modules, {"tkinter": tk}):
        viewer_type = runpy.run_path(str(ROOT / "V-Webcam.py"))["VWebcam"]

    with TemporaryDirectory() as directory:
        status, frame, focus = (Path(directory) / name for name in (
            "status.txt", "frame.ppm", "focus.json",
        ))
        scope = viewer_type.refresh.__globals__
        scope.update(STATUS_PATH=status, FRAME_PATH=frame, FOCUS_PATH=focus)
        viewer = viewer_type.__new__(viewer_type)
        viewer.paused = False
        viewer.last_frame_stamp = None
        viewer.canvas = Mock()
        viewer.image_item = 1
        viewer.fit_image = Mock()
        viewer.draw_focus = Mock()

        pixels = b"1 1\n255\n\x00\x80\xff"
        label = "complete " * 100
        region = {"x": 0, "y": 0, "width": 1, "height": 1, "label": label.strip()}
        source = "  " + "é" * 700 + "\r\nlong exact title\n "
        status.write_text("1\nlegacy label\n1\n1\n1\n123\n10\nsource_hex=" +
                          source.encode().hex() + "\n")
        assert viewer.status()["source"] == source
        status.write_text("1\nSame name\n1\n1\n1\n123\n10\n")
        frame.write_bytes(b"P6\n# r2-preview 10\n" + pixels)
        focus.write_text(json.dumps(dict(region, preview_session=10)))
        viewer.refresh()
        assert viewer.raw_photo == b"P6\n" + pixels
        assert viewer.display_focus == region

        status.write_text("1\nSame name\n1\n1\n1\n124\n20\n")
        viewer.paused = True
        viewer.refresh()
        assert viewer.raw_photo and viewer.display_focus == region
        viewer.paused = False
        viewer.refresh()
        assert viewer.raw_photo is None and viewer.display_focus is None

        # The old overlay must not be drawn after the new pixels arrive.
        frame.write_bytes(b"P6\n# r2-preview 20\n" + pixels)
        viewer.refresh()
        assert viewer.raw_photo and viewer.display_focus is None
        focus.write_text(json.dumps(dict(region, preview_session=20)))
        viewer.refresh()
        assert viewer.display_focus == region
        assert tk.PhotoImage.call_count == 2

        status.write_text("1\nSame name\n1\n1\n1\n124\n")
        viewer.refresh()
        assert viewer.raw_photo is None and viewer.display_focus is None

        frame.write_bytes(b"P6\n" + pixels)
        focus.write_text(json.dumps(region))
        viewer.refresh()
        assert viewer.raw_photo == b"P6\n" + pixels
        assert viewer.display_focus is None

        status.write_text("1\nSame name\n1\n1\n1\n124\n0\n")
        viewer.refresh()
        assert viewer.raw_photo is None and viewer.display_focus is None

        status.write_text("1\nSame name\n1\n1\n1\n124\n20\n")
        viewer.refresh()
        assert viewer.raw_photo is None and viewer.display_focus is None
        frame.write_bytes(b"P6\n# r2-preview 20\n" + pixels)
        focus.write_text(json.dumps(dict(region, preview_session=20)))
        viewer.refresh()
        assert viewer.raw_photo and viewer.display_focus == region
        frame.unlink()
        viewer.refresh()
        assert viewer.raw_photo is None and viewer.display_focus is None


def main() -> None:
    viewer = read("V-Webcam.py")
    eyes = read("Eyes.c")
    visual = read("Visual.c")
    launcher = read("R2_Launch_Code.sh")
    preflight = read("tools/preflight_r2_hardware.sh")
    workflow = read(".github/workflows/build.yml")

    assert "frame.ppm" in viewer and "focus.json" in viewer
    assert "Display only" in viewer and "Pause display" in viewer
    assert "not os.environ.get(\"DISPLAY\")" in viewer
    assert "DATA_DIR.lstat()" in viewer and "os.geteuid()" in viewer
    assert 'self.canvas.itemconfigure(self.image_item, image="")' in viewer
    assert "focus = self.display_focus" in viewer
    assert '#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"' in eyes
    assert '#define R2_VWEBCAM_DIR "/tmp/r2-vwebcam-r2"' in visual
    assert "r2_vwebcam_publish_frame(eyes)" in eyes
    assert "r2_vwebcam_start_viewer()" in eyes and "r2_vwebcam_stop_viewer()" in eyes
    assert "if (total) { errno = EPROTO; return -1; }" in eyes, (
        "partial raw frames must be reported as read failures, not clean EOF"
    )
    capture_start = eyes.index("int r2_eyes_capture(")
    capture_end = eyes.index("/* ============================================================\n   FRAME ACCESS", capture_start)
    capture = eyes[capture_start:capture_end]
    eof_branch = capture[capture.index("if (result == 0)"):capture.index("if (result < 0)")]
    assert "r2_eyes_close(eyes);" in eof_branch, (
        "EOF from a live camera or file must close the dead FFmpeg stream"
    )
    assert "if (!eyes->source_is_live)" not in eof_branch, (
        "a dead live-camera pipe must not remain marked open"
    )
    assert 'char *const argv[] = {"/usr/bin/python3", viewer, NULL};' in eyes
    assert "execv(argv[0], argv)" in eyes
    assert "PR_SET_PDEATHSIG" in eyes and "getppid() != parent_pid" in eyes
    assert "execlp(\"python3\"" not in eyes
    assert 'getenv("R2_CAMERA_DEVICE")' in eyes
    assert 'R2_CAMERA_DEVICE="${R2_CAMERA_DEVICE:-/dev/video0}"' in launcher
    assert "R2_CAMERA_DEVICE" in read("docs/local-speech-input.md")
    assert "R2_VWEBCAM_MAX_W 960U" in eyes
    assert "r2_vwebcam_last_publish_ms" in eyes
    assert "if (!display || !*display)" in eyes
    assert "Return ONLY valid JSON with keys description and focus" in visual
    assert '"format", json_object_new_string("json")' in visual
    assert "extract_visual_description(model_text, &focus)" in visual
    assert "request_sequence <= visual_focus_sequence" in visual
    commit_start = visual.index("rc = visual_publish_archive(&experience);")
    commit_end = visual.index("pthread_mutex_unlock(&visual_lock);", commit_start)
    assert "if (rc == SQLITE_DONE)" in visual[commit_start:commit_end]
    assert "publish_committed_focus(request_sequence, frame->preview_session, focus)" in visual[commit_start:commit_end]
    assert "r2_log_save_shared_sensory(visual_db" in visual
    assert "visual_save_separate(experience)" in visual
    assert "visual_insert_compat(db, experience->created, experience->source," in visual
    for legacy_column in ("created_at", "timestamp", "file_path", "path"):
        assert f'visual_column_exists(db, "{legacy_column}")' in visual
    assert 'visual_backfill("image_path", "file_path")' in visual
    assert "publish_focus_region_impl(focus, preview_session)" in visual
    assert "const char *display = getenv(\"DISPLAY\")" in visual
    assert "if (!display || !*display)" in visual
    assert '"$R2_SOURCE/V-Webcam.py"' in launcher
    assert 'R2_SOURCE="$R2_SOURCE"' in launcher
    assert "V-Webcam.py" in preflight
    assert "python3 -m py_compile V-Webcam.py tests/vwebcam_smoke.py" in workflow
    verify_session_matching()
    print("V-Webcam mirror and focus overlay smoke checks passed.")


if __name__ == "__main__":
    main()
