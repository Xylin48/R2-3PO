#!/usr/bin/env python3
"""Static regression checks for R2's Imagination and Sensory Journal wiring."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
core = (ROOT / "r2.c").read_text(encoding="utf-8")
log = (ROOT / "Log.c").read_text(encoding="utf-8")
reality = (ROOT / "Reality.c").read_text(encoding="utf-8")
reward = (ROOT / "Reward.c").read_text(encoding="utf-8")
addiction = (ROOT / "Addiction.c").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")

for path in ("Imagination.c", "Imagination.h", "SensoryJournal.c", "SensoryJournal.h"):
    assert (ROOT / path).is_file(), f"missing subsystem file: {path}"

assert '#include "Imagination.h"' in core
assert "r2_imagination_init()" in core and "r2_imagination_shutdown();" in core

# A failed persistent-history query must not be represented as zero points or
# an empty repeated-interest history.
reward_start = reward.find("char *r2_reward_context(void)")
reward_end = reward.find("\nint r2_reward_review_diary(", reward_start)
if reward_start < 0 or reward_end < 0:
    raise SystemExit("Could not locate reward context report.")
reward_context = reward[reward_start:reward_end]
for marker in (
    "if (rc != SQLITE_ROW)",
    "if (!omitted && !failed && rc != SQLITE_DONE)",
    "Reward context unavailable: the cumulative reward total could not be read.",
    "Reward context unavailable: recent reward events could not be read completely.",
):
    if marker not in reward_context:
        raise SystemExit(f"Reward context masks an incomplete read: {marker!r}")

addiction_start = addiction.find("char *r2_addiction_report(void)")
if addiction_start < 0:
    raise SystemExit("Could not locate Addiction context report.")
addiction_context = addiction[addiction_start:]
for marker in (
    "if (rc != SQLITE_OK)",
    "if (!omitted && !failed && rc != SQLITE_DONE)",
    "Addiction context unavailable: learning records could not be read completely.",
):
    if marker not in addiction_context:
        raise SystemExit(f"Addiction context masks an incomplete read: {marker!r}")

reality_init_start = reality.find("int r2_reality_init(void)")
reality_init_end = reality.find("\nvoid r2_reality_shutdown(void)", reality_init_start)
if reality_init_start < 0 or reality_init_end < 0:
    raise SystemExit("Could not locate Reality-owned Addiction lifecycle.")
reality_init = reality[reality_init_start:reality_init_end]
if "if (r2_addiction_init() != 0)" not in reality_init or "r2_reality_shutdown();" not in reality_init:
    raise SystemExit("Reality must propagate Addiction initialization failure instead of continuing without preference history.")
reality_shutdown_start = reality.find("void r2_reality_shutdown(void)")
reality_shutdown_end = reality.find("\nint r2_reality_is_initialized(void)", reality_shutdown_start)
if reality_shutdown_start < 0 or reality_shutdown_end < 0:
    raise SystemExit("Could not locate Reality shutdown ownership.")
if "r2_addiction_shutdown();" not in reality[reality_shutdown_start:reality_shutdown_end]:
    raise SystemExit("Reality must close the Addiction database it owns.")
if "r2_addiction_init()" in core or "r2_addiction_shutdown();" in core:
    raise SystemExit("Core must not duplicate the Addiction lifecycle owned by Reality.")
reality_update_start = reality.find("static int update_hunger_locked(")
reality_update_end = reality.find("\nstatic void sync_room_mirrors", reality_update_start)
if reality_update_start < 0 or reality_update_end < 0:
    raise SystemExit("Could not locate Reality metric update transaction.")
reality_update = reality[reality_update_start:reality_update_end]
for bind in (
    "rc = sqlite3_bind_double(st, 1, hunger)",
    "rc = sqlite3_bind_double(st, 5, satisfaction)",
    "rc = sqlite3_bind_int64(st, 1, (sqlite3_int64)llround(elapsed))",
):
    if bind not in reality_update:
        raise SystemExit(f"Reality metric persistence ignores a SQLite bind result: {bind}")
sleep_start = reality.find("int r2_reality_sleep(")
sleep_end = reality.find("\nint r2_reality_record_dream(", sleep_start)
if sleep_start < 0 or sleep_end < 0:
    raise SystemExit("Could not locate Reality sleep update.")
sleep_update = reality[sleep_start:sleep_end]
for bind in ("rc = sqlite3_bind_double(st, 1, hours * 12.5)",
             "rc = sqlite3_bind_double(st, 2, hours * 10.0)"):
    if bind not in sleep_update:
        raise SystemExit(f"Reality sleep persistence ignores a SQLite bind result: {bind}")

reality_tick_start = reality.find("int r2_reality_tick(void)")
reality_tick_end = reality.find("\nint r2_reality_", reality_tick_start + 1)
if reality_tick_start < 0 or reality_tick_end < 0:
    raise SystemExit("Could not locate Reality tick transaction.")
reality_tick = reality[reality_tick_start:reality_tick_end]
for bind in (
    "rc = sqlite3_bind_int64(st, 1, last)",
    "rc = sqlite3_bind_int64(st, 2, now)",
    "rc = sqlite3_bind_int64(st, 3, now - last)",
    "rc = sqlite3_bind_int64(st, 2, now - 30 * 86400)",
    "rc = sqlite3_bind_int64(st, 2, now - 7 * 86400)",
):
    if bind not in reality_tick:
        raise SystemExit(f"Reality tick ignores a SQLite binding result: {bind}")

tv_start = reality.find("int r2_reality_tv_connect(")
tv_end = reality.find("\nint r2_reality_tv_disconnect(", tv_start)
if tv_start < 0 or tv_end < 0:
    raise SystemExit("Could not locate Reality TV device write path.")
tv_connect = reality[tv_start:tv_end]
for bind in (
    'rc = bind_text(st, 1, name) == 0 ? SQLITE_OK : SQLITE_ERROR',
    'rc = bind_text(st, 2, rf ? "rf" : "input") == 0 ? SQLITE_OK : SQLITE_ERROR',
    "rc = sqlite3_bind_int(st, 3, port)",
):
    if bind not in tv_connect:
        raise SystemExit(f"Reality TV device write ignores a binding result: {bind}")

assert 'extract_marker(reply, "[IMAGINE]"' in core
assert 'extract_marker(reply, "[IMAGINE_FEEDBACK]"' in core
assert "[END IMAGINE]" in core and "[END IMAGINE_FEEDBACK]" in core
assert "r2_imagination_create(trim(request))" in core
assert "r2_imagination_feedback(branch_id, assessment, notes)" in core

assert '#include "SensoryJournal.h"' in core
assert "r2_sj_init()" in core and "r2_sj_shutdown();" in core
for init_failure in (
    'R2_INIT_FAIL("could not initialize R2 Sensory Journal")',
    'R2_INIT_FAIL("could not initialize R2 reward ledger")',
    'R2_INIT_FAIL("could not initialize R2 Visual Experience Library")',
    'R2_INIT_FAIL("could not initialize R2 Alternate-Self Lab")',
    'R2_INIT_FAIL("could not initialize R2 Imagination")',
):
    assert init_failure in core, f"core startup silently degrades instead of requiring {init_failure}"
assert "r2_sj_recent((int)limit)" in core
assert "r2_sj_search(arg1, (int)limit)" in core
memory_save_start = core.find("int save_memory(")
memory_save_end = core.find("\nstatic ", memory_save_start + 1)
if memory_save_start < 0 or memory_save_end < 0:
    raise SystemExit("Could not locate persistent memory save implementation.")
memory_save = core[memory_save_start:memory_save_end]
for bind in (
    "rc = sqlite3_bind_text(st, 1, now, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_text(st, 2, memory, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_text(st, 3, category, -1, SQLITE_TRANSIENT)",
):
    if bind not in memory_save:
        raise SystemExit(f"Persistent memory update ignores a SQLite binding result: {bind}")

gameboy_link_start = core.find("static int64_t find_gameboy_life_event(")
gameboy_link_end = core.find("\n/* Pull only adapter-verified virtual events", gameboy_link_start)
if gameboy_link_start < 0 or gameboy_link_end < 0:
    raise SystemExit("Could not locate Game Boy Life Log deduplication lookup.")
gameboy_link = core[gameboy_link_start:gameboy_link_end]
if "lookup_rc = sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_TRANSIENT)" not in gameboy_link:
    raise SystemExit("Game Boy Life Log deduplication ignores the event lookup binding result.")

reward_modifier_start = reward.find("int r2_reward_current_modifier(")
reward_modifier_end = reward.find("\nchar *r2_reward_context(", reward_modifier_start)
if reward_modifier_start < 0 or reward_modifier_end < 0:
    raise SystemExit("Could not locate reward modifier lookup.")
reward_modifier = reward[reward_modifier_start:reward_modifier_end]
for bind in (
    "int rc = sqlite3_bind_text(st, 1, target, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_int64(st, 2, now)",
    "if (rc == SQLITE_OK) rc = sqlite3_step(st)",
):
    if bind not in reward_modifier:
        raise SystemExit(f"Reward modifier lookup ignores a SQLite binding result: {bind}")

assert "r2_sj_note(arg1" in core
assert 'extract_marker(reply, "[SENSORY_JOURNAL]"' in core

assert '#include "SensoryJournal.h"' in log
index_call = log.find("r2_sj_index_event(inserted_id")
unlock_call = log.find("pthread_mutex_unlock(&log_lock);", log.find("int64_t r2_log_event("))
assert index_call > unlock_call, "journal indexing must happen after releasing the Life Log mutex"
for category in ('"sensory"', '"thinking"', '"conversation"'):
    assert category in log, f"missing journal category: {category}"

assert "Imagination.c SensoryJournal.c" in makefile
assert "Imagination.h SensoryJournal.h" in makefile
assert '"$R2_SOURCE/Imagination.c"' in launcher
assert '"$R2_SOURCE/SensoryJournal.c"' in launcher
assert '"$R2_SOURCE/Imagination.h"' in launcher
assert '"$R2_SOURCE/SensoryJournal.h"' in launcher
assert "SensoryJournal.c" in workflow
assert "Imagination.c" in workflow

# The newer Reality-backed [WORLD] dispatcher remains the single authority for
# inventory, fridge, money, location, and TV state; no parallel World database.
assert 'extract_marker(reply, "[WORLD]"' in core
assert "r2_reality_move_item" in core and "r2_eat_and_learn" in core

# Ensure this integration retains current-turn identity and the reply-recovery path.
assert "conversation_turn_lock" in core
assert "r2_reply_reverses_speaker_roles" in core
assert 'if (strcmp(m->role, "user") == 0 &&\n            r2_recent_message_is_tool_payload(m->content))' not in core

log_event_exists_start = log.find("int r2_log_event_exists(")
log_event_exists_end = log.find("\nint64_t r2_log_event_with_memory(", log_event_exists_start)
if log_event_exists_start < 0 or log_event_exists_end < 0:
    raise SystemExit("Could not locate Life Log event deduplication query.")
log_event_exists = log[log_event_exists_start:log_event_exists_end]
for bind in (
    "rc = sqlite3_bind_text(statement, 1, event_type, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_text(statement, 2, source, -1, SQLITE_TRANSIENT)",
):
    if bind not in log_event_exists:
        raise SystemExit(f"Life Log event deduplication ignores a binding result: {bind}")

activity_start = log.find("int64_t r2_log_activity_start(")
activity_end = log.find("\nint64_t r2_log_activity_end(", activity_start)
if activity_start < 0 or activity_end < 0:
    raise SystemExit("Could not locate Life Log activity-start persistence.")
activity = log[activity_start:activity_end]
for bind in (
    "rc = sqlite3_bind_text(statement, 1, activity_key, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_text(statement, 2, activity_name, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_int64(statement, 7, (sqlite3_int64)event_id)",
):
    if bind not in activity:
        raise SystemExit(f"Life Log activity-start persistence ignores a binding result: {bind}")

activity_end_start = log.find("int64_t r2_log_activity_end(")
activity_end_end = log.find("\n/* ------------------------------------------------------------\n * PERSISTENT LOCATION TRANSITIONS", activity_end_start)
if activity_end_start < 0 or activity_end_end < 0:
    raise SystemExit("Could not locate Life Log activity-end persistence.")
activity_end_body = log[activity_end_start:activity_end_end]
for bind in (
    "rc = sqlite3_bind_text(statement, 1, utc, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_text(statement, 8, activity_key, -1, SQLITE_TRANSIENT)",
):
    if bind not in activity_end_body:
        raise SystemExit(f"Life Log activity-end persistence ignores a binding result: {bind}")

location_start = log.find("int r2_log_location_transition(")
location_end = log.find("\nint64_t r2_log_", location_start + 1)
if location_start < 0 or location_end < 0:
    raise SystemExit("Could not locate Life Log location persistence.")
location = log[location_start:location_end]
for bind in (
    "rc = sqlite3_bind_text(statement, 1, location, -1, SQLITE_TRANSIENT)",
    "rc = sqlite3_bind_int(statement, 3, is_home ? 0 : 1)",
):
    if bind not in location:
        raise SystemExit(f"Life Log location persistence ignores a binding result: {bind}")

print("Imagination, Sensory Journal, Reality, and conversation wiring smoke passed.")
