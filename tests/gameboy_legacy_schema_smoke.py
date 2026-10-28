#!/usr/bin/env python3
"""Exercise additive migrations for legacy Game Boy SQLite schemas."""
from __future__ import annotations

import importlib.util
import os
import sqlite3
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[1]
CONSOLE = REPO / "GameBoyAdvance.py"


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="r2-gameboy-legacy-") as temp:
        root = Path(temp) / "device"
        state_dir = root / "State"
        state_dir.mkdir(parents=True)
        (root / "Cartridges").mkdir()

        os.environ["R2_GAMEBOY_ROOT"] = str(root)
        os.environ["R2_GAMEBOY_DB"] = str(root / "gameboy.db")

        # Old cartridge DB: the slot table predates its metadata columns.
        cartridge_db = sqlite3.connect(root / "gameboy.db")
        cartridge_db.executescript("""
            CREATE TABLE cartridge_slot (
                id INTEGER PRIMARY KEY CHECK (id = 1)
            );
            INSERT INTO cartridge_slot(id) VALUES (1);

            -- Legacy releases stored these tables alongside cartridge_slot.
            CREATE TABLE console_state (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                power_state TEXT NOT NULL DEFAULT 'off'
            );
            INSERT INTO console_state(id, power_state) VALUES (1, 'on');

            CREATE TABLE console_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                context TEXT NOT NULL,
                event_type TEXT NOT NULL,
                summary TEXT NOT NULL,
                details TEXT,
                game_title TEXT,
                session_id TEXT
            );
            INSERT INTO console_events(id, timestamp, context, event_type, summary, details)
            VALUES (1, '2023-01-01T00:00:00+00:00', 'system', 'legacy_combined_boot',
                    'legacy combined event retained', 'source database history');
            INSERT INTO console_events(id, timestamp, context, event_type, summary, details)
            VALUES (7, '2023-01-02T00:00:00+00:00', 'system', 'legacy_combined_event_7',
                    'legacy event with non-colliding ID', 'source database history');

            CREATE TABLE cartridge_inventory (filename TEXT PRIMARY KEY COLLATE NOCASE);
            INSERT INTO cartridge_inventory(filename) VALUES ('Legacy.gba');
        """)
        cartridge_db.commit()
        cartridge_db.close()

        # Old state DB: preserve existing console state, event history, and
        # cartridge inventory while adding fields required by current code.
        state_path = state_dir / "console_state.db"
        legacy = sqlite3.connect(state_path)
        legacy.executescript("""
            CREATE TABLE console_state (
                id INTEGER PRIMARY KEY CHECK (id = 1),
                power_state TEXT NOT NULL DEFAULT 'off'
            );
            INSERT INTO console_state(id, power_state) VALUES (1, 'off');

            CREATE TABLE console_events (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                context TEXT NOT NULL,
                event_type TEXT NOT NULL,
                summary TEXT NOT NULL,
                details TEXT,
                game_title TEXT,
                session_id TEXT
            );
            INSERT INTO console_events(timestamp, context, event_type, summary, details)
            VALUES ('2024-01-02T03:04:05+00:00', 'system', 'legacy_boot',
                    'legacy event retained', 'older schema fixture');

            CREATE TABLE cartridge_inventory (
                filename TEXT PRIMARY KEY COLLATE NOCASE
            );
            INSERT INTO cartridge_inventory(filename) VALUES ('Target.gba');
        """)
        legacy.commit()
        legacy.close()

        spec = importlib.util.spec_from_file_location("r2_gameboy_legacy", CONSOLE)
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        db = module.connect()
        try:
            state = db.execute(
                "SELECT id, power_state, emulator_pid, session_id FROM console_state WHERE id=1"
            ).fetchone()
            assert state is not None and state["power_state"] == "off"
            assert state["emulator_pid"] is None and state["session_id"] is None

            old_event = db.execute(
                "SELECT timestamp, summary, verified, life_log_synced "
                "FROM console_events WHERE event_type='legacy_boot'"
            ).fetchone()
            assert old_event is not None
            assert old_event["summary"] == "legacy event retained"
            assert old_event["verified"] == 0 and old_event["life_log_synced"] == 0

            combined_event = db.execute(
                "SELECT summary, verified, life_log_synced FROM console_events "
                "WHERE event_type='legacy_combined_boot'"
            ).fetchone()
            assert combined_event is not None
            assert combined_event["summary"] == "legacy combined event retained"
            assert combined_event["verified"] == 0 and combined_event["life_log_synced"] == 0
            assert db.execute(
                "SELECT COUNT(*) FROM console_events WHERE event_type='legacy_combined_event_7'"
            ).fetchone()[0] == 1


            old_cartridge = db.execute(
                "SELECT filename, title, location, discovered_at, updated_at "
                "FROM cartridge_inventory WHERE filename='Legacy.gba'"
            ).fetchone()
            assert old_cartridge is not None
            assert old_cartridge["filename"] == "Legacy.gba"
            assert old_cartridge["title"] == ""
            assert old_cartridge["location"] == "pockets"
            assert db.execute(
                "SELECT COUNT(*) FROM cartridge_inventory WHERE filename='Target.gba'"
            ).fetchone()[0] == 1

            # Exercise real post-migration writes, not just column presence.
            module.record_event(db, "system", "post_migration_write",
                                "new event after legacy migration",
                                "legacy schema compatibility test")
            db.execute(
                "UPDATE cartridge.cartridge_slot SET rom_path=?, rom_title=?, inserted_at=? WHERE id=1",
                (str(root / "Cartridges" / "Legacy.gba"), "Legacy Cartridge",
                 "2026-10-27T23:00:00+00:00"),
            )
            db.execute(
                "INSERT INTO cartridge_inventory(filename,title,location,discovered_at,updated_at) "
                "VALUES(?,?,?,?,?)",
                ("New.gba", "New Cartridge", "pockets", "2026-10-27", "2026-10-27"),
            )
            db.commit()

            assert db.execute(
                "SELECT COUNT(*) FROM console_events WHERE event_type='post_migration_write'"
            ).fetchone()[0] == 1
            slot = db.execute(
                "SELECT rom_path, rom_title FROM cartridge.cartridge_slot WHERE id=1"
            ).fetchone()
            assert slot is not None and slot["rom_title"] == "Legacy Cartridge"
            assert db.execute(
                "SELECT COUNT(*) FROM cartridge_inventory WHERE filename IN ('Legacy.gba','Target.gba','New.gba')"
            ).fetchone()[0] == 3
            assert db.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            assert db.execute("PRAGMA cartridge.integrity_check").fetchone()[0] == "ok"

            # Migration is copy-only: the original combined DB remains readable.
            source = sqlite3.connect(root / "gameboy.db")
            try:
                assert source.execute(
                    "SELECT COUNT(*) FROM console_events WHERE event_type='legacy_combined_boot'"
                ).fetchone()[0] == 1
                assert source.execute(
                    "SELECT COUNT(*) FROM cartridge_inventory WHERE filename='Legacy.gba'"
                ).fetchone()[0] == 1
            finally:
                source.close()
        finally:
            db.close()

    # Fresh installations must not receive a migration marker before an old
    # combined database is ever present at the configured cartridge path.
    with tempfile.TemporaryDirectory(prefix="r2-gameboy-fresh-") as fresh:
        fresh_root = Path(fresh) / "device"
        os.environ["R2_GAMEBOY_ROOT"] = str(fresh_root)
        os.environ["R2_GAMEBOY_DB"] = str(fresh_root / "gameboy.db")
        fresh_spec = importlib.util.spec_from_file_location(
            "r2_gameboy_fresh", CONSOLE
        )
        assert fresh_spec and fresh_spec.loader
        fresh_module = importlib.util.module_from_spec(fresh_spec)
        fresh_spec.loader.exec_module(fresh_module)
        fresh_db = fresh_module.connect()
        try:
            assert fresh_db.execute(
                "SELECT COUNT(*) FROM console_migrations"
            ).fetchone()[0] == 0
        finally:
            fresh_db.close()

    print("Legacy Game Boy cartridge/state schemas preserve existing data and accept new writes.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
