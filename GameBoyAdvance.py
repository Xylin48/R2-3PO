#!/usr/bin/env python3
"""R2's persistent virtual Game Boy console.

This is the device layer, not an emulator. It delegates game execution to mGBA.
The cartridge slot lives in gameboy.db; console/session state and event history
live in State/console_state.db. Older combined gameboy.db state is imported
transactionally and retained in the source database for recovery.
No emulator save-state or rewind commands are exposed by this interface.
"""
from __future__ import annotations

import argparse
import fcntl
import json
import os
import select
import signal
import shutil
import sqlite3
import subprocess
import sys
import time
from contextlib import closing, contextmanager, nullcontext
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

ROOT = Path(os.environ.get("R2_GAMEBOY_ROOT", os.environ.get("R2_GAMEBOY_DIR", str(Path(__file__).resolve().parent)))).resolve()
CARTRIDGES = ROOT / "Cartridges"
SAVES = ROOT / "Saves"
STATE = ROOT / "State"
DB_PATH = Path(os.environ.get("R2_GAMEBOY_DB", str(ROOT / "gameboy.db")))
STATE_DB_PATH = STATE / "console_state.db"
EMULATOR = os.environ.get("R2_MGBA_EXECUTABLE", "/usr/games/mgba-qt")
ROM_EXTENSIONS = {".gba", ".gb", ".gbc"}
VERIFIED_GAME_EVENT_TYPES = {
    "character_jumped", "coin_collected", "item_collected", "life_gained",
    "life_lost", "damage_taken", "level_started", "level_completed",
    "game_completed", "screen_transition",
}
BUTTON_KEYS = {
    "A": "x", "B": "z", "L": "a", "R": "s",
    "START": "Return", "SELECT": "BackSpace",
    "UP": "Up", "DOWN": "Down", "LEFT": "Left", "RIGHT": "Right",
}
DEFAULT_PRESS_MS = 120
MAX_PRESS_MS = 5000

# Keep newly created DB/WAL/save files group-writable for the shared r2 group.
os.umask(0o002)


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def ensure_dirs() -> None:
    for path in (CARTRIDGES, SAVES, STATE, STATE / "mgba-config" / "mgba",
                 DB_PATH.parent, STATE_DB_PATH.parent):
        path.mkdir(parents=True, exist_ok=True)


@contextmanager
def console_lock() -> Iterator[None]:
    STATE.mkdir(parents=True, exist_ok=True)
    with (STATE / ".console.lock").open("a+b") as lock:
        deadline = time.monotonic() + 3.0
        while True:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except BlockingIOError as exc:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("The Game Boy Advance is busy. Retry the command.") from exc
                time.sleep(min(0.025, remaining))
        yield


def _table_columns(db: sqlite3.Connection, schema: str, table: str) -> list[str]:
    # Schema/table names are fixed internal identifiers, never user input.
    return [str(row["name"]) for row in
            db.execute(f'PRAGMA {schema}.table_info("{table}")').fetchall()]


def _quoted_columns(columns: list[str]) -> str:
    return ", ".join('"' + name.replace('"', '""') + '"' for name in columns)


def _copy_legacy_row(db: sqlite3.Connection, table: str, columns: list[str],
                     values: list[Any], key: str, identity: Any) -> None:
    # Table and key identifiers are fixed internal names. Values (including
    # the exact legacy payload) are always bound, not interpolated.
    quoted = _quoted_columns(columns)
    placeholders = ", ".join("?" for _ in columns)
    inserted = db.execute(
        f'INSERT INTO main."{table}" ({quoted}) VALUES ({placeholders})', values,
    )
    if identity is None and key == "id":
        identity = inserted.lastrowid
    actual = db.execute(
        f'SELECT {quoted} FROM main."{table}" WHERE "{key}"=?', (identity,),
    ).fetchone()
    if (inserted.rowcount != 1 or identity is None or actual is None
            or tuple(actual) != tuple(values)):
        raise sqlite3.IntegrityError(f"Legacy {table} row was not copied exactly.")


