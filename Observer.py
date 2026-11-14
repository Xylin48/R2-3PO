#!/usr/bin/env python3
"""Read-only desktop observer for R2's factual public event summaries."""
from __future__ import annotations
import errno
import fcntl
import os
import re
import sqlite3
import time
from pathlib import Path
DATABASE = Path(os.environ.get("R2_OBSERVER_DATABASE", "/home/x/R2_Home/R2/r2_memory.db"))
ACTIVITY_FILE = Path(os.environ.get(
    "R2_ACTIVITY_FILE", "/home/x/R2_Home/R2_Log/Activity.txt"
))
POLL_MS = 900
MAX_ROWS = 250
# New event types remain hidden unless explicitly reviewed and added here.
PUBLIC_EVENT_TYPES = frozenset({
    "object_added", "object_moved", "object_removed", "creator_gift", "fridge_item_taken",
    "fridge_food_consumed", "fridge_item_stored", "food_consumed", "purchase",
    # Public, factual changes in modeled enjoyment/addiction status; private thoughts remain excluded.
    "enjoyment_changed", "addiction_status_changed",
    # Generic future hooks for verified activity sessions and location changes.
    "activity_started", "activity_progress", "activity_ended",
    "location_changed", "departure", "arrival",
    # Explicitly public Game Boy device/session facts; raw button presses stay private to the device log.
    "gameboy_device_action", "gameboy_game_started", "gameboy_console_powered_off",
    "gameboy_game_event",
    # CRT/VCR device events are public, factual world/media events.
    "tv_display_opened", "tv_display_closed", "tv_power", "tv_source_selected",
    "tv_rf_tuned", "tv_device_connected", "tv_device_disconnected",
    "vcr_tape_inserted", "vcr_transport_changed", "vcr_tape_ejected",
})
def is_observer_event(category: str, event_type: str) -> bool:
    """True only for explicitly approved factual event types/categories."""
    return event_type in PUBLIC_EVENT_TYPES and category in {"world", "media"}
# Activity.txt is a separate file from the authoritative SQLite Life Log.
# Rebuild the exact event-ID index from complete, newline-terminated records
# rather than guessing from a 4096-byte suffix or an in-memory session cursor.
ACTIVITY_EVENT_MARKER = re.compile(rb" \\(event #([1-9][0-9]*)\\)\\n$")


class ActivityWriter:
    def __init__(self, path: Path):
        self.path = path
        self._identity = None
        self._offset = 0
        self._saved_ids = set()

    def append(self, local_time: str, summary: str, event_id: int) -> None:
        if event_id <= 0:
            raise ValueError("Activity event ID must be positive")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a+b") as stream:
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            stat = os.fstat(stream.fileno())
            identity = (stat.st_dev, stat.st_ino)
            if identity != self._identity or stat.st_size < self._offset:
                # An operator may rotate or truncate the log. Never reuse an
                # index from a different file or earlier generation.
                self._identity = identity
                self._offset = 0
                self._saved_ids.clear()
            stream.seek(self._offset)
            while True:
                line = stream.readline()
                if not line:
                    break
                if not line.endswith(b"\\n"):
                    # An interrupted append is preserved exactly for repair,
                    # not combined with a later line or silently discarded.
                    raise OSError(errno.EIO, "Activity.txt ends with an incomplete line")
                try:
                    line.decode("utf-8")
                except UnicodeDecodeError as exc:
                    raise OSError(errno.EILSEQ, "Activity.txt contains invalid UTF-8") from exc
                marker = ACTIVITY_EVENT_MARKER.search(line)
                if marker:
                    self._saved_ids.add(int(marker.group(1)))
            self._offset = stream.tell()
            if event_id in self._saved_ids:
                return
            if self._offset == 0:
                stream.write(b"R2-3PO Activity Log\\n")
                stream.write(b"Factual observer events only; private thoughts and notifications excluded.\\n")
                stream.write(("-" * 72 + "\\n").encode("utf-8"))
            clean_time = " ".join((local_time or "time unavailable").split())
            clean_summary = " ".join((summary or "").split())
            stream.write(f"[{clean_time}] {clean_summary} (event #{event_id})\\n".encode("utf-8"))
            stream.flush()
            # Mark an event complete only after its entire record is durable.
            # A failed fsync leaves it discoverable in the next full/delta scan.
            os.fsync(stream.fileno())
            self._offset = stream.tell()
            self._saved_ids.add(event_id)


def append_activity_line(path: Path, local_time: str, summary: str, event_id: int) -> None:
    """Standalone append helper; the GUI reuses one indexed writer for speed."""
    ActivityWriter(path).append(local_time, summary, event_id)


def open_database():
    # The window cannot create or mutate the private Life Log database.
    uri = DATABASE.resolve().as_uri() + "?mode=ro"
    connection = sqlite3.connect(uri, uri=True, timeout=2.0)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA query_only=ON")
    return connection
