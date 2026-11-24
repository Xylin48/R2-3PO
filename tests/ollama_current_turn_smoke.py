#!/usr/bin/env python3
"""Guard against dropping the active user turn during bounded Ollama history selection."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *ollama_chat_with_limit(")
end = core.find("\nstatic char *ollama_chat(", start)
if start < 0 or end < 0:
    raise SystemExit("Could not locate the bounded Ollama request builder.")
builder = core[start:end]

required = (
    "size_t current_user_index = SIZE_MAX;",
    'if (!msgs[index].role || strcmp(msgs[index].role, "user") != 0)',
    '"----- BEGIN CURRENT USER MESSAGE -----\\n"',
    '"----- END CURRENT USER MESSAGE -----"',
    "r2_preserve_inference_units(msgs, count, include, limits, current_user_index,",
    "query_length <= serialized_length - query_offset",
    "const char *authoritative_query = ollama_authoritative_user_query;",
    "strcmp(content, authoritative_query) == 0",
    "memcmp(begin, authoritative_query, wrapped_length) == 0",
    "if (wrapped_length && begin[wrapped_length - 1] == '\\n')",
    "if (current_user_index == SIZE_MAX && authoritative_query && *authoritative_query)",
    '"ollama_current_user_turn_missing"',
    "active user turn was absent from the serialized history",
    "if (current_user_index == SIZE_MAX)\n        current_user_index = fallback_user_index;",
)
for needle in required:
    if needle not in builder:
        raise SystemExit(f"Current-turn retention safeguard missing: {needle}")

if "static _Thread_local const char *ollama_authoritative_user_query = NULL;" not in core:
    raise SystemExit("The active user query must be tracked per thread, not in shared global state.")
if "ollama_authoritative_user_query = query;" not in core:
    raise SystemExit("Conversation inference must pass the authoritative user query into payload selection.")
if "ollama_authoritative_user_query = previous_authoritative_query;" not in core:
    raise SystemExit("The authoritative query binding must be restored after inference.")

print("Ollama current-turn retention smoke passed.")