def _migrate_legacy_combined_database(db: sqlite3.Connection) -> None:
    """Import old state tables from gameboy.db without deleting source data.

    Earlier releases kept cartridge, console state, inventory, and event history
    together in gameboy.db. Current releases split state into State/console_state.db.
    Existing destination state/inventory rows own their identity; events retain
    both histories when IDs collide. The source tables remain a recovery copy.
    """
    migration_key = f"split_legacy_gameboy_db_v1:{DB_PATH.resolve()}"
    db.execute(
        "CREATE TABLE IF NOT EXISTS console_migrations ("
        "migration_key TEXT PRIMARY KEY, completed_at TEXT NOT NULL)"
    )
    db.execute("BEGIN IMMEDIATE")
    try:
        if db.execute(
            "SELECT 1 FROM console_migrations WHERE migration_key=?", (migration_key,)
        ).fetchone():
            db.commit()
            return

        legacy_tables = {
            table for table in ("console_state", "console_events", "cartridge_inventory")
            if db.execute(
                "SELECT 1 FROM cartridge.sqlite_master WHERE type='table' AND name=?",
                (table,),
            ).fetchone()
        }
        # Fresh cartridge-only databases do not receive a migration marker.
        if not legacy_tables:
            db.commit()
            return

        for table, key in (("console_state", "id"), ("cartridge_inventory", "filename")):
            if table not in legacy_tables:
                continue
            source_columns = set(_table_columns(db, "cartridge", table))
            target_columns = _table_columns(db, "main", table)
            common = [name for name in target_columns if name in source_columns]
            if key not in common:
                raise sqlite3.IntegrityError(f"Legacy {table} lacks its {key} identity.")
            quoted = _quoted_columns(common)
            for row in db.execute(f'SELECT {quoted} FROM cartridge."{table}"'):
                identity = row[key]
                if identity is None:
                    raise sqlite3.IntegrityError(f"Legacy {table} has a missing {key} identity.")
                # Preserve established destination state and physical location
                # rather than overwriting them with the older recovery copy.
                if db.execute(
                    f'SELECT 1 FROM main."{table}" WHERE "{key}"=?', (identity,),
                ).fetchone():
                    continue
                columns = list(common)
                values = [row[name] for name in columns]
                if table == "cartridge_inventory":
                    # Older combined databases may contain only the filename.
                    # Use the same neutral metadata as an upgraded state DB,
                    # even when the destination has fresh NOT NULL columns
                    # without defaults. Never replace a field present in the
                    # source with an inferred value.
                    for name, default in (
                        ("title", ""), ("location", "pockets"),
                        ("discovered_at", ""), ("updated_at", ""),
                    ):
                        if name not in source_columns:
                            columns.append(name)
                            values.append(default)
                _copy_legacy_row(db, table, columns, values, key, identity)

        if "console_events" in legacy_tables:
            source_columns = _table_columns(db, "cartridge", "console_events")
            target_columns = _table_columns(db, "main", "console_events")
            common = [name for name in target_columns
                      if name in source_columns and name != "id"]
            if not all(name in common for name in ("timestamp", "context", "event_type", "summary")):
                raise sqlite3.IntegrityError("Legacy console_events lacks required event fields.")
            has_id = "id" in source_columns
            order = "id" if has_id else "timestamp"
            # Allocate collision IDs beyond BOTH histories. A remap must not
            # occupy an unrelated source event's original, unused ID.
            largest_id = db.execute("SELECT COALESCE(MAX(id),0) FROM main.console_events").fetchone()[0]
            if has_id:
                invalid_id = db.execute(
                    "SELECT 1 FROM cartridge.console_events WHERE id IS NOT NULL "
                    "AND typeof(id)!='integer' LIMIT 1"
                ).fetchone()
                if invalid_id:
                    raise sqlite3.IntegrityError("Legacy console event identity is not an integer.")
                duplicate_id = db.execute(
                    "SELECT id FROM cartridge.console_events WHERE id IS NOT NULL "
                    "GROUP BY id HAVING COUNT(*)>1 LIMIT 1"
                ).fetchone()
                if duplicate_id:
                    raise sqlite3.IntegrityError("Legacy console event identity is duplicated.")
                source_max = db.execute("SELECT COALESCE(MAX(id),0) FROM cartridge.console_events").fetchone()[0]
                largest_id = max(largest_id, source_max, 0)
            for row in db.execute(f'SELECT * FROM cartridge.console_events ORDER BY {order}'):
                source_id = row["id"] if has_id else None
                if source_id is not None:
                    existing = db.execute(
                        "SELECT * FROM main.console_events WHERE id=?", (source_id,),
                    ).fetchone()
                    if existing is not None and all(existing[name] == row[name] for name in common):
                        continue
                else:
                    existing = None
                if source_id is None or existing is not None:
                    if largest_id >= 9223372036854775807:
                        raise sqlite3.IntegrityError("Legacy console event ID space is exhausted.")
                    largest_id += 1
                    destination_id = largest_id
                else:
                    destination_id = source_id
                _copy_legacy_row(db, "console_events", ["id"] + common,
                                 [destination_id] + [row[name] for name in common],
                                 "id", destination_id)

        completed_at = utc_now()
        marker = db.execute(
            "INSERT INTO console_migrations(migration_key, completed_at) VALUES(?,?)",
            (migration_key, completed_at),
        )
        actual_marker = db.execute(
            "SELECT migration_key, completed_at FROM console_migrations WHERE migration_key=?",
            (migration_key,),
        ).fetchone()
        if marker.rowcount != 1 or actual_marker is None or tuple(actual_marker) != (migration_key, completed_at):
            raise sqlite3.IntegrityError("Legacy console migration marker was not recorded exactly.")
        db.commit()
    except BaseException:
        db.rollback()
        raise


def connect() -> sqlite3.Connection:
    ensure_dirs()
    db = sqlite3.connect(STATE_DB_PATH, timeout=5.0)
    try:
        _initialize_connection(db)
        return db
    except BaseException:
        db.close()
        raise


def _initialize_connection(db: sqlite3.Connection) -> None:
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA busy_timeout=5000")
    # Cartridge insertion/ejection updates the attached gameboy.db and the
    # console event/inventory tables in console_state.db in one transaction.
    # WAL guarantees rollback on SQL errors, but SQLite explicitly does not
    # guarantee multi-file atomicity after a power loss in WAL mode. Use the
    # rollback super-journal for both files and refuse to operate if an older
    # WAL database cannot be safely switched (for example, active readers).
    main_mode = db.execute("PRAGMA main.journal_mode=DELETE").fetchone()[0]
    if main_mode.lower() != "delete":
        raise sqlite3.OperationalError(
            f"Console state journal remains {main_mode}; cross-file commit is unsafe"
        )
    # gameboy.db may be an older combined database. Its legacy state tables are
    # imported transactionally below; the original tables remain untouched.
    db.execute("ATTACH DATABASE ? AS cartridge", (str(DB_PATH),))
    cartridge_mode = db.execute("PRAGMA cartridge.journal_mode=DELETE").fetchone()[0]
    if cartridge_mode.lower() != "delete":
        raise sqlite3.OperationalError(
            f"Cartridge journal remains {cartridge_mode}; cross-file commit is unsafe"
        )
    db.execute("PRAGMA main.synchronous=FULL")
    db.execute("PRAGMA cartridge.synchronous=FULL")
    db.executescript("""
        CREATE TABLE IF NOT EXISTS cartridge.cartridge_slot (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            rom_path TEXT,
            rom_title TEXT,
            inserted_at TEXT
        );
        INSERT OR IGNORE INTO cartridge.cartridge_slot(id) VALUES (1);
        CREATE TABLE IF NOT EXISTS console_state (
            id INTEGER PRIMARY KEY CHECK (id = 1),
            power_state TEXT NOT NULL DEFAULT 'off'
                CHECK (power_state IN ('off','on')),
            emulator_pid INTEGER,
            emulator_start_ticks INTEGER,
            game_started_at TEXT,
            session_id TEXT
        );
        CREATE TABLE IF NOT EXISTS console_events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            context TEXT NOT NULL CHECK (context IN ('physical','virtual','system')),
            event_type TEXT NOT NULL,
            summary TEXT NOT NULL,
            details TEXT,
            game_title TEXT,
            session_id TEXT,
            evidence_source TEXT,
            verified INTEGER NOT NULL DEFAULT 0,
            life_log_synced INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS console_events_time_idx
            ON console_events(id);
        CREATE TABLE IF NOT EXISTS cartridge_inventory (
            filename TEXT PRIMARY KEY COLLATE NOCASE,
            title TEXT NOT NULL,
            location TEXT NOT NULL DEFAULT 'pockets'
                CHECK (location IN ('pockets','shelf','box','room','slot')),
            discovered_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
    """)
    # The separate cartridge database can also outlive the code version
    # that created it. Add the slot's current metadata fields in place.
    slot_columns = {row[1] for row in db.execute(
        "PRAGMA cartridge.table_info(cartridge_slot)"
    )}
    for name, declaration in (
        ("rom_path", "TEXT"),
        ("rom_title", "TEXT"),
        ("inserted_at", "TEXT"),
    ):
        if name not in slot_columns:
            db.execute(
                f"ALTER TABLE cartridge.cartridge_slot ADD COLUMN {name} {declaration}"
            )

    # Older releases already had the console_state table, so CREATE TABLE IF
    # NOT EXISTS does not add fields introduced by newer runtime versions.
    # Add only missing columns in place and preserve the existing state row.
    state_columns = {row[1] for row in db.execute("PRAGMA table_info(console_state)")}
    for name, declaration in (
        ("emulator_pid", "INTEGER"),
        ("emulator_start_ticks", "INTEGER"),
        ("game_started_at", "TEXT"),
        ("session_id", "TEXT"),
    ):
        if name not in state_columns:
            db.execute(f"ALTER TABLE console_state ADD COLUMN {name} {declaration}")

    # Older inventory tables are upgraded additively too. Neutral defaults
    # keep existing cartridges visible without inventing a location history.
    inventory_columns = {row[1] for row in db.execute("PRAGMA table_info(cartridge_inventory)")}
    for name, declaration in (
        ("title", "TEXT NOT NULL DEFAULT ''"),
        ("location", "TEXT NOT NULL DEFAULT 'pockets'"),
        ("discovered_at", "TEXT NOT NULL DEFAULT ''"),
        ("updated_at", "TEXT NOT NULL DEFAULT ''"),
    ):
        if name not in inventory_columns:
            db.execute(f"ALTER TABLE cartridge_inventory ADD COLUMN {name} {declaration}")

    # Event provenance fields were added after the initial console event log.
    columns = {row[1] for row in db.execute("PRAGMA table_info(console_events)")}
    for name, declaration in (
        ("evidence_source", "TEXT"),
        ("verified", "INTEGER NOT NULL DEFAULT 0"),
        ("life_log_synced", "INTEGER NOT NULL DEFAULT 0"),
    ):
        if name not in columns:
            db.execute(f"ALTER TABLE console_events ADD COLUMN {name} {declaration}")
    # Import old combined-database state before creating a default state row.
    # An existing destination state row remains authoritative.
    _migrate_legacy_combined_database(db)
    db.execute("INSERT OR IGNORE INTO console_state(id, power_state) VALUES (1, 'off')")
    db.commit()


