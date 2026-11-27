#!/usr/bin/env python3
"""Guard foreground scheduling, RAM-aware context, and additive memory migration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
text_budget = (ROOT / "r2_context_budget.h").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
visual = (ROOT / "Visual.c").read_text(encoding="utf-8")
diary = (ROOT / "r2_diary.c").read_text(encoding="utf-8")
life_log = (ROOT / "Log.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
alternate_self = (ROOT / "AlternateSelf.c").read_text(encoding="utf-8")

required = {
    "RAM-aware context selection": "int r2_ollama_context_tokens(void)",
    "Ollama receives explicit context length": '"num_ctx", json_object_new_int(num_ctx)',
    "Output budget uses the shared context limit":
        "num_predict = r2_text_predict_limit(num_ctx, num_predict);",
    "Legacy memories are migrated additively": "static int migrate_memory_schema(void)",
    "Legacy content aliases are preserved": '"content", "memory_text", "text", "description", "value"',
    "Current-turn reserve prevents old context crowding it out": "size_t current_user_reserve = 1500;",
    "Live history has a byte ceiling": "#define MAX_LIVE_MESSAGE_BYTES (32U * 1024U * 1024U)",
    "Autonomous reflection uses the background scheduler":
        "ollama_background_mode = 1;\n    ollama_reflection_worker = 1;",
    "cgroup RAM reads distinguish zero from unreadable": "static unsigned long long read_uint_file(const char *path, int *valid, int allow_max)",
    "cgroup RAM exhaustion forces minimal headroom": "(!cgroup_used_valid || cgroup_used >= cgroup_max)",
    "unlimited cgroup v1 sentinel is not treated as real headroom": "v1_max < (1ULL << 60)",
    "foreground timeout is configurable for long local inference": "R2_OLLAMA_TIMEOUT_SECONDS",
    "foreground timeout has a six-hour safety ceiling": "requested <= 21600L",
    "foreground timeout defaults to six hours for slow local inference": "const long default_timeout = 21600L;",
}
for label, needle in required.items():
    if needle not in core:
        raise SystemExit(f"Missing runtime headroom safeguard: {label}")
if ("int headroom = ctx > 1536 ? ctx - 1536 : 256;" not in text_budget or
        "if (ceiling > headroom) ceiling = headroom;" not in text_budget or
        "return requested > ceiling ? ceiling : requested;" not in text_budget):
    raise SystemExit("Missing shared text-model generation headroom cap.")

if "int r2_ollama_context_tokens(void);" not in (ROOT / "r2.h").read_text(encoding="utf-8"):
    raise SystemExit("Shared RAM-aware context sizing must be part of the core interface.")
if "int num_ctx = r2_ollama_context_tokens();" not in visual:
    raise SystemExit("Vision must use the shared RAM-aware context selector.")

reflection_start = core.find("static void *autonomous_thinking(")
reflection_end = core.find("/* ============================================================", reflection_start + 40)
if reflection_start < 0:
    raise SystemExit("Could not find autonomous reflection worker")
reflection = core[reflection_start:reflection_end if reflection_end > reflection_start else reflection_start + 12000]
background = reflection.index("ollama_background_mode = 1;")
generation = reflection.index("generate_with_private_evidence(")
learning = reflection.index("publish_structured_self_report(")
if not background < generation < learning or reflection.count("ollama_background_mode =") != 1:
    raise SystemExit("Reflection generation and derived learning must share the worker's background mode.")

talk_start = core.find("static char *r2_talk_impl(const char *message)")
talk_end = core.find("\nchar *r2_talk(const char *message)", talk_start)
if talk_start < 0 or talk_end < 0:
    raise SystemExit("Could not locate the serialized conversation-turn implementation.")
talk = core[talk_start:talk_end]
followup_start = talk.index("The action can change R2's current inventory")
refresh = talk.index("refresh_reality_context_message() != 0", followup_start)
followup = talk.index("chat_with_relevant_memories(message);", refresh)
recorded_result = talk.index("reply = recorded_action_reply(tool_evidence);", followup)
assert followup_start < refresh < followup < recorded_result
assert talk.count("chat_with_relevant_memories(message)") == 2
assert talk.count("process_tools(") == 1

for label, source, needle in (
    ("fridge schema migration", reality, "static int fridge_migrate_schema(const char *legacy_path)"),
    ("legacy stock import at first fridge creation", reality,
     "rc = fridge_import_legacy_stock(legacy_path);"),
    ("Reality need-state migration", reality, "static int migrate_reality_self_schema(void)"),
    ("visual experience schema migration", visual, "static int visual_migrate_schema(void)"),
    ("diary link schema migration", diary, "diary_ensure_column(\"r2_diary_entry_links\", \"memory_indexed\""),
    ("Life Log schema migration", life_log, "static int migrate_legacy_log_schema(void)"),
    ("repeated-interest schema migration", addiction, "static int migrate_addiction_schema(void)"),
    ("Alternate-Self schema migration", alternate_self, "static int migrate_altself_schema(void)"),
):
    if needle not in source:
        raise SystemExit(f"Missing backwards-compatibility safeguard: {label}")

if any("DROP TABLE" in source.upper() for source in (core, reality, visual, diary, life_log, addiction, alternate_self)):
    raise SystemExit("R2 schema migrations must never drop persistent tables")
if "DELETE FROM r2_alternate_self_branches" in alternate_self:
    raise SystemExit("Alternate-Self migration must preserve existing branches")
print("Runtime headroom and legacy memory safeguards passed.")
