#!/usr/bin/env python3
"""Read-only preflight audit for R2's SQLite databases.

Point --root at the R2 data directory containing r2_reality.db, r2_fridge.db,
r2_memory.db, r2_rewards.db, and r2_addictions.db. The tool never initializes R2, creates tables, or migrates a
source database. --snapshot-dir makes independent SQLite-backup snapshots
without overwriting existing files, then audits those snapshots. For a
cross-database-consistent snapshot, stop R2 before running this tool.
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
import sys
from pathlib import Path
from typing import Any

DATABASES = (
    "r2_reality.db", "r2_fridge.db", "r2_memory.db",
    "r2_rewards.db", "r2_addictions.db",
)
# Device databases are optional because some installations do not deploy those
# peripherals. If present, they are audited and snapshotted too.
OPTIONAL_DATABASES = (
    "Devices/GameBoyAdvance/gameboy.db",
    "Devices/GameBoyAdvance/State/console_state.db",
)
ALL_DATABASES = DATABASES + OPTIONAL_DATABASES
LEGACY_PHANTOM_DESCRIPTION = (
    "Guaranteed filling burger generated because the fridge was empty"
)


def _readonly_connection(path: Path) -> sqlite3.Connection:
    uri = path.resolve().as_uri() + "?mode=ro"
    db = sqlite3.connect(uri, uri=True, timeout=2.0)
    db.row_factory = sqlite3.Row
    db.execute("PRAGMA query_only=ON")
    return db


def snapshot_database(source: Path, destination: Path) -> None:
    """Create a private, consistent SQLite backup without changing the source."""
    source_db: sqlite3.Connection | None = None
    target_db: sqlite3.Connection | None = None
    created = False
    try:
        source_db = _readonly_connection(source)
        # O_EXCL prevents races and mode 0600 keeps copied memories private.
        fd = os.open(destination, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        os.close(fd)
        created = True
        target_db = sqlite3.connect(destination)
        source_db.backup(target_db)
        target_db.commit()
    except Exception:
        if target_db is not None:
            target_db.close()
            target_db = None
        if created:
            try:
                destination.unlink()
            except OSError:
                pass
        raise
    finally:
        if source_db is not None:
            source_db.close()
        if target_db is not None:
            target_db.close()


def _path_has_symlink_component(root: Path, relative: str) -> bool:
    current = root
    for part in Path(relative).parts:
        current = current / part
        if current.is_symlink():
            return True
    return False


def create_snapshot_set(root: Path, snapshot_dir: Path) -> list[Path]:
    """Create snapshots for required and present optional DBs, never overwriting."""
    snapshot_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    targets = [snapshot_dir / name for name in ALL_DATABASES]
    conflicts = [path for path in targets if path.exists() or path.is_symlink()]
    if conflicts:
        joined = ", ".join(str(path.relative_to(snapshot_dir)) for path in conflicts)
        raise FileExistsError(f"refusing to mix with existing snapshot file(s): {joined}")

    # Refuse a database or parent-directory symlink before reading/copying it.
    linked_sources = [name for name in ALL_DATABASES
                      if _path_has_symlink_component(root, name)]
    if linked_sources:
        joined = ", ".join(linked_sources)
        raise OSError(f"refusing symlink database path(s): {joined}")

    created: list[Path] = []
    try:
        for name in ALL_DATABASES:
            source = root / name
            if not source.is_file():
                continue
            destination = snapshot_dir / name
            destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
            snapshot_database(source, destination)
            created.append(destination)
    except Exception:
        for path in created:
            try:
                path.unlink()
            except OSError:
                pass
        raise
    return created


def audit_database(path: Path) -> dict[str, Any]:
    report: dict[str, Any] = {
        "path": str(path),
        "exists": path.is_file() or path.is_symlink(),
        "integrity": None,
        "tables": {},
        "legacy_phantom_food_records": [],
        "foreign_key_violations": [],
        "error": None,
    }
    if path.is_symlink():
        report["error"] = "database path is a symlink; refusing to follow it"
        return report
    if not report["exists"]:
        report["error"] = "database file not found"
        return report

    db: sqlite3.Connection | None = None
    try:
        db = _readonly_connection(path)
        report["integrity"] = [
            str(row[0]) for row in db.execute("PRAGMA integrity_check").fetchall()
        ]
        report["foreign_key_violations"] = [
            {"table": str(row[0]), "rowid": row[1],
             "parent": str(row[2]), "fkid": int(row[3])}
            for row in db.execute("PRAGMA foreign_key_check").fetchall()
        ]
        table_rows = db.execute(
            "SELECT name FROM sqlite_master "
            "WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name"
        ).fetchall()

        for table_row in table_rows:
            table = str(table_row[0])
            quoted = '"' + table.replace('"', '""') + '"'
            columns = [
                str(row[1]) for row in db.execute(f"PRAGMA table_info({quoted})")
            ]
            count = int(db.execute(f"SELECT COUNT(*) FROM {quoted}").fetchone()[0])
            report["tables"][table] = {"rows": count, "columns": columns}

            if "description" in columns:
                matches = int(db.execute(
                    f"SELECT COUNT(*) FROM {quoted} WHERE description = ?",
                    (LEGACY_PHANTOM_DESCRIPTION,),
                ).fetchone()[0])
                if matches:
                    report["legacy_phantom_food_records"].append(
                        {"table": table, "rows": matches}
                    )
    except sqlite3.Error as exc:
        report["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if db is not None:
            db.close()
    return report


def _table_columns(db: sqlite3.Connection, table: str) -> set[str]:
    quoted = '"' + table.replace('"', '""') + '"'
    return {str(row[1]) for row in db.execute(f"PRAGMA table_info({quoted})")}


def _relationship_check(
    source_db: sqlite3.Connection | None,
    source_name: str,
    source_table: str,
    source_column: str,
    target_db: sqlite3.Connection | None,
    target_name: str,
    target_table: str,
    target_column: str,
) -> dict[str, Any]:
    name = f"{source_name}:{source_table}.{source_column}->{target_name}:{target_table}.{target_column}"
    result: dict[str, Any] = {
        "name": name, "status": "not_checked",
        "source_database": source_name, "source_table": source_table,
        "source_column": source_column, "target_database": target_name,
        "target_table": target_table, "target_column": target_column,
        "checked_rows": 0, "orphan_count": 0, "orphan_values": [],
    }
    if source_db is None or target_db is None:
        result["reason"] = "source or target database file is unavailable"
        return result
    try:
        source_tables = {str(row[0]) for row in source_db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )}
        target_tables = {str(row[0]) for row in target_db.execute(
            "SELECT name FROM sqlite_master WHERE type='table'"
        )}
        if source_table not in source_tables or target_table not in target_tables:
            result["reason"] = "legacy schema is missing a source or target table"
            return result
        if source_column not in _table_columns(source_db, source_table) or \
           target_column not in _table_columns(target_db, target_table):
            result["reason"] = "legacy schema is missing a source or target column"
            return result
        qs = '"' + source_table.replace('"', '""') + '"'
        qc = '"' + source_column.replace('"', '""') + '"'
        required = (source_table, source_column) in {
            ("r2_alternate_self_branches", "log_event_id"),
            ("r2_alternate_self_event_links", "branch_id"),
            ("r2_alternate_self_event_links", "event_id"),
            ("r2_diary_entry_links", "diary_entry_id"),
            ("r2_sensory_journal", "event_id"),
            ("r2_sensory_journal_diary_links", "event_id"),
            ("r2_sensory_journal_diary_links", "diary_entry_id"),
            ("r2_log_activity_sessions", "start_event_id"),
            ("r2_log_milestones", "first_event_id"),
            ("r2_log_conversations", "event_id"),
            ("r2_log_inner_states", "event_id"),
            ("r2_log_beliefs", "last_event_id"),
            ("r2_log_continuity", "last_event_id"),
            ("r2_log_hypotheticals", "event_id"),
            ("r2_log_system_snapshots", "event_id"),
            ("r2_log_links", "from_event_id"),
            ("r2_log_links", "to_event_id"),
        }
        select_sql = f"SELECT {qc} FROM {qs}" + ("" if required else f" WHERE {qc} IS NOT NULL")
        values = [row[0] for row in source_db.execute(select_sql)]
        qt = '"' + target_table.replace('"', '""') + '"'
        qtc = '"' + target_column.replace('"', '""') + '"'
        orphan_values: list[Any] = []
        orphan_count = 0
        for value in values:
            if value is None or target_db.execute(
                f"SELECT 1 FROM {qt} WHERE {qtc}=? LIMIT 1", (value,)
            ).fetchone() is None:
                orphan_count += 1
                if len(orphan_values) < 50:
                    orphan_values.append(value)
        result["status"] = "checked"
        result["checked_rows"] = len(values)
        result["orphan_count"] = orphan_count
        result["orphan_values"] = orphan_values
    except sqlite3.Error as exc:
        result["status"] = "error"
        result["reason"] = f"{type(exc).__name__}: {exc}"
    return result


def audit_relationships(
    root: Path, reports: dict[str, dict[str, Any]]
) -> list[dict[str, Any]]:
    """Read-only check that organs' persisted cross-references still resolve."""
    connections: dict[str, sqlite3.Connection | None] = {}
    for name in ("r2_memory.db", "r2_rewards.db"):
        path = root / name
        if not path.is_file() or path.is_symlink():
            connections[name] = None
            continue
        try:
            connections[name] = _readonly_connection(path)
        except sqlite3.Error:
            connections[name] = None

    relations = [
        ("r2_memory.db", "r2_log_events", "session_id", "r2_memory.db", "r2_log_sessions", "id"),
        ("r2_memory.db", "r2_log_events", "related_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_activity_sessions", "start_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_activity_sessions", "end_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_milestones", "first_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_conversations", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_inner_states", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_beliefs", "last_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_continuity", "last_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_links", "from_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_links", "to_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_hypotheticals", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_log_system_snapshots", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_visual_experiences", "life_log_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_alternate_self_branches", "log_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_alternate_self_branches", "evidence_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_alternate_self_event_links", "branch_id", "r2_memory.db", "r2_alternate_self_branches", "id"),
        ("r2_memory.db", "r2_alternate_self_event_links", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_diary_entry_links", "log_event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_diary_entry_links", "diary_entry_id", "r2_memory.db", "diary_entries", "id"),
        ("r2_memory.db", "r2_sensory_journal", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_sensory_journal_diary_links", "event_id", "r2_memory.db", "r2_log_events", "id"),
        ("r2_memory.db", "r2_sensory_journal_diary_links", "diary_entry_id", "r2_memory.db", "diary_entries", "id"),
        ("r2_rewards.db", "reward_events", "log_event_id", "r2_memory.db", "r2_log_events", "id"),
    ]
    output = []
    try:
        for src_name, src_table, src_col, dst_name, dst_table, dst_col in relations:
            output.append(_relationship_check(
                connections[src_name], src_name, src_table, src_col,
                connections[dst_name], dst_name, dst_table, dst_col,
            ))
    finally:
        for connection in connections.values():
            if connection is not None:
                connection.close()
    return output


def audit_root(root: Path) -> dict[str, Any]:
    reports: dict[str, dict[str, Any]] = {}
    for name in ALL_DATABASES:
        path = root / name
        if _path_has_symlink_component(root, name):
            reports[name] = {
                "path": str(path),
                "exists": path.exists() or path.is_symlink(),
                "integrity": None,
                "tables": {},
                "legacy_phantom_food_records": [],
                "foreign_key_violations": [],
                "error": "database path traverses a symlink; refusing to follow it",
            }
        else:
            reports[name] = audit_database(path)

    required = {name: reports[name] for name in DATABASES}
    optional = {name: reports[name] for name in OPTIONAL_DATABASES}
    required_existing = [item for item in required.values() if item["exists"]]
    optional_existing = [item for item in optional.values() if item["exists"]]
    failures = [
        name for name, item in reports.items()
        if item["exists"] and (
            item["error"] is not None or item["integrity"] != ["ok"]
            or item["foreign_key_violations"]
        )
    ]
    missing = [name for name, item in required.items() if not item["exists"]]
    optional_missing = [name for name, item in optional.items() if not item["exists"]]
    relationships = audit_relationships(root, reports)
    relationship_failures = [
        item for item in relationships
        if item["status"] == "error" or item.get("orphan_count", 0) > 0
    ]
    legacy = [
        {"database": name, **record}
        for name, item in reports.items()
        for record in item["legacy_phantom_food_records"]
    ]
    return {
        "root": str(root.resolve()),
        "read_only": True,
        "databases": reports,
        "relationships": relationships,
        "summary": {
            "found": len(required_existing),
            "expected": len(DATABASES),
            "missing": missing,
            "optional_found": len(optional_existing),
            "optional_expected": len(OPTIONAL_DATABASES),
            "optional_missing": optional_missing,
            "integrity_failures": failures,
            "relationship_failures": relationship_failures,
            "relationship_checks_not_run": [
                item for item in relationships if item["status"] == "not_checked"
            ],
            "legacy_phantom_food_records": legacy,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True,
                        help="R2 data directory containing SQLite database files")
    parser.add_argument("--snapshot-dir", type=Path,
                        help="optional destination for non-overwriting SQLite snapshots")
    parser.add_argument("--json", action="store_true",
                        help="emit machine-readable JSON")
    args = parser.parse_args()

    audit_root_path = args.root
    snapshot_errors: list[str] = []
    snapshot_created = False
    if args.snapshot_dir is not None:
        try:
            created = create_snapshot_set(args.root, args.snapshot_dir)
            if created:
                audit_root_path = args.snapshot_dir
                snapshot_created = True
        except (OSError, sqlite3.Error) as exc:
            snapshot_errors.append(f"snapshot set: {type(exc).__name__}: {exc}")
            # On any snapshot failure, audit the originals read-only instead of
            # accidentally treating an old/partial snapshot as current data.
            audit_root_path = args.root

    report = audit_root(audit_root_path)
    report["source_root"] = str(args.root.resolve())
    report["snapshot_errors"] = snapshot_errors
    report["snapshot_created"] = snapshot_created
    report["snapshot_dir"] = str(args.snapshot_dir.resolve()) if args.snapshot_dir else None
    if args.json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        heading = "snapshot audit" if snapshot_created else "read-only audit"
        print(f"R2 database preflight ({heading}): {report['root']}")
        if snapshot_created:
            print(f"Source: {args.root.resolve()}")
        for name, item in report["databases"].items():
            if not item["exists"]:
                label = "OPTIONAL MISSING" if name in OPTIONAL_DATABASES else "MISSING"
                print(f"  {label}  {name}")
                continue
            integrity = ", ".join(item["integrity"] or []) or "unavailable"
            print(f"  {'ERROR' if item['error'] or integrity != 'ok' else 'OK'}  "
                  f"{name}: integrity={integrity}, tables={len(item['tables'])}")
            if item["error"]:
                print(f"           error: {item['error']}")
            for table, detail in item["tables"].items():
                print(f"           {table}: {detail['rows']} rows")
            for record in item["legacy_phantom_food_records"]:
                print(f"           WARNING: {record['rows']} legacy phantom-food "
                      f"row(s) in {record['table']}")
        for error in snapshot_errors:
            print(f"SNAPSHOT ERROR: {error}")
        summary = report["summary"]
        print(f"Summary: {summary['found']}/{summary['expected']} required database files found; "
              f"{summary['optional_found']}/{summary['optional_expected']} optional device database files found; "
              f"{len(summary['integrity_failures'])} integrity/foreign-key failure(s); "
              f"{len(summary['relationship_failures'])} broken relationship(s); "
              f"{len(summary['legacy_phantom_food_records'])} legacy phantom-food "
              "record group(s).")
        for relationship in summary["relationship_failures"]:
            print(f"  RELATIONSHIP ERROR: {relationship['name']}: "
                  f"status={relationship['status']}; "
                  f"orphan_values={relationship.get('orphan_values', [])}")
        if summary["missing"]:
            print("Note: missing files are reported, not created.")
        if summary["legacy_phantom_food_records"]:
            print("Note: suspicious legacy rows are reported only; no rows are changed.")
        if snapshot_created:
            print("Snapshots are per-database; stop R2 first for cross-database consistency.")

    # Missing expected databases are a failed preflight, not a clean pass.
    # The report remains read-only and still lists every missing path.
    return 2 if (report["summary"]["integrity_failures"] or
                  report["summary"]["relationship_failures"] or
                  report["summary"]["missing"] or snapshot_errors) else 0


if __name__ == "__main__":
    sys.exit(main())