def event_payload(db: sqlite3.Connection, event_id: int) -> tuple | None:
    row = db.execute(
        "SELECT timestamp,context,event_type,summary,details,game_title,session_id,"
        "evidence_source,verified,life_log_synced FROM console_events WHERE id=?",
        (event_id,),
    ).fetchone()
    return tuple(row) if row is not None else None


def record_event(db: sqlite3.Connection, context: str, event_type: str,
                 summary: str, details: str | None = None,
                 game_title: str | None = None, session_id: str | None = None,
                 evidence_source: str | None = None, verified: bool = False,
                 *, commit: bool = True) -> int:
    expected = (utc_now(), context, event_type, summary, details, game_title,
                session_id, evidence_source, 1 if verified else 0, 0)
    with (db if commit else nullcontext()):
        inserted = db.execute(
            "INSERT INTO console_events(timestamp,context,event_type,summary,details,game_title,session_id,"
            "evidence_source,verified,life_log_synced) VALUES(?,?,?,?,?,?,?,?,?,?)",
            expected,
        )
        event_id = inserted.lastrowid
        if (inserted.rowcount != 1 or event_id is None or event_id <= 0
                or event_payload(db, event_id) != expected):
            raise sqlite3.IntegrityError(f"Console event {event_type} was not recorded exactly.")
    return event_id


def record_verified_game_event(db: sqlite3.Connection, event_type: str,
                               summary: str, details: str,
                               evidence_source: str) -> int:
    """Record an event detected by a trusted game-specific adapter.

    This API is intentionally not exposed as an R2 [WORLD] action. A model
    statement alone is not evidence that a character performed an action.
    """
    with console_lock():
        row = reconcile(db)
        if row["power_state"] != "on" or not row["emulator_pid"]:
            raise ValueError("No active game session; virtual events cannot be recorded.")
        if event_type not in VERIFIED_GAME_EVENT_TYPES:
            raise ValueError("Unsupported verified game event type.")
        if not summary or len(summary) > 1000 or len(details or "") > 3000:
            raise ValueError("Event summary/details are empty or too long.")
        if not evidence_source or not evidence_source.startswith("adapter:"):
            raise ValueError("A game adapter evidence source is required.")
        return record_event(db, "virtual", event_type, summary, details,
                            row["cartridge_title"], row["session_id"],
                            evidence_source=evidence_source, verified=True)


def pending_verified_events(db: sqlite3.Connection) -> list[dict[str, Any]]:
    rows = db.execute(
        "SELECT id,timestamp,event_type,summary,details,game_title,session_id,evidence_source "
        "FROM console_events WHERE context='virtual' AND verified=1 AND life_log_synced=0 "
        "ORDER BY id"
    ).fetchall()
    return [{
        "event_id": row["id"], "timestamp": row["timestamp"],
        "event_type": row["event_type"], "summary": row["summary"],
        "details": row["details"], "game_title": row["game_title"],
        "session_id": row["session_id"], "evidence_source": row["evidence_source"],
        "context": "virtual", "verified": True,
    } for row in rows]


def acknowledge_verified_event(db: sqlite3.Connection, event_id: int) -> bool:
    with db:
        if not db.in_transaction:
            db.execute("BEGIN IMMEDIATE")
        previous = event_payload(db, event_id)
        if previous is None or previous[1] != "virtual" or previous[-2:] != (1, 0):
            return False
        cursor = db.execute(
            "UPDATE console_events SET life_log_synced=1 "
            "WHERE id=? AND context='virtual' AND verified=1 AND life_log_synced=0",
            (event_id,),
        )
        if cursor.rowcount != 1 or event_payload(db, event_id) != previous[:-1] + (1,):
            raise sqlite3.IntegrityError("Verified event acknowledgement was not recorded exactly.")
    return True


class ProcessIdentityError(RuntimeError):
    """The running process cannot be identified; absence has not been proved."""


def process_start_ticks(pid: int) -> int | None:
    """Read the Linux process identity without treating a read error as exit."""
    try:
        raw = Path(f"/proc/{pid}/stat").read_bytes()
    except (FileNotFoundError, ProcessLookupError):
        return None
    except OSError as exc:
        raise ProcessIdentityError(f"Cannot read emulator process {pid}: {exc}") from exc
    try:
        end = raw.rfind(b")")
        if end < 0:
            raise ValueError("missing process name terminator")
        tail = raw[end + 2:].split()
        # tail begins with proc stat field 3; starttime is field 22.
        if tail[0] in {b"Z", b"X", b"x"}:
            return None
        ticks = int(tail[19])
        if ticks < 0:
            raise ValueError("negative process start time")
        return ticks
    except (ValueError, IndexError) as exc:
        raise ProcessIdentityError(f"Malformed emulator process identity for {pid}.") from exc


