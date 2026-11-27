from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
r2 = (ROOT / "r2.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
sounds = (ROOT / "R2Sounds.c").read_text(encoding="utf-8")
sound_header = (ROOT / "R2Sounds.h").read_text(encoding="utf-8")
reply_module = (ROOT / "R2Reply.c").read_text(encoding="utf-8")

marker_processing_index = r2.find("clean_final_reply(reply)")
tool_processing_index = r2.find("char *tools = process_tools_with_status(stripped_reply, NULL, &capture_failed)")
strip_marker_index = r2.find("r2_sounds_strip_markers_with_literals(reply, literal_tool_span)")

checks = {
    "core includes the existing sound API": '#include "R2Sounds.h"' in r2,
    "conversation prompt permits rare state-tagged droid effects": "[R2_SOUND:beep:STATE]" in r2 and "[R2_SOUND:whistle:STATE]" in r2,
    "sound-state choices use contextual affect judgment rather than a fixed mapping": "AFFECT AND NONVERBAL EXPRESSION" in r2 and "Distinguish observed cues from uncertain interpretations" in r2 and "choose any state tag as your own contextual judgment, not by a fixed mapping" in r2 and "do not rely on a hard-coded emotion-to-response table" in r2,
    "nonverbal sounds remain optional and are not forced into routine replies": "Do not invent a physical event or force a sound into a routine reply" in r2 and "Prefer no sound when it adds nothing" in r2,
    "tool routing is decided before playback": marker_processing_index >= 0 and tool_processing_index >= 0 and tool_processing_index < marker_processing_index,
    "sound markers are stripped before tool side effects": strip_marker_index >= 0 and tool_processing_index >= 0 and strip_marker_index < tool_processing_index and "process_tools_with_status(stripped_reply, NULL, &capture_failed)" in r2 and "r2_sounds_strip_markers" in sound_header,
    "official FX location is hard-coded in the sound API": '/home/x/R2_Home/R2_sounds/FX' in sound_header,
    "launcher uses the official FX location as its default": '/home/x/R2_Home/R2_sounds/FX' in launcher,
    "reply sanitizer removes explicit private reasoning blocks": 'r2_reply_sanitize' in reply_module and '"<think>"' in reply_module and '"<analysis>"' in reply_module,
    "core sanitizes generated user-facing replies": 'r2_reply_sanitize_with_literals(reply, literal_tool_span)' in r2,
    "launcher compiles the reply sanitizer module": '"$R2_SOURCE/R2Reply.c"' in launcher,
    "Makefile builds and tracks reply sanitizer": "R2Reply.c" in makefile and "R2Reply.h" in makefile,
    "launcher compiles the sound module": '"$R2_SOURCE/R2Sounds.c"' in launcher,
    "launcher checks both sound module files": '"$R2_SOURCE/R2Sounds.h"' in launcher,
    "launcher preflights FX access as the runtime user": 'find "$1" -maxdepth 1 -type f -iname "*.mp3"' in launcher and "WARNING: r2 cannot find readable MP3 effects" in launcher,
    "launcher forwards the selected FX directory into the runtime": 'R2_FX_DIR="$R2_FX_DIR"' in launcher,
    "launcher runs the read-only sound inventory as r2 when deployed": 'run_as_r2' in launcher and 'python3 "$SOUND_AUDIT" --root "$R2_FX_DIR"' in launcher,
    "Makefile builds and tracks the sound module": "R2Sounds.c" in makefile and "R2Sounds.h" in makefile,
    "sound selection scans the configured FX directory": "R2_SOUNDS_DEFAULT_FX_DIR" in sounds and "opendir(fx_dir)" in sounds,
    "sound scheduling records an event in the existing Life Log": 'r2_log_event(R2_LOG_SENSORY, "sound_effect_scheduled"' in sounds,
    "core stops/reaps audio playback on shutdown": "r2_sounds_shutdown();" in r2,
    "core resets sound lifecycle during initialization": "r2_sounds_init();" in r2 and "void r2_sounds_init(void)" in sounds,
    "sound marker recognition is case-insensitive and ignores Markdown code examples": "strncasecmp(reply + pos, marker_text, marker_length)" in sounds and "inline_ticks == 0" in sounds and "sound_fence_line" in sounds and "if (in_fence)" in sounds,
    "marker cleanup failure returns a visible fallback instead of exposing control text": "Reply cleanup produced no visible text; returning a visible reply and preserving the user turn." in r2 and "if (!reply_has_visible_text(sound_clean_reply))" in r2,
    "empty or marker-only replies outside standalone display inspection report the failed turn before tool execution": (
        "if (!reply_has_visible_text(stripped_reply) &&" in r2
        and "!reply_is_standalone_media_inspection(stripped_reply)" in r2
        and "I couldn't produce a reply to this turn." in r2
    ),
    "tool output append preserves full action output with exact-size allocation": "asprintf(&_part, __VA_ARGS__)" in r2 and "memcpy(results + len, _part, (size_t)_n)" in r2 and "[tool output truncated]" not in r2 and "return tool_capture_failure(results, len);" in r2 and "sizeof(tool_capture_failure_notice)" in r2,
    "reply sanitizer classifies scratch and finds the answer in visible text": "visible_looks_private" in reply_module and "reply_find_final_content(visible, literal_span)" in reply_module,
}
failed = [label for label, ok in checks.items() if not ok]
for label, ok in checks.items():
    print(f"{'PASS' if ok else 'FAIL'}: {label}")
if failed:
    raise SystemExit(1)
print("R2 sound wiring smoke passed")
