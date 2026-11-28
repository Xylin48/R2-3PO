#!/usr/bin/env python3
"""Regression checks for safe Game Boy CLI routing and TV control lifecycle."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
service = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
gameboy = (ROOT / "GameBoyAdvance.py").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")
log = (ROOT / "Log.c").read_text(encoding="utf-8")

start = shell.find("static int shell_gameboy(const char *arg)\n{")
end = shell.find("\nstatic int shell_parse_amount(", start)
assert start >= 0 and end > start, "single lifecycle-aware Game Boy handler must exist"
handler = shell[start:end]
assert "const char *device = r2_gameboy_console_path();" in handler
core_path_start = core.index("const char *r2_gameboy_console_path(void)")
core_path_end = core.index("\n}", core_path_start)
core_path = core[core_path_start:core_path_end]
assert core_path.index('getenv("R2_GAMEBOY_CONSOLE")') < core_path.index('getenv("R2_GAMEBOY_EXECUTABLE")')
assert 'R2_ROOT "/Devices/GameBoyAdvance/GameBoyAdvance"' in core_path
assert "const char *console_path = r2_gameboy_console_path();" in core
path_start = shell.index("static int shell_gameboy_state_database_path(")
path_end = shell.index("/* Read authoritative device state", path_start)
state_path = shell[path_start:path_end]
assert 'getenv("R2_GAMEBOY_ROOT")' in state_path
assert 'getenv("R2_GAMEBOY_DIR")' in state_path
assert "realpath(r2_gameboy_console_path(), NULL)" in state_path
assert '"%s%sState/console_state.db"' in state_path
status_start = shell.index("static int shell_gameboy_read_status(")
status_end = shell.index("static void shell_gameboy_log_session_start(", status_start)
assert "shell_gameboy_state_database_path(database, sizeof(database))" in shell[status_start:status_end]
for name in ("R2_GAMEBOY_CONSOLE", "R2_GAMEBOY_EXECUTABLE", "R2_GAMEBOY_ROOT", "R2_GAMEBOY_DIR"):
    assert name in launcher, f"interactive launcher must pass through {name}"
    assert name in service, f"overnight service must pass through {name}"
for runner in (launcher, service):
    assert 'R2_GAMEBOY_EXECUTABLE="${R2_GAMEBOY_CONSOLE:-${R2_GAMEBOY_EXECUTABLE:-}}"' in runner
    assert 'R2_GAMEBOY_CONSOLE="$R2_GAMEBOY_EXECUTABLE"' in runner
    assert 'realpath -m -- "$R2_GAMEBOY_EXECUTABLE"' in runner
    assert 'realpath -m -- "$R2_GAMEBOY_ROOT"' in runner
    assert 'R2_GAMEBOY_DIR="$R2_GAMEBOY_ROOT"' in runner
assert 'R2_GAMEBOY_ROOT="$R2_ROOT/Devices/GameBoyAdvance"' in launcher
assert '"R2_GAMEBOY_ROOT=$R2_GAMEBOY_ROOT"' in service
assert "execv(device, argv)" in handler
assert "system(" not in handler and "popen(" not in handler, "arguments must never enter a shell"
assert "r2_sync_gameboy_verified_events(device)" in handler
assert "shell_gameboy_log_session_start(&after)" in handler
assert "shell_gameboy_log_session_end(&before" in handler
assert shell.count("static int shell_gameboy(") == 1
assert 'shell_gameboy(shell_starts_with(command, "gameboy ") ? shell_trim(command + 8) : NULL);' in shell
for command in (
    '"status"', '"verify"', '"list"', '"power"', '"insert"',
    '"eject"', '"press"', '"move"', '"events"',
):
    assert command in gameboy, f"Game Boy CLI is missing {command}"
assert 'choices=("pockets", "shelf", "box", "room")' in gameboy

# Adapter event delivery is at-least-once until acknowledgement. Replay must
# reuse its durable Life Log record and apply the learning signal only once.
sync_start = core.index("static int gameboy_sync_verified_event(")
sync_end = core.index("\nstatic int r2_parse_control_positive_id", sync_start)
sync = core[sync_start:sync_end]
assert "r2_log_save_gameboy_once(source, event_id, summary, details," in sync
assert sync.index("r2_log_save_gameboy_once(") < sync.index("r2_reward_apply_once(")
assert "complete_legacy ? legacy_details : NULL" in sync
assert "json_object_to_json_string_ext(item, JSON_C_TO_STRING_PLAIN)" in sync
assert 'snprintf(reward_source, sizeof(reward_source), "game_event:%lld"' in sync
assert '"GameBoyAdvance.py:delivery:%s"' in sync
assert '"game_delivery:%s"' in sync
assert 'sqlite3_bind_int(st, 7, legacy_identity)' in log
assert sync.index("r2_reward_apply_once(") < sync.index('run_gameboy_console(path, "ack_event"')
assert 'run_gameboy_console(path, "ack_event", event_id_text, delivery_token, &ack_output)' in sync
assert sync.index('json_object_object_del(item, "delivery_token")') < sync.index("json_object_to_json_string_ext(")
assert 'run_gameboy_console(path, "events", NULL, NULL, &output)' in sync
assert "r2_sync_gameboy_verified_events(console_path)" in core
helper_start = log.index("static int64_t log_find_gameboy_locked(")
helper_end = log.index("/* A sampled source is evidence", helper_start)
helper = log[helper_start:helper_end]
assert "event_type='virtual_game_event' AND source COLLATE BINARY=?1" in helper
assert "event_type='gameboy_game_event' AND source='GameBoyAdvance'" in helper
assert "summary COLLATE BINARY=?2" in helper and "details COLLATE BINARY=?3" in helper
assert "sqlite3_step(st) != SQLITE_DONE" in helper
assert 'exec_sql("BEGIN IMMEDIATE;")' in helper and "log_finish_write(id > 0)" in helper
assert "NULL, NULL) != id" in helper

assert "static void *tv_control_server(void *unused)" in shell
assert "static int tv_control_start(void)" in shell
assert "static void tv_control_stop_server(void)" in shell
assert "r2_reality_tv_power(1)" in shell
assert "r2_reality_tv_vcr_transport" in shell
assert 'shell_tv(shell_starts_with(command, "tv ") ? shell_trim(command + 3) : NULL);' in shell
assert "tv_control_start() != 0" in shell and "tv_control_stop_server();" in shell

print("R2 TV/VCR and Game Boy shell control-plane smoke passed.")