def process_matches(pid: int | None, expected_ticks: int | None) -> bool:
    if not pid or pid <= 1:
        return False
    actual = process_start_ticks(pid)
    if actual is None:
        return False
    if not isinstance(expected_ticks, int) or expected_ticks < 0:
        raise ProcessIdentityError(f"Live emulator PID {pid} has no verified stored start time.")
    return actual == expected_ticks


def wait_for_emulator_exit(fd: int, timeout_ms: int) -> bool:
    poller = select.poll()
    poller.register(fd, select.POLLIN)
    events = poller.poll(timeout_ms)
    if any(flags & (select.POLLERR | select.POLLNVAL) for _, flags in events):
        raise ProcessIdentityError("Cannot observe the owned emulator process descriptor.")
    return any(flags & (select.POLLIN | select.POLLHUP) for _, flags in events)


def open_owned_emulator_pidfd(pid: int, expected_ticks: int | None) -> int | None:
    """Acquire a Linux process handle and verify it still owns the session."""
    if not process_matches(pid, expected_ticks):
        return None
    try:
        fd = os.pidfd_open(pid)
    except ProcessLookupError:
        return None
    except (AttributeError, OSError) as exc:
        raise ProcessIdentityError(f"Cannot acquire emulator process descriptor: {exc}") from exc
    try:
        if process_matches(pid, expected_ticks) and not wait_for_emulator_exit(fd, 0):
            return fd
    except BaseException:
        os.close(fd)
        raise
    os.close(fd)
    return None


def stop_owned_emulator(fd: int, pid: int) -> bool:
    """Request normal window closure, then signal only the acquired process."""
    if wait_for_emulator_exit(fd, 0):
        return True
    if shutil.which("xdotool"):
        try:
            found = subprocess.run(
                ["xdotool", "search", "--onlyvisible", "--pid", str(pid)],
                capture_output=True, text=True, timeout=2, check=False,
            )
            windows = [line.strip() for line in found.stdout.splitlines()
                       if line.strip().isascii() and line.strip().isdecimal()]
            if found.returncode == 0 and windows and not wait_for_emulator_exit(fd, 0):
                owner = subprocess.run(
                    ["xdotool", "getwindowpid", windows[0]],
                    capture_output=True, text=True, timeout=2, check=False,
                )
                if (owner.returncode == 0 and owner.stdout.strip() == str(pid)
                        and not wait_for_emulator_exit(fd, 0)):
                    subprocess.run(
                        ["xdotool", "windowclose", windows[0]],
                        check=False, timeout=3,
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                    )
                    if wait_for_emulator_exit(fd, 4000):
                        return True
        except (OSError, subprocess.SubprocessError):
            # A failed graphical close is not proof of exit. Continue the
            # existing termination sequence against the acquired descriptor.
            pass
    for sig in (signal.SIGTERM, signal.SIGKILL):
        if wait_for_emulator_exit(fd, 0):
            return True
        try:
            signal.pidfd_send_signal(fd, sig)
        except ProcessLookupError:
            pass
        except AttributeError as exc:
            raise ProcessIdentityError("Descriptor-based emulator signaling is unavailable.") from exc
        if wait_for_emulator_exit(fd, 2000):
            return True
    return False


def state_row(db: sqlite3.Connection) -> sqlite3.Row:
    row = db.execute(
        "SELECT s.*, c.rom_path AS cartridge_path, c.rom_title AS cartridge_title, "
        "c.inserted_at AS cartridge_inserted_at "
        "FROM console_state AS s CROSS JOIN cartridge.cartridge_slot AS c "
        "WHERE s.id=1 AND c.id=1"
    ).fetchone()
    if row is None:
        raise sqlite3.IntegrityError("Console state or cartridge slot is missing.")
    return row


def require_console_state(db: sqlite3.Connection, previous: sqlite3.Row,
                          **changes: Any) -> None:
    fields = ("power_state", "emulator_pid", "emulator_start_ticks",
              "game_started_at", "session_id", "cartridge_path",
              "cartridge_title", "cartridge_inserted_at")
    expected = {key: previous[key] for key in fields}
    expected.update(changes)
    actual = state_row(db)
    if any(actual[key] != value for key, value in expected.items()):
        raise sqlite3.IntegrityError("Console state transition was not recorded exactly.")


def require_inventory_location(db: sqlite3.Connection, filename: str,
                               location: str, updated_at: str) -> None:
    actual = db.execute(
        "SELECT filename,location,updated_at FROM cartridge_inventory "
        "WHERE filename=? COLLATE NOCASE", (filename,),
    ).fetchone()
    if actual is None or tuple(actual) != (filename, location, updated_at):
        raise sqlite3.IntegrityError("Cartridge location transition was not recorded exactly.")


def reconcile(db: sqlite3.Connection) -> sqlite3.Row:
    row = state_row(db)
    pid = row["emulator_pid"]
    if row["power_state"] == "on" and pid is not None and not process_matches(
        int(pid), row["emulator_start_ticks"]
    ):
        title = row["cartridge_title"] or "unknown game"
        session_id = row["session_id"]
        # The restart reconciliation event is part of the state transition.
        # If its insert fails, retain the previous row for a later retry.
        with db:
            db.execute(
                "UPDATE console_state SET power_state='off', emulator_pid=NULL, "
                "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
            )
            record_event(
                db, "system", "emulator_exited",
                f"The emulator for {title} is no longer running.",
                "Detected by checking the stored process ID and Linux process start time.",
                title, session_id, commit=False,
            )
            require_console_state(db, row, power_state="off", emulator_pid=None,
                                  emulator_start_ticks=None, game_started_at=None, session_id=None)
    return state_row(db)


def read_rom_title(path: Path) -> str:
    try:
        with path.open("rb") as rom:
            data = rom.read(0x200)
        if path.suffix.lower() == ".gba" and len(data) >= 0xAC:
            raw = data[0xA0:0xAC]
        elif path.suffix.lower() in {".gb", ".gbc"} and len(data) >= 0x144:
            raw = data[0x134:0x144]
        else:
            raw = b""
        title = raw.split(b"\0", 1)[0].decode("ascii", "ignore").strip()
        title = "".join(ch for ch in title if ch.isprintable()).strip()
        return title or path.stem
    except OSError:
        return path.stem


