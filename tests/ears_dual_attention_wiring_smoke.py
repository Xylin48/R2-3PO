#!/usr/bin/env python3
"""Check dual Ears workers retain separate source, history and attention state."""
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text()
shell = (ROOT / "shell.c").read_text()
header = (ROOT / "r2.h").read_text()
start = core.index("static void *r2_ears_capture_worker(void *unused)")
desktop = core.index("static void *r2_ears_desktop_capture_worker(void *unused)")
capture = core[start:desktop]
desktop_end = core.index("static int r2_ears_write_buffered_audio(", desktop)
desktop_capture = core[desktop:desktop_end]
assert "r2_ears_read(ears, pcm, sizeof(pcm))" in capture
assert "r2_ears_read(ears_desktop, pcm, sizeof(pcm))" in desktop_capture
assert "ears_recent_audio" in capture and "ears_recent_audio" not in desktop_capture
assert "ears_desktop_recent_audio" in desktop_capture
assert "ears_desktop_recent_total_bytes += count" in desktop_capture
# An environmental change can become noticeable without automatic speech ASR.
assert "r2_ears_notice_pcm_change(pcm" in capture
assert "r2_ears_notice_pcm_change(pcm" in desktop_capture
assert "audio_level_change_notice" in core
assert "r2_ears_listen_cancelable(" not in capture
assert "r2_ears_listen_cancelable(" not in desktop_capture
assert "r2_ears_write_desktop_recent_audio(" in core
assert "r2_ears_write_desktop_buffered_audio(" in core
listen_start = core.index("char *r2_ears_listen_cancelable(unsigned seconds,")
listen_end = core.index("char *r2_ears_listen(unsigned seconds)", listen_start)
listen = core[listen_start:listen_end]
assert "if (r2_ears_desktop_requested)" in listen
assert "r2_ears_write_desktop_buffered_audio" in listen
assert "r2_ears_write_desktop_recent_audio" in listen
assert "r2_ears_write_buffered_audio" in listen
assert "r2_ears_write_recent_audio" in listen
assert "r2_ears_open_microphone(ears)" in listen
# The Life Log must not mistake desktop playback for microphone perception.
record_start = core.index("static int64_t r2_ears_record_transcript(")
record_end = core.index("char *r2_ears_listen_cancelable(", record_start)
record = core[record_start:record_end]
assert 'desktop ? "Ears desktop playback" : "Ears microphone"' in record
assert 'desktop ? "desktop" : "microphone"' in record
assert 'recent ? "recent" : "live"' in record
assert 'r2_log_sensory("speech_transcribed"' in record
assert 'r2_ears_record_transcript(transcript, seconds,' in listen
assert 'r2_ears_desktop_requested,' in listen and 'r2_ears_recent_requested)' in listen
assert 'LOCAL ASR TRANSCRIPT' in record
assert "short microphone utterance locally" not in listen
parser_start = core.index("static char *r2_process_ears_listen_request(")
parser_end = core.index("static char *process_tools_with_status(", parser_start)
parser = core[parser_start:parser_end]
for mode in ("desktop_recent", "desktop_live", "recent", "live"):
    assert mode in parser
assert "r2_ears_desktop_requested = desktop;" in parser
assert "r2_ears_desktop_requested = 0;" in parser
for name in ("r2_ears_start_desktop", "r2_ears_stop_desktop",
             "r2_ears_status_desktop"):
    assert "int " + name + "(void)" in core
    assert "int " + name + "(void);" in header
assert "r2_ears_stop_desktop();" in core[core.index("void r2_shutdown(void)"):], (
    "core teardown must stop the independent desktop worker"
)
watch = shell[shell.index("static void *shell_watch_worker(void *unused)"):
              shell.index("static void shell_watch_start(void)")]
assert "managed_microphone" in watch and "managed_desktop" in watch
assert "wanted_audio" not in watch
assert "r2_ears_start_desktop()" in watch and "r2_ears_start()" in watch
assert "r2_ears_stop_desktop()" in watch and "r2_ears_stop()" in watch
assert "r2_ears_status_desktop()" in watch and "r2_ears_status()" in watch
assert "audio_origin=desktop_playback" in core
assert "No automatic transcription or semantic interpretation." in core
print("Independent desktop/mic PCM workers, deliberate source-specific listening and persistence links passed.")
