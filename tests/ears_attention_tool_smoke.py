#!/usr/bin/env python3
"""R2 may deliberately inspect real audio; raw Ears capture never auto-transcribes."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
prompt = core[core.index("static const char *SYSTEM_PROMPT ="):core.index("static const char r2_conversation_system_prompt[]")]
tools_start = core.index("/* Deliberate hearing is an action chosen by the brain")
tools_end = core.index("/* Persistent room, object, inventory and self-continuity actions.", tools_start)
tool = core[tools_start:tools_end]
watch = (ROOT / "shell.c").read_text(encoding="utf-8")
worker_start = watch.index("static void *shell_watch_worker(void *unused)")
worker_end = watch.index("static void shell_watch_start(void)", worker_start)
worker = watch[worker_start:worker_end]

assert "[EARS_LISTEN] recent|seconds" in prompt and "[EARS_LISTEN] live|seconds" in prompt
assert "do not use it for hypothetical situations" in prompt.lower()
assert 'extract_marker(reply, "[EARS_LISTEN]", "\\n", &ears_listen_pos)' in tool
assert "strtoul(seconds_text, &end, 10)" in tool
assert "seconds >= 1UL && seconds <= 15UL" in tool
assert "r2_ears_listen_cancelable((unsigned)seconds, NULL, NULL)" in tool
assert "r2_ears_recent_requested = recent;" in tool
assert "r2_ears_write_buffered_audio(total_bytes, wav)" in core
assert "r2_ears_write_recent_audio(total_bytes, start_total, wav, cancelled, userdata)" in core
assert "r2_ears_status() > 0 && !atomic_load(&ears_capture_running)" in tool
assert "That source was left untouched" in tool
assert "Do not infer what was said." in tool

# Watch only maintains raw capture and must not turn incoming speech into chat.
assert "r2_ears_listen_cancelable(" not in worker
assert "shell_talk_watch(" not in worker
assert "No automatic transcription or image descriptions are generated." in watch
print("Deliberate Ears listening tool and no-auto-transcription Watch behavior passed.")