def safe_cartridge(name: str) -> Path:
    # Cartridge insertion accepts a loose filename, never an arbitrary path.
    if not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError("Specify a cartridge filename from the Cartridges folder, not a path.")
    # A linked entry is not an independently stored loose cartridge. Reject
    # it before resolution so a symlink cannot fabricate physical inventory.
    if (CARTRIDGES / name).is_symlink():
        raise ValueError("Cartridge files must be regular files, not symbolic links.")
    path = (CARTRIDGES / name).resolve()
    if path.parent != CARTRIDGES.resolve():
        raise ValueError("Cartridge must be inside the console's Cartridges folder.")
    if path.suffix.lower() not in ROM_EXTENSIONS:
        raise ValueError("Supported cartridge formats are .gba, .gb, and .gbc.")
    if not path.is_file():
        raise ValueError(f"No cartridge file named '{name}' exists in Cartridges.")
    if path.stat().st_size < 0x150:
        raise ValueError("The cartridge file is too small to be a valid GB/GBC/GBA ROM.")
    return path


def stored_cartridge_path(value: str) -> Path:
    # Apply the same loose-file contract at verification and actual startup.
    # Resolving the slot first would silently follow a replaced in-folder link.
    raw = Path(value)
    if raw.parent.resolve() != CARTRIDGES.resolve():
        raise ValueError("The inserted cartridge path is outside Cartridges.")
    return safe_cartridge(raw.name)


def inventory_file_available(filename: str) -> bool:
    # Legacy inventory may contain old non-filename identities. Keep the
    # historical row, but never report an unrelated external file as available.
    if not filename or Path(filename).name != filename or filename in {".", ".."}:
        return False
    path = CARTRIDGES / filename
    return (not path.is_symlink() and path.is_file()
            and path.resolve().parent == CARTRIDGES.resolve())


def sync_cartridge_inventory(db: sqlite3.Connection) -> None:
    """Discover loose cartridge files without moving previously stored games."""
    ensure_dirs()
    now = utc_now()
    for path in sorted(CARTRIDGES.iterdir(), key=lambda p: p.name.casefold()):
        # Do not follow linked files during inventory discovery. Otherwise a
        # linked external ROM can appear in R2's physical possessions even
        # though the insertion path cannot legitimately load it.
        if (path.is_symlink() or not path.is_file()
                or path.suffix.lower() not in ROM_EXTENSIONS):
            continue
        title = read_rom_title(path)
        db.execute(
            "INSERT INTO cartridge_inventory(filename,title,location,discovered_at,updated_at) "
            "VALUES(?,?,'pockets',?,?) ON CONFLICT(filename) DO UPDATE SET "
            "title=excluded.title,updated_at=excluded.updated_at "
            "WHERE cartridge_inventory.title IS NOT excluded.title",
            (path.name, title, now, now),
        )
    slot = db.execute("SELECT rom_path,rom_title FROM cartridge.cartridge_slot WHERE id=1").fetchone()
    if slot and slot["rom_path"]:
        filename = Path(slot["rom_path"]).name
        title = slot["rom_title"] or Path(filename).stem
        db.execute(
            "INSERT INTO cartridge_inventory(filename,title,location,discovered_at,updated_at) "
            "VALUES(?,?,'slot',?,?) ON CONFLICT(filename) DO UPDATE SET "
            "title=excluded.title,location='slot',updated_at=excluded.updated_at "
            "WHERE cartridge_inventory.title IS NOT excluded.title "
            "OR cartridge_inventory.location != 'slot'",
            (filename, title, now, now),
        )
    db.commit()


def move_cartridge(db: sqlite3.Connection, filename: str, location: str) -> dict[str, Any]:
    """Move one loose cartridge between R2's physical inventory locations."""
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False, "message": "Power off the console before moving cartridges.", **status_data(db)}
    destination = location.strip().lower()
    if destination not in {"pockets", "shelf", "box", "room"}:
        return {"ok": False, "message": "Destination must be pockets, shelf, box, or room.", **status_data(db)}
    if not filename or Path(filename).name != filename or filename in {".", ".."}:
        return {"ok": False, "message": "Use a cartridge filename, not a path.", **status_data(db)}
    sync_cartridge_inventory(db)
    current = db.execute("SELECT filename,location,title FROM cartridge_inventory WHERE filename=? COLLATE NOCASE", (filename,)).fetchone()
    if current is None:
        return {"ok": False, "message": f"No known cartridge named '{filename}'.", **status_data(db)}
    filename = current["filename"]
    if not filename or Path(filename).name != filename or filename in {".", ".."}:
        return {"ok": False, "message": "Historical inventory identity is not a loose cartridge filename.", **status_data(db)}
    if (CARTRIDGES / filename).is_symlink():
        return {"ok": False, "message": "A linked ROM is not a physical loose cartridge; its historical inventory record remains preserved.", **status_data(db)}
    slot = db.execute("SELECT rom_path FROM cartridge.cartridge_slot WHERE id=1").fetchone()
    if slot and slot["rom_path"] and Path(slot["rom_path"]).name.casefold() == filename.casefold():
        return {"ok": False, "message": "That cartridge is inserted. Eject it before moving it.", **status_data(db)}
    updated_at = utc_now()
    with db:
        db.execute("UPDATE cartridge_inventory SET location=?,updated_at=? WHERE filename=? COLLATE NOCASE", (destination, updated_at, filename))
        record_event(db, "physical", "cartridge_moved", f"R2 put the {current['title']} cartridge in {destination}.", f"Filename={filename}; destination={destination}.", current["title"], commit=False)
        require_inventory_location(db, filename, destination, updated_at)
        require_console_state(db, row)
    return {"ok": True, "message": f"Moved {current['title']} cartridge to {destination}.", **status_data(db)}


