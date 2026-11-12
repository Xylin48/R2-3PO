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
key_insert = body.index('INSERT INTO reward_once_keys')
event_insert = body.index('INSERT INTO reward_events')
key_link = body.index("UPDATE reward_once_keys SET reward_event_id=?", event_insert)
total_update = body.index('UPDATE reward_totals SET points=points+?')
commit = body.index('sqlite3_exec(reward_db, "COMMIT;"')
require(begin < key_insert < event_insert < key_link < total_update < commit,
        "once-key, reward row, total, and commit must share one transaction")
require('FROM reward_events WHERE id=? AND target=? AND source=?' in body
        and 'prior_event.id = sqlite3_column_int64(st, 0)' in body,
        "retry must detect and reconstruct a reward row committed by an earlier attempt")
require("if (duplicate)" in body and "return 1;" in body,
        "already-recorded once events must not increment totals twice")
require('if (rc != SQLITE_OK) return -1;' in body,
        "database failures must propagate to the caller")
require("static int reward_apply_internal" in source
        and "return reward_apply_internal(target, source, points, reason," in source,
        "public reward APIs must share the atomic implementation")
require('SELECT 1 FROM addiction_events WHERE target=? COLLATE NOCASE ' in
        (Path(__file__).resolve().parents[1] / "Addiction.c").read_text(encoding="utf-8"),
        "reinforcement choice replay must deduplicate by stable reward event details")
require("reward_deliver_addiction_choice(&prior_event)" in body
        and "reward_deliver_addiction_choice(item)" in source,
        "once retries and startup must both recover pending Addiction delivery")
require("int r2_reward_reconnect_history(" in source
        and "find_existing_log_event(item->id)" in source,
        "reward history reconciliation must remain available after the refactor")
once_start = source.index("int r2_reward_apply_once(")
once_end = source.index("\nint r2_reward_current_modifier(", once_start)
once_body = source[once_start:once_end]
require("voluntary_choice, 1);" in once_body and "r2_reward_apply(" not in once_body,
        "idempotent rewards must not commit a key before a separate reward transaction")

print("Atomic reward idempotency smoke test passed.")
