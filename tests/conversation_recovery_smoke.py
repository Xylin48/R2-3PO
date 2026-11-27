#!/usr/bin/env python3
"""Regression guard for multi-turn conversation recovery and turn serialization."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


def function_slice(start_marker: str, end_marker: str) -> str:
    start = core.find(start_marker)
    end = core.find(end_marker, start + 1) if start >= 0 else -1
    if start < 0 or end < 0:
        raise SystemExit(f"Could not isolate {start_marker!r}")
    return core[start:end]


impl = function_slice(
    "static char *r2_talk_impl(const char *message)",
    "char *r2_talk(const char *message)",
)
public = function_slice(
    "char *r2_talk(const char *message)",
    "int r2_conversation(void)",
)

# Persist the actual current user turn before model work, so a failed inference
# can be retried without silently dropping or rewriting the user's words.
add_user = impl.find('message_add("user", message)')
first_generation = impl.find("chat_with_relevant_memories(message)")
require(
    add_user >= 0 and first_generation > add_user,
    "the original user turn is stored before contextual generation begins",
)
require(
    "RELATIONSHIP AND NAME CONTINUITY" in core
    and "quoted message, an archived conversation, a diary" in core
    and "prioritize the correction" in core,
    "explicit family relationships survive name contamination from quoted/retrieved history while current corrections win",
)

# Affect should be interpreted contextually by R2, not routed through a
# hard-coded emotion-to-response mapping in the system prompt.
require(
    "AFFECT AND NONVERBAL EXPRESSION" in core
    and "do not rely on a hard-coded emotion-to-response table" in core
    and "choose any state tag as your own contextual judgment, not by a fixed mapping" in core
    and "Use sleepy or hungry only when the current modeled need actually supports it" not in core,
    "affect and nonverbal expression are learned from context rather than fixed state-to-reaction rules",
)

require(
    'r2_conversation_system_prompt' in core and 'R2_CONVERSATION_IDENTITY' in core,
    "the first answer includes the robot's identity",
)

require(
    impl.count("chat_with_relevant_memories(message)") == 2
    and "Conversation generation failed; preserving the user turn." in impl,
    "the answer and the action follow-up each have one generation path",
)

# A single display inspection may run before producing outward text.
visible_start = core.index("static size_t reply_control_span(")
visible_end = core.index("static char *r2_talk_impl(", visible_start)
visible_helper = core[visible_start:visible_end]
require(
    'control_blocks[] = {' in core and "control_blocks[i].open" in visible_helper
    and '{"[RUN]", "[END RUN]"}' in core
    and '"[WORLD]"' in visible_helper
    and "Malformed control output is never user-facing text" in visible_helper
    and "reply_control_span(text, line)" in visible_helper
    and "reply_control_span(reply, reply + offset)" in visible_helper,
    "action-control blocks and lines do not count as visible user-facing text",
)
visible_gate = impl.find("if (!reply_has_visible_text(stripped_reply) &&")
tool_execution = impl.find("char *tools = process_tools_with_capture_status(stripped_reply, NULL, &capture_failed)")
require(
    visible_gate >= 0 and tool_execution > visible_gate
    and "!reply_is_standalone_media_inspection(stripped_reply)" in impl[visible_gate:tool_execution]
    and "I couldn't produce a reply to this turn." in impl[visible_gate:tool_execution],
    "empty replies and other command-only replies fail before tool execution",
)
require(
    "if (!tools)" in impl
    and "couldn't finish processing that response" in impl
    and "goto finalize_reply;" in impl,
    "tool-processing failure is converted into a visible response instead of a silent turn",
)

tool_processor = function_slice(
    "static char *process_tools_with_capture_status(",
    "/* Preserve the existing result-only interface for ordinary callers. */",
)
growth_start = tool_processor.find("char *_grown = realloc(results, _new_cap);")
growth_end = tool_processor.find("results = _grown;", growth_start)
growth_failure = tool_processor[growth_start:growth_end] if growth_start >= 0 and growth_end >= 0 else ""
growth_commit = tool_processor[growth_end:growth_end + 120] if growth_end >= 0 else ""
require(
    "An action may already have completed" in core
    and "Verify the current state " in core
    and "before retrying the request." in core
    and "return tool_capture_failure(results, len);" in growth_failure
    and "if (capture_failed) *capture_failed = 1;" in growth_failure
    and "free(results);" not in growth_failure
    and "sizeof(tool_capture_failure_notice)" in tool_processor
    and "memmove(results + prefix, results, len + 1)" in core
    and "cap = _new_cap;" not in growth_failure
    and "cap = _new_cap;" in growth_commit,
    "tool-result allocation failure keeps warning bounded by the original allocation",
)
direct_tool_oom = impl.find("if (capture_failed)")
require(
    direct_tool_oom >= 0
    and "I ran out of memory while recording an action result." in core
    and "reply = tools;" in impl[direct_tool_oom:]
    and "tool_evidence = tools;" in impl[direct_tool_oom:]
    and "xstrdup(" not in impl[direct_tool_oom:impl.find("/* Tool-routing replies", direct_tool_oom)]
    and "goto finalize_reply;" in impl[direct_tool_oom:],
    "tool-result OOM warning is shown directly instead of being paraphrased by another model call",
)

# The public entry point must serialize entire turns, while announcing intent
# before waiting so autonomous background work can yield to queued user input.
lock = public.find("pthread_mutex_lock(&conversation_turn_lock)")
intent = public.find("atomic_fetch_add(&foreground_conversation_waiting, 1)")
call = public.find("r2_talk_impl(message)")
unlock = public.rfind("pthread_mutex_unlock(&conversation_turn_lock)")
decrement = public.rfind("atomic_fetch_sub(&foreground_conversation_waiting, 1)")
require(
    intent >= 0 and lock > intent and call > lock and unlock > call
    and decrement > unlock,
    "foreground intent is announced before lock wait and turn ownership spans the full implementation",
)
shutdown_guard = public[lock:call]
require(
    "if (shutting_down)" in shutdown_guard
    and "pthread_mutex_unlock(&conversation_turn_lock)" in shutdown_guard
    and "atomic_fetch_sub(&foreground_conversation_waiting, 1)" in shutdown_guard
    and "return xstrdup(" in shutdown_guard,
    "shutdown rejects new turns before inference and releases the lock and waiting count",
)
require(
    "reply = xstrdup(" in public
    and "returning an explicit failure message" in public
    and public.find("reply = xstrdup(") > call,
    "the public API has its own non-empty fallback if the implementation fails",
)

# Background needs work must never wait behind or mutate a live user turn.
needs = function_slice(
    "static void autonomous_needs_check(void)",
    "int r2_think(void)",
)
require(
    "pthread_mutex_trylock(&conversation_turn_lock)" in needs
    and "atomic_load(&foreground_ollama_waiting) > 0" in needs
    and "atomic_load(&foreground_conversation_waiting) > 0" in needs,
    "model-selected autonomous needs work yields to foreground model and conversation work",
)
require(
    "autonomous_needs_food_safeguard" not in core
    and "r2_reality_autonomous_feed_if_needed" not in core,
    "a failed or missing model decision must not trigger deterministic feeding",
)
require(
    "pthread_mutex_trylock(&conversation_turn_lock)" in core
    and "autonomous_needs_stale_action" in core,
    "autonomous action path uses a non-blocking turn guard and stale-action protection",
)

# The Hands worker checks task_head/shutting_down while holding task_queue_lock.
# Shutdown broadcasts must use that same mutex or a wakeup can land in the
# predicate-check/cond_wait gap and leave the worker asleep during shutdown.
request_shutdown = function_slice(
    "void r2_request_shutdown(void)",
    "int r2_request_restart(void)",
)
request_lock = request_shutdown.find("pthread_mutex_lock(&task_queue_lock)")
request_state = request_shutdown.find("shutting_down = 1;")
request_wake = request_shutdown.find("pthread_cond_broadcast(&task_queue_cond)")
request_unlock = request_shutdown.find("pthread_mutex_unlock(&task_queue_lock)")
require(
    request_lock >= 0 and request_state > request_lock
    and request_wake > request_state and request_unlock > request_wake,
    "public shutdown changes the worker predicate and wakes it under the queue mutex",
)
shutdown_start = core.find("void r2_shutdown(void)")
shutdown_tail = core[shutdown_start:] if shutdown_start >= 0 else ""
shutdown_signal = shutdown_tail.find("r2_request_shutdown();")
shutdown_drain = shutdown_tail.find("pthread_mutex_lock(&conversation_turn_lock)")
shutdown_release = shutdown_tail.find("pthread_mutex_unlock(&conversation_turn_lock)", shutdown_drain)
shutdown_log = shutdown_tail.find('r2_log_system_snapshot("shutdown_begin")')
require(
    0 <= shutdown_signal < shutdown_drain < shutdown_release < shutdown_log,
    "core uses the shared worker cancellation path before draining active turns and writing shutdown events",
)
hands_join = shutdown_tail.find("pthread_join(hands_thread, NULL)")
pending_drain = shutdown_tail.find("Task *pending_tasks = task_head")
require(
    hands_join >= 0 and pending_drain > hands_join
    and "task_head = NULL;" in shutdown_tail[pending_drain:]
    and "task_tail = NULL;" in shutdown_tail[pending_drain:]
    and "free(pending_tasks->command);" in shutdown_tail[pending_drain:],
    "shutdown reclaims queued task nodes after the Hands worker stops without executing them",
)
init_start = core.find("int r2_init(void)")
shutdown_start = core.find("void r2_shutdown(void)", init_start)
init_body = core[init_start:shutdown_start] if init_start >= 0 and shutdown_start > init_start else ""
diary_create = init_body.find("r2_startup_thread_create(&diary_thread")
failure_start = diary_create - len("if (")
failure_end = init_body.find("pthread_join(hands_thread, NULL);", failure_start)
init_failure = init_body[failure_start:failure_end] if failure_start >= 0 and failure_end > failure_start else ""
init_lock = init_failure.find("pthread_mutex_lock(&task_queue_lock)")
init_state = init_failure.find("shutting_down = 1;")
init_wake = init_failure.find("pthread_cond_broadcast(&task_queue_cond)")
init_unlock = init_failure.find("pthread_mutex_unlock(&task_queue_lock)")
require(
    init_lock >= 0 and init_state > init_lock and init_wake > init_state
    and init_unlock > init_wake,
    "initialization failure changes the worker predicate and wakes it before joining",
)

# Keep action side effects and their history representation paired; never
# preserve raw model control syntax as a future instruction.
require(
    'message_add_pair_locked(\n                    "assistant", tool_request_context, "user", tagged_tool_result)'
    in impl
    and "[R2_INTERNAL_TOOL_RESULT]" in impl
    and "raw [WORLD]/[RUN]/etc. commands into later model context" in impl,
    "executed tool actions are recorded as an atomic tagged result, not replayable raw commands",
)

start_background = core.find("static char *start_background_task(")
end_background = core.find("/* ============================================================\n   COMPLETED TASKS", start_background)
background = core[start_background:end_background] if start_background >= 0 and end_background > start_background else ""
background_lock = background.find("pthread_mutex_lock(\n        &task_queue_lock")
background_guard = background.find("if (shutting_down)")
background_unlock = background.find("pthread_mutex_unlock(&task_queue_lock)", background_guard)
require(
    background_lock >= 0 and background_guard > background_lock
    and background_unlock > background_guard and "free(id)" in background[background_guard:],
    "background task submission refuses late work after shutdown has begun",
)

print("PASS: conversation recovery and serialization regression suite")