def status_data(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    sync_cartridge_inventory(db)
    return {
        "device": "Game Boy Advance",
        "power_state": row["power_state"],
        "cartridge_inserted": bool(row["cartridge_path"]),
        "cartridge_path": row["cartridge_path"],
        "cartridge_title": row["cartridge_title"],
        "emulator_pid": row["emulator_pid"],
        "game_started_at": row["game_started_at"],
        "session_id": row["session_id"],
        "game_running": bool(row["power_state"] == "on" and row["emulator_pid"]),
        "database": str(DB_PATH),
        "state_database": str(STATE_DB_PATH),
        "cartridges_directory": str(CARTRIDGES),
        "saves_directory": str(SAVES),
    }


def mgba_environment() -> dict[str, str]:
    env = os.environ.copy()
    # Isolate mGBA settings from the desktop user's personal emulator config.
    env["XDG_CONFIG_HOME"] = str(STATE / "mgba-config")
    config_dir = STATE / "mgba-config" / "mgba"
    config_dir.mkdir(parents=True, exist_ok=True)
    # Save paths and rewind/autosave options are passed explicitly with -C when
    # launching mGBA. Do not rewrite a config.ini with uncertain section state.
    # mGBA's Qt frontend reads keyboard shortcuts from qt.ini. Empty values
    # override its default save/load-state and rewind hotkeys for this console.
    qt_config = config_dir / "qt.ini"
    disabled_actions = [
        "loadState", "saveState", "loadStateFile", "saveStateFile",
        "quickLoad", "quickSave", "undoLoadState", "undoSaveState",
        "holdRewind", "rewind", "frameRewind",
    ]
    disabled_actions += [f"quickLoad.{slot}" for slot in range(1, 10)]
    disabled_actions += [f"quickSave.{slot}" for slot in range(1, 10)]
    qt_config.write_text(
        "[shortcutKey]\n" + "".join(f"{action}=\n" for action in disabled_actions),
        encoding="utf-8",
    )
    return env



def verify_console(db: sqlite3.Connection) -> dict[str, Any]:
    """Verify the installed device, current cartridge slot, and emulator path.

    This does not change power state or replace the cartridge. Power-on
    performs the actual launch using the single ROM stored in the slot.
    """
    row = reconcile(db)
    emulator = Path(EMULATOR).expanduser()
    if not emulator.is_absolute():
        emulator = emulator.resolve()
    checks = {
        "device_root_exists": ROOT.is_dir(),
        "cartridges_directory_exists": CARTRIDGES.is_dir(),
        "saves_directory_writable": SAVES.is_dir() and os.access(SAVES, os.W_OK),
        "state_directory_writable": STATE.is_dir() and os.access(STATE, os.W_OK),
        "emulator_exists": emulator.is_file(),
        "emulator_executable": emulator.is_file() and os.access(emulator, os.X_OK),
    }
    rom_path = row["cartridge_path"]
    slot_error = None
    if rom_path:
        rom = Path(rom_path)
        checks["slot_rom_exists"] = (rom.parent.resolve() == CARTRIDGES.resolve()
                                     and inventory_file_available(rom.name))
        checks["slot_rom_format_supported"] = rom.suffix.lower() in ROM_EXTENSIONS
        try:
            stored_cartridge_path(rom_path)
        except (ValueError, OSError) as exc:
            slot_error = str(exc)
    else:
        checks["slot_rom_exists"] = False
        checks["slot_rom_format_supported"] = False
    failures = []
    for key, description in (
        ("device_root_exists", "device root is missing"),
        ("cartridges_directory_exists", "Cartridges directory is missing"),
        ("saves_directory_writable", "Saves directory is not writable"),
        ("state_directory_writable", "State directory is not writable"),
        ("emulator_executable", f"mGBA is missing or not executable at {emulator}"),
    ):
        if not checks[key]:
            failures.append(description)
    if rom_path and slot_error:
        failures.append(slot_error.rstrip("."))
    ok = not failures
    if failures:
        message = "Console verification failed: " + "; ".join(failures) + "."
    elif not rom_path:
        message = "Console verified; cartridge slot is empty, so power on will not launch a game."
    else:
        message = f"Console verified. Power on will launch {row['cartridge_title'] or Path(rom_path).stem} from the current cartridge slot."
    return {
        "ok": ok, "message": message, "checks": checks,
        "emulator_path": str(emulator), **status_data(db),
    }

def power_on(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] == "on":
        return {"ok": False, "message": "The console is already powered on.", **status_data(db)}
    cartridge = row["cartridge_path"]
    if not cartridge:
        with db:
            db.execute(
                "UPDATE console_state SET power_state='on', emulator_pid=NULL, "
                "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
            )
            record_event(db, "physical", "console_powered_on_empty",
                         "R2 powered on the Game Boy Advance with no cartridge inserted.",
                         commit=False)
            require_console_state(db, row, power_state="on", emulator_pid=None,
                                  emulator_start_ticks=None, game_started_at=None, session_id=None)
        return {"ok": True, "message": "Console powered on. The cartridge slot is empty; no emulator was launched.",
                **status_data(db)}
    proc = None
    try:
        rom = stored_cartridge_path(cartridge)
        emulator = Path(EMULATOR).expanduser().resolve()
        if not emulator.is_file() or not os.access(emulator, os.X_OK):
            raise ValueError(
                f"mGBA executable not found or not executable at {emulator}. "
                "Set R2_MGBA_EXECUTABLE to its actual executable path."
            )
        title = row["cartridge_title"] or read_rom_title(rom)
        session_id = f"gba-{int(time.time())}-{os.getpid()}"
        # Do not pass --savestate/-t, and do not expose a save-state command.
        # Use per-console config and only the one ROM recorded in the slot.
        proc = subprocess.Popen(
            [str(emulator), "-C", f"savegamePath={SAVES}",
             "-C", f"savestatePath={STATE / 'disabled-save-states'}",
             "-C", "autosave=false", "-C", "rewindEnable=false", str(rom)],
            cwd=str(ROOT), env=mgba_environment(),
            stdin=subprocess.DEVNULL, start_new_session=True,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(0.15)
        if proc.poll() is not None:
            return {"ok": False, "message": f"mGBA exited immediately with status {proc.returncode}.",
                    **status_data(db)}
        ticks = process_start_ticks(proc.pid)
        if ticks is None:
            raise ValueError("mGBA started but its process identity could not be verified.")
        started_at = utc_now()
        with db:
            updated = db.execute(
                "UPDATE console_state SET power_state='on', emulator_pid=?, emulator_start_ticks=?, "
                "game_started_at=?, session_id=? WHERE id=1 AND power_state='off' "
                "AND EXISTS (SELECT 1 FROM cartridge.cartridge_slot WHERE id=1 AND rom_path=?)",
                (proc.pid, ticks, started_at, session_id, cartridge),
            )
            if updated.rowcount != 1:
                raise sqlite3.IntegrityError("Console or cartridge changed while mGBA was starting.")
            record_event(db, "physical", "console_powered_on",
                         f"R2 powered on the Game Boy Advance with {title} inserted.",
                         "The console launched mGBA with only the ROM recorded in the cartridge slot.",
                         title, session_id, commit=False)
            record_event(db, "virtual", "game_session_started",
                         f"An emulated game session began: {title}.",
                         "This is an in-game context; it is not a claim that game events are physical-world events.",
                         title, session_id, commit=False)
            require_console_state(db, row, power_state="on", emulator_pid=proc.pid,
                                  emulator_start_ticks=ticks, game_started_at=started_at,
                                  session_id=session_id)
    except (OSError, ValueError, sqlite3.Error, ProcessIdentityError) as exc:
        message = str(exc)
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=2)
            except (OSError, subprocess.TimeoutExpired) as cleanup_error:
                message = f"{message} Could not confirm mGBA cleanup: {cleanup_error}"
        return {"ok": False, "message": message, **status_data(db)}
    state = status_data(db)
    if not state["game_running"] or state["session_id"] != session_id:
        return {"ok": False, "message": "mGBA stopped before startup could be confirmed.", **state}
    return {"ok": True, "message": f"Console powered on; started {title}.", **state}


def power_off(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] == "off":
        return {"ok": True, "message": "The console is already powered off.", **status_data(db)}
    pid = row["emulator_pid"]
    title = row["cartridge_title"] or "unknown game"
    session_id = row["session_id"]
    if pid:
        fd = open_owned_emulator_pidfd(int(pid), row["emulator_start_ticks"])
        if fd is not None:
            try:
                if not stop_owned_emulator(fd, int(pid)):
                    return {"ok": False, "message": "Emulator exit was not confirmed.",
                            **status_data(db)}
            except OSError as exc:
                return {"ok": False, "message": f"Could not stop the emulator: {exc}",
                        **status_data(db)}
            finally:
                os.close(fd)
    # Never publish an off state without its physical and virtual history.
    with db:
        db.execute(
            "UPDATE console_state SET power_state='off', emulator_pid=NULL, "
            "emulator_start_ticks=NULL, game_started_at=NULL, session_id=NULL WHERE id=1"
        )
        record_event(
            db, "physical", "console_powered_off",
            "R2 powered off the Game Boy Advance.",
            "Power-off does not create a save state or invoke an in-game save. Unsaved progress may be lost.",
            title, session_id, commit=False,
        )
        if session_id:
            record_event(db, "virtual", "game_session_interrupted",
                         f"The emulated session for {title} ended when the console was powered off.",
                         "The console did not issue an in-game save command.",
                         title, session_id, commit=False)
        require_console_state(db, row, power_state="off", emulator_pid=None,
                              emulator_start_ticks=None, game_started_at=None, session_id=None)
    return {"ok": True, "message": "Console powered off. No in-game save command or save state was issued.",
            **status_data(db)}


def insert_cartridge(db: sqlite3.Connection, name: str) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False,
                "message": "The Game Boy Advance is powered on. Turn it off before removing or replacing the cartridge.",
                **status_data(db)}
    if row["cartridge_path"]:
        return {"ok": False, "message": "A cartridge is already inserted. Eject it before inserting another game.", **status_data(db)}
    try:
        path = safe_cartridge(name)
    except (ValueError, OSError) as exc:
        return {"ok": False, "message": str(exc), **status_data(db)}
    sync_cartridge_inventory(db)
    inventory = db.execute("SELECT filename,location FROM cartridge_inventory WHERE filename=? COLLATE NOCASE", (path.name,)).fetchone()
    if inventory is None or inventory["filename"] != path.name:
        return {"ok": False, "message": "Cartridge is not registered under its exact filename in R2's inventory.", **status_data(db)}
    if inventory["location"] != "pockets":
        return {"ok": False, "message": f"The {read_rom_title(path)} cartridge is in {inventory['location']}, not R2's pockets. Retrieve it first.", **status_data(db)}
    title = read_rom_title(path)
    inserted_at = utc_now()
    with db:
        db.execute(
            "UPDATE cartridge.cartridge_slot SET rom_path=?, rom_title=?, inserted_at=? WHERE id=1",
            (str(path), title, inserted_at),
        )
        db.execute("UPDATE cartridge_inventory SET location='slot',updated_at=? WHERE filename=? COLLATE NOCASE", (inserted_at, path.name))
        record_event(db, "physical", "cartridge_inserted",
                     f"R2 inserted the {title} cartridge.",
                     f"ROM filename: {path.name}", title, commit=False)
        require_inventory_location(db, path.name, "slot", inserted_at)
        require_console_state(db, row, cartridge_path=str(path), cartridge_title=title,
                              cartridge_inserted_at=inserted_at)
    return {"ok": True, "message": f"Inserted cartridge: {title}.",
            **status_data(db)}


