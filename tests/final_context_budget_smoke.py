#!/usr/bin/env python3
"""Guard the final retrieval-context budget after all evidence sources are appended."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")

start = core.find("static char *chat_with_relevant_memories(")
end = core.find("\n}\n", start)
if start < 0 or end < 0:
    raise SystemExit("Could not isolate chat_with_relevant_memories().")
body = core[start:end + 3]

def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require("r2_bound_context_records" not in body and "final_context_limit" not in body,
        "selected evidence is not clipped by intermediate character caps")
require("r2_append_association_section(&memory_context" in body,
        "diary and visual sections use checked complete allocations")
require("r2_vision_current_frame_base64(&camera)" in core
        and "r2_vision_media_current_frame_base64(&media)" in core
        and "r2_visual_describe_encoded_frame(" in core,
        "camera and media frames are translated into evidence independently from historical records")
# Qwen generates its descriptions only after the provisional prompt allocation.
# Inspect the final Llama messages and refit the generation budget rather than
# letting independent visual evidence silently overflow the chosen context.
final_start = core.find("size_t final_input_bytes = 0;")
final_end = core.find("const char *payload = json_object_to_json_string_ext(root", final_start)
require(final_start > 0 and final_end > final_start,
        "completed visual evidence is budgeted before serialization")
require("int translated_visual_inputs = 0;" in core
        and "if (json_object_array_length(arr) != before_visual_evidence)" in core
        and "translated_visual_inputs = 1;" in core
        and "if (translated_visual_inputs) {" in core[final_start - 80:final_start],
        "only appended live visual evidence or availability notices trigger a final visual budget check")
final_wire = core[final_start:final_end]
require("json_object_array_length(chat_messages)" in final_wire
        and "json_object_get_string_len(content_value)" in final_wire
        and "final_input_bytes += content_bytes + 64U;" in final_wire
        and "r2_fit_complete_input(final_input_bytes, num_ctx, &num_predict)" in final_wire
        and 'ollama_json_object_add_checked(request_options, "num_predict"' in final_wire
        and "json_object_put(arr);" in final_wire,
        "full translated observations preserve the input and fit output or fail visibly")

require(body.find("if (life_history.length)") < body.find("size_t base_count"),
        "complete Life Log evidence is assembled before prompt construction")
require("r2_preserve_inference_units(msgs, count" in core
        and "Complete instructions, selected recall and current turn require" in core,
        "final inference preserves complete selected messages or rejects insufficient configured capacity")

budget_start = core.find("static R2SystemContextBudget r2_compute_system_context_budget(")
budget_end = core.find("\n}\n", budget_start)
if budget_start < 0 or budget_end < 0:
    raise SystemExit("Could not locate the prompt-budget allocator.")
budget = core[budget_start:budget_end + 3]
require("budget.retrieval_reserve = share + (remainder > 2 ? 1 : 0);" in budget,
        "the serializer explicitly reserves a share for current-turn retrieval")
require("budget.capabilities_reserve = share;" in budget,
        "the serializer reserves space for capability instructions after active-turn evidence")
require("r2_compute_system_context_budget(system_budget, override_length)" in core,
        "the actual Ollama serializer uses the tested prompt-budget allocator")
require("message_add_system_prompt_sections_locked() != 0" in core,
        "the static identity and capability prompts are split before runtime budgeting")
require("capability_context=%d" in core and "audit_capability_context = 1" in core,
        "prompt audit reports whether capability instructions survived serialization")
require("size_t category_remaining = reality_reserve > reality_used" in core
        and "reality_used += length;" in core,
        "provisional allocation still measures live Reality separately")
require("Keep the user's text byte-for-byte intact." in body and "copy[context_index].content = context_content;" in body,
        "retrieval preserves the user's actual message separately")
print("Final retrieved-context budget regression passed.")
