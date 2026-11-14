#!/usr/bin/env python3
"""Smoke-test observer privacy filtering without a desktop."""
import importlib.util
from pathlib import Path
path = Path(__file__).resolve().parents[1] / "Observer.py"
source_text = path.read_text(encoding="utf-8")
poll_start = source_text.index("    def poll():")
poll_end = source_text.index("    def close():", poll_start)
poll = source_text[poll_start:poll_end]
assert "if not tree.exists(item_id):" in poll, "retries must not insert duplicate UI rows"
assert "last_id = 0" in poll and "reconcile_activity_page(" in poll and "last_id = next_cursor" in poll, (
    "the Activity file replays historical approved rows and only advances after full-page persistence"
)
assert "except OSError as exc:" in poll and "activity_file_error = True" in poll, (
    "a failed Activity.txt append leaves its page eligible for retry"
)
assert "/home/x/R2_Home/R2_Log/Activity.txt" in source_text
assert "/home/x/R2_Home/R2_Diary/Activity.txt" not in source_text
spec = importlib.util.spec_from_file_location("r2_observer", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
assert module.is_observer_event("world", "object_moved")
assert module.is_observer_event("world", "creator_gift")
assert module.is_observer_event("world", "enjoyment_changed")
assert module.is_observer_event("world", "addiction_status_changed")
assert module.is_observer_event("media", "activity_ended")
assert module.is_observer_event("world", "location_changed")
assert module.is_observer_event("world", "gameboy_device_action")
assert module.is_observer_event("world", "gameboy_game_started")
assert module.is_observer_event("world", "gameboy_game_event")
assert module.is_observer_event("world", "tv_display_opened")
assert module.is_observer_event("world", "tv_display_closed")
assert module.is_observer_event("world", "tv_power")
assert module.is_observer_event("world", "vcr_tape_inserted")
assert module.is_observer_event("world", "vcr_transport_changed")
assert module.is_observer_event("world", "vcr_tape_ejected")
assert not module.is_observer_event("world", "controller_input")
assert not module.is_observer_event("conversation", "conversation_turn")
assert not module.is_observer_event("thinking", "thought")
assert not module.is_observer_event("thinking", "diary_entry_linked")
assert not module.is_observer_event("world", "internal_notification")
assert not module.is_observer_event("world", "unknown_new_event")
print("Observer allowlist smoke test passed.")

from pathlib import Path
import tempfile

with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "R2_Diary" / "Activity.txt"
    module.append_activity_line(output, "2026-10-09 09:15:00", "R2 moved to kitchen.", 17)
    module.append_activity_line(output, "2026-10-09 09:16:00", "R2 picked up a book.", 18)
    # Retrying the same event after a reported write error must not duplicate it.
    module.append_activity_line(output, "2026-10-09 09:16:00", "R2 picked up a book.", 18)
    contents = output.read_text(encoding="utf-8")
    assert contents.count("R2-3PO Activity Log") == 1
    assert contents.count("(event #18)") == 1
    assert "[2026-10-09 09:15:00] R2 moved to kitchen. (event #17)" in contents
    assert "[2026-10-09 09:16:00] R2 picked up a book. (event #18)" in contents
    assert contents.index("R2 moved to kitchen") < contents.index("R2 picked up a book")
print("Activity.txt append smoke test passed.")

import sqlite3

# A closed Observer does not imply its existing Life Log events vanish. Replay
# every permitted event without exposing private entries or rewriting lines.
with tempfile.TemporaryDirectory() as directory:
    output = Path(directory) / "R2_Log" / "Activity.txt"
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.execute("CREATE TABLE r2_log_events "
               "(id INTEGER PRIMARY KEY,local_time TEXT,category TEXT,event_type TEXT,summary TEXT)")
    db.executemany("INSERT INTO r2_log_events VALUES(?,?,?,?,?)", [
        (1, "2026-11-14", "world", "object_added", "First public event"),
        (2, "2026-11-14", "thinking", "thought", "PRIVATE THOUGHT"),
        (3, "2026-11-14", "world", "object_moved", "Second public event"),
        (4, "2026-11-14", "world", "object_added", "Summary quotes (event #99) but is event four"),
        (5, "2026-11-14", "world", "internal_notification", "PRIVATE NOTIFICATION"),
        (99, "2026-11-14", "media", "activity_ended", "Independent event ninety-nine"),
    ])
    writer = module.ActivityWriter(output)
    cursor = 0
    while cursor < 99:
        cursor, _ = module.reconcile_activity_page(db, writer, cursor, 99, batch_size=2)
    contents = output.read_text(encoding="utf-8")
    for event_id in (1, 3, 4, 99):
        assert contents.count(f"(event #{event_id})\\n") == 1
    assert "PRIVATE" not in contents
    assert contents.index("(event #1)\\n") < contents.index("(event #3)\\n")
    assert contents.index("(event #4)\\n") < contents.index("(event #99)\\n")

    # Simulate closing the popup with two more events written to the Life Log.
    db.execute("INSERT INTO r2_log_events VALUES(100,'2026-11-14','world','object_added','While popup closed')")
    db.execute("INSERT INTO r2_log_events VALUES(101,'2026-11-14','conversation','conversation_turn','PRIVATE DIALOGUE')")
    writer = module.ActivityWriter(output)
    cursor = 0
    while cursor < 101:
        cursor, _ = module.reconcile_activity_page(db, writer, cursor, 101, batch_size=2)
    contents = output.read_text(encoding="utf-8")
    for event_id in (1, 3, 4, 99, 100):
        assert contents.count(f"(event #{event_id})\\n") == 1
    assert "While popup closed" in contents and "PRIVATE DIALOGUE" not in contents

    # A crash after a partial line cannot cause the next record to overwrite
    # the torn bytes, misidentify its event ID, or invent a repaired record.
    with output.open("ab") as stream:
        stream.write(b"[partial unsynced event ")
    before = output.read_bytes()
    try:
        writer.append("2026-11-14", "must not publish over incomplete history", 102)
    except OSError:
        pass
    else:
        raise AssertionError("a partial final Activity line was accepted")
    assert output.read_bytes() == before
    db.close()
print("Observer restart backfill, full-file dedupe, and torn-log refusal passed.")