def eject_cartridge(db: sqlite3.Connection) -> dict[str, Any]:
    row = reconcile(db)
    if row["power_state"] != "off":
        return {"ok": False,
                "message": "The Game Boy Advance is powered on. Turn it off before removing or replacing the cartridge.",
                **status_data(db)}
    title = row["cartridge_title"]
    if not row["cartridge_path"]:
        return {"ok": False, "message": "The cartridge slot is already empty.", **status_data(db)}
    filename = Path(row["cartridge_path"]).name
    sync_cartridge_inventory(db)
    updated_at = utc_now()
    with db:
        db.execute("UPDATE cartridge.cartridge_slot SET rom_path=NULL, rom_title=NULL, inserted_at=NULL WHERE id=1")
        db.execute("UPDATE cartridge_inventory SET location='pockets',updated_at=? WHERE filename=? COLLATE NOCASE", (updated_at, filename))
        record_event(db, "physical", "cartridge_ejected", f"R2 removed the {title} cartridge and is carrying it in his pockets.", f"Filename={filename}; destination=pockets.", title, commit=False)
        require_inventory_location(db, filename, "pockets", updated_at)
        require_console_state(db, row, cartridge_path=None, cartridge_title=None,
                              cartridge_inserted_at=None)
    return {"ok": True, "message": f"Ejected {title}. The cartridge slot is now empty.",
            **status_data(db)}


def list_cartridges(db: sqlite3.Connection) -> list[dict[str, Any]]:
    sync_cartridge_inventory(db)
    rows = db.execute("SELECT filename,title,location FROM cartridge_inventory ORDER BY filename COLLATE NOCASE").fetchall()
    return [{"filename": row["filename"], "title": row["title"], "location": row["location"], "file_available": inventory_file_available(row["filename"])} for row in rows]


