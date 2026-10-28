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
    assert all(item["status"] == "checked" for item in relationships), relationships
    assert all(item["orphan_count"] == 0 for item in relationships), relationships

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

print("Read-only database audit detects SQLite FK violations and broken cross-organ references.")
