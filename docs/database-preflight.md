# R2 database preflight

`tools/audit_r2_databases.py` inspects the SQLite files in R2's data directory:

- `r2_reality.db`
- `r2_fridge.db`
- `r2_memory.db`
- `r2_rewards.db`
- `r2_addictions.db`

It opens existing databases read-only, runs SQLite's `integrity_check`, reports user tables/row counts, and flags exact matches for the legacy phantom-burger description. It does not repair databases, initialize missing databases, migrate schemas, or delete flagged records.

When present, it also audits and snapshots the Game Boy cartridge database and runtime/event database:

- `Devices/GameBoyAdvance/gameboy.db`
- `Devices/GameBoyAdvance/State/console_state.db`

Those device databases are optional for installations without the Game Boy subsystem. Missing core databases fail preflight; missing optional device databases are reported separately. Corrupt or unreadable optional databases still count as integrity failures.

## Audit a stopped R2 instance

Pass the directory that contains the database files (normally `/home/x/R2_Home/R2`):

```sh
python3 tools/audit_r2_databases.py --root /home/x/R2_Home/R2
```

Add `--json` for machine-readable output.

## Snapshot and audit

The script can use SQLite's backup API to create a per-database snapshot and audit the copies:

```sh
python3 tools/audit_r2_databases.py \
  --root /home/x/R2_Home/R2 \
  --snapshot-dir /home/x/R2_Home/database-preflight-snapshot
```

Existing snapshot files are never overwritten. Choose a new or empty destination directory. If any expected snapshot filename already exists, the script refuses the whole snapshot set before copying anything, so it cannot silently mix stale and fresh database copies. New snapshot files are created with owner-only permissions (0600). Source database files are opened read-only; suspicious rows are reported, not altered.

Snapshots are consistent individually, but the separate databases are not captured at one atomic instant. Stop R2 before running the audit when cross-database consistency matters. Keep the snapshot directory private because these databases may contain personal memories and activity records. Do not upload the database files to public issues or logs.

A missing database is reported as missing and is not created. Because this is a deployment preflight for the five expected operational databases, any missing database, unreadable/corrupt database, integrity failure, or snapshot error yields a non-zero exit status. If you intentionally run a partial installation, treat the missing-database report as an expected limitation rather than allowing the preflight to pass silently.

## Compare the Visual archive with its index

Use the archive directory from the same stopped R2 instance as the database:

```sh
python3 tools/audit_r2_databases.py \
  --root /home/x/R2_Home/R2 \
  --visual-library /home/x/R2_Home/Visual_Library
```

Use `--visual-db` if Visual was configured with a separate database. When auditing a database snapshot, pair it with the archive from the same stopped instance. The database backup does not copy JPEGs.

The report separates missing indexed files, unindexed JPEG candidates, unresolved relative paths, and pending Life Log links. Older rows without a recovery key are listed separately. Pending archives have a saved interpretation and are listed separately from files that lack an index or pending record. The report indicates file presence. Visual verifies the complete bytes during restart recovery. While R2 is running, a candidate may be a save that has reached disk before the database commit. The comparison reads file metadata and the index, leaving archives and database records in place.

A copied archive can have the same filenames under a different directory while its database still records the original absolute paths. The archive comparison reports those outside paths and any matching local filenames. A filename match alone establishes a possible copy for review. Restoring the original configured paths keeps existing references usable.

The archive report separates `pending_archives` from `withdrawn_archives`.
Pending records retain the original interpretation and encoded bytes after a
storage failure; startup publishes only when the complete owned file verifies.
Missing or partial files remain pending. Withdrawn records are cancelled saves
whose cleanup did not finish. Startup excludes them even if a matching JPEG is
restored later. The read-only report does not delete either kind of record.