def press_button(db: sqlite3.Connection, button: str, duration_ms: int) -> dict[str, Any]:
    row = reconcile(db)
    name = button.upper()
    if name not in BUTTON_KEYS:
        return {"ok": False, "message": "Unknown GBA button. Use A, B, L, R, START, SELECT, UP, DOWN, LEFT, or RIGHT.",
                **status_data(db)}
    if row["power_state"] != "on" or not row["emulator_pid"]:
        return {"ok": False, "message": "No game is running. Power on the console with a cartridge inserted first.",
                **status_data(db)}
    if duration_ms < 1 or duration_ms > MAX_PRESS_MS:
        return {"ok": False, "message": f"Button duration must be 1–{MAX_PRESS_MS} milliseconds.",
                **status_data(db)}
    if shutil.which("xdotool") is None:
        return {"ok": False, "message": "xdotool is not installed; controller input was not sent. Install xdotool to enable R2 button presses.",
                **status_data(db)}
    pid = str(row["emulator_pid"])
    fd = open_owned_emulator_pidfd(int(pid), row["emulator_start_ticks"])
    if fd is None:
        return {"ok": False, "message": "Emulator session ended before controller input.",
                **status_data(db)}
    key = BUTTON_KEYS[name]

    def require_window_owner(window: str) -> None:
        if wait_for_emulator_exit(fd, 0):
            raise ProcessIdentityError("Emulator session ended during controller input.")
        owner = subprocess.run(
            ["xdotool", "getwindowpid", window],
            capture_output=True, text=True, timeout=2, check=False,
        )
        if (owner.returncode != 0 or owner.stdout.strip() != pid
                or wait_for_emulator_exit(fd, 0)):
            raise ProcessIdentityError("Controller window no longer belongs to the active emulator.")
    pressed_window: str | None = None
    input_error: str | None = None
    try:
        found = subprocess.run(
            ["xdotool", "search", "--onlyvisible", "--pid", pid],
            capture_output=True, text=True, timeout=2, check=False,
        )
        windows = [line.strip() for line in found.stdout.splitlines()
                   if line.strip().isascii() and line.strip().isdecimal()]
        if found.returncode != 0 or not windows:
            return {"ok": False, "message": "Could not find the running mGBA window; no button press was sent.",
                    **status_data(db)}
        window = windows[0]
        require_window_owner(window)
        subprocess.run(["xdotool", "windowactivate", "--sync", window],
                       check=True, timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        require_window_owner(window)
        # Record the target immediately before keydown, which may deliver the
        # key before reporting an error. Do not release a key merely because
        # window activation was attempted.
        pressed_window = window
        subprocess.run(["xdotool", "keydown", "--window", window, key],
                       check=True, timeout=3, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        if wait_for_emulator_exit(fd, duration_ms):
            raise ProcessIdentityError("Emulator session ended while holding the controller button.")
    except (OSError, subprocess.SubprocessError, ProcessIdentityError) as exc:
        input_error = f"Button input failed: {exc}"
    finally:
        # A failed keydown may still have reached the emulator. Release
        # only while the same process and window still own that input.
        try:
            if pressed_window is not None:
                require_window_owner(pressed_window)
                released = subprocess.run(
                    ["xdotool", "keyup", "--window", pressed_window, key],
                    check=False, timeout=3,
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                )
                if released.returncode != 0 and input_error is None:
                    input_error = "Button release failed; xdotool did not confirm keyup."
                if wait_for_emulator_exit(fd, 0) and input_error is None:
                    input_error = "Emulator session ended before input completion was confirmed."
        except (OSError, subprocess.SubprocessError, ProcessIdentityError) as exc:
            if input_error is None:
                input_error = f"Button release failed: {exc}"
        finally:
            os.close(fd)
    if input_error is not None:
        return {"ok": False, "message": input_error, **status_data(db)}
    title = row["cartridge_title"] or "unknown game"
    record_event(db, "physical", "controller_input",
                 f"R2 pressed {name} while playing {title}.",
                 f"Emulator key={BUTTON_KEYS[name]}; duration_ms={duration_ms}.",
                 title, row["session_id"])
    return {"ok": True, "message": f"Sent {name} button press for {duration_ms} ms.",
            **status_data(db)}


def print_result(value: Any, as_json: bool) -> None:
    if as_json:
        print(json.dumps(value, ensure_ascii=False))
        return
    if isinstance(value, dict):
        print(value.get("message", ""))
        if "power_state" in value:
            print(f"Power: {value['power_state']}")
            print(f"Cartridge: {value.get('cartridge_title') or '(empty)'}")
            print(f"Game running: {'yes' if value.get('game_running') else 'no'}")
            print(f"Database: {value.get('database')}")
            if "checks" in value:
                for check, passed in value["checks"].items():
                    print(f"Check {check}: {'OK' if passed else 'FAILED'}")
                print(f"mGBA: {value.get('emulator_path')}")
    elif isinstance(value, list):
        if not value:
            print("No cartridges or pending verified game events.")
        for item in value:
            if "filename" in item:
                print(f"{item['filename']} — {item['title']} [{item.get('location', 'unknown location')}]")
            else:
                print(f"#{item['event_id']} [{item['context']}] {item['summary']} "
                      f"({item['evidence_source']})")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="R2's persistent virtual Game Boy Advance console")
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    sub = parser.add_subparsers(dest="command")
    sub.add_parser("status", help="inspect console and cartridge state")
    sub.add_parser("verify", help="verify install, emulator, and currently inserted ROM before power-on")
    sub.add_parser("list", help="list known cartridges and their physical locations")
    move = sub.add_parser("move", help="move a loose cartridge to pockets, shelf, box, or room")
    move.add_argument("filename")
    move.add_argument("location", choices=("pockets", "shelf", "box", "room"))
    ins = sub.add_parser("insert", help="insert a cartridge while powered off")
    ins.add_argument("filename")
    sub.add_parser("eject", help="eject the cartridge while powered off")
    power = sub.add_parser("power", help="power on or off")
    power.add_argument("state", choices=("on", "off"))
    press = sub.add_parser("press", help="send one GBA controller button press")
    press.add_argument("button")
    press.add_argument("duration_ms", nargs="?", type=int, default=DEFAULT_PRESS_MS)
    sub.add_parser("events", help="list verified virtual events waiting for Life Log sync")
    ack = sub.add_parser("ack_event", help="acknowledge a verified event after Life Log sync")
    ack.add_argument("event_id", type=int)
    args = parser.parse_args(argv)
    command = args.command or "power"
    state = args.state if args.command == "power" else "on"
    try:
        with console_lock(), closing(connect()) as db:
            if command == "status":
                result = status_data(db)
            elif command == "verify":
                result = verify_console(db)
            elif command == "list":
                result = list_cartridges(db)
            elif command == "move":
                result = move_cartridge(db, args.filename, args.location)
            elif command == "insert":
                result = insert_cartridge(db, args.filename)
            elif command == "eject":
                result = eject_cartridge(db)
            elif command == "press":
                result = press_button(db, args.button, args.duration_ms)
            elif command == "events":
                result = pending_verified_events(db)
            elif command == "ack_event":
                acknowledged = acknowledge_verified_event(db, args.event_id)
                result = {
                    "ok": acknowledged,
                    "message": ("Verified event acknowledged." if acknowledged else
                                "No matching pending verified event was acknowledged."),
                }
            elif command == "power":
                result = power_on(db) if state == "on" else power_off(db)
            else:
                result = {"ok": False, "message": f"Unsupported command: {command}"}
    except (TimeoutError, ProcessIdentityError) as exc:
        result = {"ok": False, "message": str(exc)}
    print_result(result, args.json)
    return 0 if not isinstance(result, dict) or result.get("ok", True) else 1


if __name__ == "__main__":
    raise SystemExit(main())
