#!/usr/bin/env python3
"""Guard against stale model/lifecycle assumptions in the integrated core."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
header = (ROOT / "r2.h").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
readme = (ROOT / "README.md").read_text(encoding="utf-8")
conversation_qa = (ROOT / "docs/conversation-quality-regression.md").read_text(encoding="utf-8")


def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


match = re.search(r'^#define\s+R2_OLLAMA_MODEL\s+"([^"]+)"', header, re.MULTILINE)
require(match is not None, "the shared public header declares the conversational Llama model")
model = match.group(1)
require(
    f"`{model}` for conversation" in readme
    and "`qwen3-vl:4b-instruct-q4_K_M` for camera/TV" in readme,
    "README distinguishes the text and visual model configurations",
)
startup_reader = core.split("static char *get_startup_memories(int limit)", 1)[1].split(
    "MEMORY RETRIEVAL", 1
)[0]
require(
    "r2_ollama_context_tokens()" in startup_reader
    and "complete text remains available through memory retrieval" in startup_reader
    and "ORDER BY updated_at DESC, id DESC" in startup_reader,
    "startup memory selects complete recent records within the configured context budget",
)
require(
    "currently configured local Ollama model" in conversation_qa
    and "Gemma" not in conversation_qa
    and "llama3" not in conversation_qa.lower(),
    "live chat QA documentation avoids stale model names",
)
for stale_name in ("qwen3.5:9b", "gemma", "qwen2.5vl"):
    require(stale_name not in core.lower(), f"core source contains no stale {stale_name} model references")
require(
    "The implementation uses the production initialization sequence shared" in header
    and "should eventually contain/use" not in header,
    "public lifecycle documentation describes implemented initialization",
)
print("Model-reference consistency smoke test passed.")
