#!/usr/bin/env python3
"""Guard atomic reward + idempotency-key persistence and crash recovery."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / "Reward.c").read_text(encoding="utf-8")
start = source.index("static int reward_apply_internal(")
end = source.index("\nint r2_reward_apply(", start)
body = source[start:end]

def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")

begin = body.index('sqlite3_exec(reward_db, "BEGIN IMMEDIATE;"')
key_insert = body.index('INSERT OR IGNORE INTO reward_once_keys')
event_insert = body.index('INSERT INTO reward_events')
total_update = body.index('UPDATE reward_totals SET points=points+?')
commit = body.index('sqlite3_exec(reward_db, "COMMIT;"')
require(begin < key_insert < event_insert < total_update < commit,
        "once-key, reward row, total, and commit must share one transaction")
require('FROM reward_events WHERE target=? AND source=? LIMIT 1' in body
        and 'prior_event.id = sqlite3_column_int64(st, 0)' in body,
        "retry must detect and reconstruct a reward row committed by an earlier attempt")
require("if (duplicate)" in body and "return 1;" in body,
        "already-recorded once events must not increment totals twice")
require("reward_deliver_addiction_choice(&prior_event)" in body,
        "duplicate retries must use durable and idempotent addiction-choice recovery")
require("prior_event.voluntary_choice = sqlite3_column_int(st, 8)" in body,
        "retry must recover the original voluntary choice marker from its reward row")
require('if (rc != SQLITE_OK) return -1;' in body,
        "database failures must propagate to the caller")
require("static int reward_apply_internal" in source
        and "return reward_apply_internal(target, source, points, reason," in source,
        "public reward APIs must share the atomic implementation")
require('SELECT 1 FROM addiction_events WHERE target=? COLLATE NOCASE ' in
        (Path(__file__).resolve().parents[1] / "Addiction.c").read_text(encoding="utf-8"),
        "reinforcement choice replay must deduplicate by stable reward event details")
require("reward_deliver_addiction_choice(&event)" in body
        and "addiction_choice_indexed" in source
        and "voluntary_choice INTEGER NOT NULL DEFAULT 0" in source,
        "persist original reward intent and mark completed cross-organ delivery")
require('OR (voluntary_choice=1 AND addiction_choice_indexed=0)' in source
        and "reward_deliver_addiction_choice(item)" in source,
        "startup reconciliation must repair pending Addiction events even when Life Log is indexed")
require("int r2_reward_reconnect_history(" in source
        and "find_existing_log_event(item->id)" in source,
        "reward history reconciliation must remain available after the refactor")
once_start = source.index("int r2_reward_apply_once(")
once_end = source.index("\nint r2_reward_current_modifier(", once_start)
once_body = source[once_start:once_end]
require("voluntary_choice, 1);" in once_body and "r2_reward_apply(" not in once_body,
        "idempotent rewards must not commit a key before a separate reward transaction")

print("Atomic reward idempotency smoke test passed.")
