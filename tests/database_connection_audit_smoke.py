#!/usr/bin/env python3
"""Exercise read-only SQLite integrity and cross-organ relationship audits."""
from pathlib import Path
import sqlite3
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import audit_r2_databases as audit  # noqa: E402

with tempfile.TemporaryDirectory(prefix="r2-db-links-") as tmp:
    root = Path(tmp)
    memory = root / "r2_memory.db"
    rewards = root / "r2_rewards.db"

    with sqlite3.connect(memory) as db:
        db.executescript("""
            CREATE TABLE r2_log_events(id INTEGER PRIMARY KEY);
            CREATE TABLE diary_entries(id INTEGER PRIMARY KEY);
            CREATE TABLE r2_visual_experiences(id INTEGER PRIMARY KEY, life_log_event_id INTEGER);
            CREATE TABLE r2_alternate_self_branches(
                id INTEGER PRIMARY KEY, log_event_id INTEGER, evidence_event_id INTEGER
            );
            CREATE TABLE r2_alternate_self_event_links(
                branch_id INTEGER, event_id INTEGER
            );
            CREATE TABLE r2_diary_entry_links(
                diary_entry_id INTEGER, log_event_id INTEGER
            );
            CREATE TABLE r2_sensory_journal(event_id INTEGER PRIMARY KEY);
            CREATE TABLE r2_sensory_journal_diary_links(
                event_id INTEGER, diary_entry_id INTEGER
            );
            INSERT INTO r2_log_events(id) VALUES(1),(2);
            INSERT INTO diary_entries(id) VALUES(10);
            INSERT INTO r2_visual_experiences VALUES(1,1);
            INSERT INTO r2_alternate_self_branches VALUES(1,1,2);
            INSERT INTO r2_alternate_self_event_links VALUES(1,2);
            INSERT INTO r2_diary_entry_links VALUES(10,1);
            INSERT INTO r2_sensory_journal VALUES(1);
            INSERT INTO r2_sensory_journal_diary_links VALUES(1,10);
        """)
    with sqlite3.connect(rewards) as db:
        db.executescript("""
            CREATE TABLE reward_events(id INTEGER PRIMARY KEY, log_event_id INTEGER);
            INSERT INTO reward_events VALUES(1,1);
        """)

    reports = audit.audit_root(root)
    assert reports["read_only"] is True
    relationships = audit.audit_relationships(root, reports["databases"])
    assert relationships
    checked_relationships = [item for item in relationships if item["status"] == "checked"]
    assert checked_relationships
    assert all(item["status"] in ("checked", "not_checked") for item in relationships), relationships
    assert all(item["orphan_count"] == 0 for item in checked_relationships), checked_relationships

    with sqlite3.connect(memory) as db:
        db.execute("INSERT INTO r2_alternate_self_branches VALUES(2,NULL,NULL)")
    relationships = audit.audit_relationships(root, reports["databases"])
    missing_required = [
        item for item in relationships
        if item["source_table"] == "r2_alternate_self_branches"
        and item["source_column"] == "log_event_id"
    ]
    assert len(missing_required) == 1 and missing_required[0]["orphan_values"] == [None], missing_required
    with sqlite3.connect(memory) as db:
        db.execute("DELETE FROM r2_alternate_self_branches WHERE id=2")

    with sqlite3.connect(rewards) as db:
        db.execute("INSERT INTO reward_events VALUES(2,999)")
    relationships = audit.audit_relationships(root, reports["databases"])
    broken = [item for item in relationships if item["orphan_count"]]
    assert len(broken) == 1, broken
    assert broken[0]["source_table"] == "reward_events"
    assert broken[0]["orphan_values"] == [999]

    # Verify PRAGMA foreign_key_check detects broken in-file organ links even
    # when the source database was created with foreign-key enforcement off.
    with sqlite3.connect(memory) as db:
        db.executescript("""
            CREATE TABLE fk_parent(id INTEGER PRIMARY KEY);
            CREATE TABLE fk_child(parent_id INTEGER REFERENCES fk_parent(id));
            INSERT INTO fk_child(parent_id) VALUES(404);
        """)
    fk_report = audit.audit_database(memory)
    assert fk_report["integrity"] == ["ok"]
    assert fk_report["foreign_key_violations"], fk_report

    # Auditing must not repair or rewrite the original rows.
    with sqlite3.connect(rewards) as db:
        assert db.execute("SELECT log_event_id FROM reward_events WHERE id=2").fetchone()[0] == 999

    archive = root / "Visual_Library"
    archive.mkdir()
    owned = "visual_10_20_0123456789abcdef.jpg"
    candidate = "visual_10_20_abcdef0123456789.jpg"
    legacy = "visual_11_22.jpg"
    relative = "visual_12_22_0123456789abcdef.jpg"
    staged = "visual_14_23_0123456789abcdef.jpg"
    for name in (owned, candidate, legacy, relative, staged, "family-photo.jpg"):
        (archive / name).write_bytes(b"fixture")
    (archive / "visual_13_22_0123456789abcdef.jpg").symlink_to(archive / owned)
    visual_db = root / "detached.db"
    with sqlite3.connect(visual_db) as db:
        db.executescript("""
            CREATE TABLE r2_visual_experiences(
                id INTEGER PRIMARY KEY, image_path TEXT, file_path TEXT,
                life_log_event_id INTEGER, life_log_archive_key TEXT
            );
        """)
        missing = str(archive / "missing.jpg")
        db.executemany("INSERT INTO r2_visual_experiences VALUES(?,?,?,?,?)", [
            (1, "", str(archive / owned), 17, None),
            (2, missing, None, None, "pending-key"),
            (3, f"archive/{relative}", None, None, None),
        ])
        db.execute("CREATE TABLE r2_visual_pending_archives(id INTEGER PRIMARY KEY, image_path TEXT)")
        db.executemany("INSERT INTO r2_visual_pending_archives VALUES(?,?)", [
            (21, str(archive / staged)), (22, str(archive / "missing-staged.jpg")),
        ])
    visual = audit.audit_visual_archive(visual_db, archive)
    assert visual["error"] is None
    assert visual["unindexed_candidates"] == [candidate]
    assert visual["legacy_unindexed_candidates"] == [legacy]
    assert visual["missing_indexed_files"] == [{"id": 2, "path": missing}]
    assert visual["pending_life_log_ids"] == [2]
    assert visual["pending_archives"] == [
        {"id": 21, "path": str(archive / staged), "file_present": True},
        {"id": 22, "path": str(archive / "missing-staged.jpg"), "file_present": False},
    ]
    assert visual["legacy_unlinked_ids"] == [3]
    assert visual["unresolved_relative_paths"] == [{"id": 3, "path": f"archive/{relative}"}]
    assert (archive / candidate).read_bytes() == b"fixture"
    with sqlite3.connect(visual_db) as db:
        assert db.execute("SELECT life_log_event_id FROM r2_visual_experiences WHERE id=2").fetchone() == (None,)

    # An older pending table upgrades additively; cancelled cleanup records
    # remain distinct even if their original JPEG is restored later.
    with sqlite3.connect(visual_db) as db:
        db.execute("ALTER TABLE r2_visual_pending_archives ADD COLUMN withdrawn INTEGER NOT NULL DEFAULT 0")
        db.executemany("INSERT INTO r2_visual_pending_archives VALUES(?,?,1)", [
            (23, str(archive / "withdrawn.jpg")),
            (24, str(archive / "absent-withdrawn.jpg")),
        ])
    (archive / "withdrawn.jpg").write_bytes(b"cancelled fixture")
    withdrawn_report = audit.audit_visual_archive(visual_db, archive)
    assert withdrawn_report["error"] is None
    assert withdrawn_report["pending_archives"] == visual["pending_archives"]
    assert withdrawn_report["withdrawn_archives"] == [
        {"id": 23, "path": str(archive / "withdrawn.jpg"), "file_present": True},
        {"id": 24, "path": str(archive / "absent-withdrawn.jpg"), "file_present": False},
    ]

    # A copied backup keeps the original absolute paths. Same-named files in
    # the copy are candidates for relocation, rather than unrecorded memories.
    restored = root / "restored_archive"
    restored.mkdir()
    for name in (owned, staged):
        (restored / name).write_bytes(b"restored fixture")
    moved = audit.audit_visual_archive(visual_db, restored)
    assert moved["error"] is None
    assert not moved["unindexed_candidates"]
    outside = moved["paths_outside_library"]
    assert {"record_type": "published", "id": 1, "path": str(archive / owned),
            "matching_filename": str(restored / owned)} in outside
    assert {"record_type": "pending", "id": 21, "path": str(archive / staged),
            "matching_filename": str(restored / staged)} in outside
    with sqlite3.connect(visual_db) as db:
        assert db.execute("SELECT file_path FROM r2_visual_experiences WHERE id=1").fetchone() == (str(archive / owned),)
        assert db.execute("SELECT image_path FROM r2_visual_pending_archives WHERE id=21").fetchone() == (str(archive / staged),)

print("Read-only database audit detects SQLite FK violations and broken cross-organ references.")
