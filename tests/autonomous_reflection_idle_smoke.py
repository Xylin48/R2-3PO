#!/usr/bin/env python3
"""Guard idle-triggered reflection without delaying periodic Reality/needs checks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

activity_decl = "static atomic_uint_fast64_t last_conversation_activity_ms = ATOMIC_VAR_INIT(0);"
if activity_decl not in core:
    raise SystemExit("Missing monotonic foreground-activity timestamp.")

talk_start = core.find("char *r2_talk(const char *message)\n{")
talk_end = core.find("\n}", talk_start)
if talk_start < 0 or talk_end < 0:
    raise SystemExit("Could not locate the public conversation wrapper.")
talk = core[talk_start:talk_end]
activity_call = "record_conversation_activity(r2_log_elapsed_ms())"
activity_store = talk.find(activity_call)
turn_lock = talk.find("pthread_mutex_lock(&conversation_turn_lock)")
if activity_store < 0 or turn_lock < 0 or activity_store > turn_lock:
    raise SystemExit("A queued user turn must reset reflection idleness before waiting on the turn lock.")
if not turn_lock < talk.rfind(activity_call) < talk.rfind("atomic_fetch_sub(&foreground_conversation_waiting, 1)"):
    raise SystemExit("The idle interval must restart after foreground processing finishes.")

start = core.find("static void *autonomous_thinking(void *arg)")
end = core.find("\n}\n", start) + 3
if start < 0 or end < 3:
    raise SystemExit("Could not isolate autonomous_thinking.")
worker = core[start:end]

for required in (
    "sleep(1);",
    "r2_log_elapsed_ms()",
    "now_ms - last_activity_ms < interval_ms",
    "now_ms - last_reflection_ms < interval_ms",
    "r2_reality_tick();",
    "autonomous_needs_check();",
    "reflection_deferred_for_conversation",
    "reflection_discarded_after_user_activity",
    "atomic_load(&foreground_conversation_waiting) > 0",
    "atomic_load(&foreground_ollama_waiting) > 0",
):
    if required not in worker:
        raise SystemExit(f"Idle-reflection regression guard missing: {required}")

if "for (\n            int i = 0;\n            i < THINK_INTERVAL" in worker:
    raise SystemExit("Reflection still uses a fixed 900-second sleep that ignores recent conversation.")
if worker.count("reflection_activity_ms") < 3:
    raise SystemExit("Reflection must compare the activity snapshot before and after inference to reject stale context.")
claim = core[core.index("static int r2_reflection_try_claim("):start]
assert "pthread_mutex_trylock(&conversation_turn_lock)" in claim
assert "atomic_load(&last_conversation_activity_ms) != activity_ms" in claim
assert worker.count("r2_reflection_try_claim(reflection_activity_ms)") == 2
commit = worker.index("int diary_saved = r2_store_reflection(")
assert worker.rfind("r2_reflection_try_claim(reflection_activity_ms)", 0, commit) >= 0
assert worker.index("pthread_mutex_unlock(&conversation_turn_lock)", commit) < worker.index("publish_structured_self_report", commit)
if "THINK_INTERVAL 900" not in core:
    raise SystemExit("The established 900-second idle interval was changed.")

print("Autonomous reflection idle-gating regression checks passed.")