def reconcile_activity_page(connection, writer: ActivityWriter, cursor: int,
                            latest: int, batch_size: int = MAX_ROWS):
    """Replay permitted public records in bounded, exact-ID order.

    The Life Log remains query-only. A failed append never advances the
    cursor; the file index makes replay after an uncertain flush idempotent.
    """
    allowed_types = tuple(sorted(PUBLIC_EVENT_TYPES))
    placeholders = ",".join("?" for _ in allowed_types)
    rows = connection.execute(
        "SELECT id,local_time,category,event_type,summary FROM r2_log_events "
        "WHERE id>? AND id<=? AND category IN ('world','media') "
        f"AND event_type IN ({placeholders}) ORDER BY id ASC LIMIT ?",
        (cursor, latest, *allowed_types, batch_size),
    ).fetchall()
    for row in rows:
        writer.append(row["local_time"], row["summary"], int(row["id"]))
    return (latest if len(rows) < batch_size else int(rows[-1]["id"])), rows


def main() -> int:
    try:
        import tkinter as tk
        from tkinter import ttk
    except ImportError:
        print("[R2 Observer] Tkinter is unavailable; install python3-tk to use the popup.")
        return 2
    root = tk.Tk()
    root.title("R2-3PO — Observer Log")
    root.geometry("760x460")
    root.minsize(520, 280)
    header = ttk.Frame(root, padding=(12, 10, 12, 6))
    header.pack(fill="x")
    ttk.Label(header, text="R2-3PO", font=("TkDefaultFont", 14, "bold")).pack(side="left")
    ttk.Label(header, text="Factual activity timeline · private thoughts excluded",
              foreground="#666666").pack(side="right")
    body = ttk.Frame(root, padding=(10, 4, 10, 10))
    body.pack(fill="both", expand=True)
    tree = ttk.Treeview(body, columns=("time", "event"), show="headings")
    tree.heading("time", text="R2 time")
    tree.heading("event", text="Observed event")
    tree.column("time", width=155, minwidth=130, stretch=False)
    tree.column("event", width=560, minwidth=250, stretch=True)
    scroll = ttk.Scrollbar(body, orient="vertical", command=tree.yview)
    tree.configure(yscrollcommand=scroll.set)
    tree.pack(side="left", fill="both", expand=True)
    scroll.pack(side="right", fill="y")
    status = tk.StringVar(value="Waiting for R2's Life Log…")
    ttk.Label(root, textvariable=status, anchor="w", padding=(12, 4)).pack(fill="x")
    last_id = None
    display_cutoff = None
    activity_writer = ActivityWriter(ACTIVITY_FILE)
    connection = None
    last_open_attempt = 0.0
    activity_file_error = False
    allowed_types = tuple(sorted(PUBLIC_EVENT_TYPES))
    allowed_placeholders = ",".join("?" for _ in allowed_types)
    def poll():
        nonlocal last_id, display_cutoff, connection, last_open_attempt, activity_file_error
        try:
            if connection is None and time.monotonic() - last_open_attempt >= 2.0:
                last_open_attempt = time.monotonic()
                if DATABASE.exists():
                    try:
                        connection = open_database()
                    except sqlite3.Error:
                        connection = None
            if connection is not None:
                latest = connection.execute(
                    "SELECT COALESCE(MAX(id), 0) FROM r2_log_events"
                ).fetchone()[0]
                # The popup initially shows the latest approved summaries.
                # Activity.txt independently catches up from the first ID,
                # including events created while this popup was closed.
                if last_id is None:
                    rows = connection.execute(
                        "SELECT id, local_time, category, event_type, summary "
                        "FROM r2_log_events WHERE id <= ? "
                        "AND category IN ('world','media') "
                        f"AND event_type IN ({allowed_placeholders}) ORDER BY id DESC LIMIT ?",
                        (latest, *allowed_types, MAX_ROWS),
                    ).fetchall()
                    for row in reversed(rows):
                        summary = " ".join((row["summary"] or "").split())
                        local_time = row["local_time"] or "time unavailable"
                        tree.insert("", "end", iid=str(row["id"]),
                                    values=(local_time, summary))
                    last_id = 0
                    display_cutoff = int(latest)
                try:
                    next_cursor, rows = reconcile_activity_page(
                        connection, activity_writer, last_id, int(latest))
                except OSError as exc:
                    # Retry the same batch. Exact-ID indexing prevents a
                    # duplicate if the full line reached disk before failure.
                    activity_file_error = True
                    status.set(f"Live window · Activity.txt write failed: {exc}")
                else:
                    for row in rows:
                        event_id = int(row["id"])
                        if event_id <= display_cutoff:
                            continue
                        item_id = str(event_id)
                        if not tree.exists(item_id):
                            summary = " ".join((row["summary"] or "").split())
                            tree.insert("", "end", iid=item_id,
                                        values=(row["local_time"] or "time unavailable", summary))
                    last_id = next_cursor
                    activity_file_error = False
                children = tree.get_children()
                if len(children) > MAX_ROWS:
                    for item in children[:len(children) - MAX_ROWS]:
                        tree.delete(item)
                if children:
                    tree.yview_moveto(1.0)
                if not activity_file_error:
                    status.set(f"Live · public factual events only · saving to {ACTIVITY_FILE}")
        except sqlite3.Error:
            if connection is not None:
                try:
                    connection.close()
                except sqlite3.Error:
                    pass
                connection = None
            status.set("Life Log temporarily unavailable; retrying…")
        root.after(POLL_MS, poll)
    def close():
        if connection is not None:
            try:
                connection.close()
            except sqlite3.Error:
                pass
        root.destroy()
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(100, poll)
    root.mainloop()
    return 0
if __name__ == "__main__":
    raise SystemExit(main())
