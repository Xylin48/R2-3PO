#!/usr/bin/env python3
"""Reality-backed CRT display and built-in VCR renderer for R2-3PO.

Reality remains authoritative for TV controls, inserted tape, transport state,
and per-file playback position. This GUI reads SQLite read-only and sends all
state changes to the running R2 shell over its private Unix socket.
"""
from __future__ import annotations

import math
import os
from contextlib import closing
import socket
import sqlite3
import time
import tkinter as tk
from tkinter import filedialog
from pathlib import Path

DB_PATH = Path(os.environ.get("R2_REALITY_DB", "/home/x/R2_Home/R2/r2_reality.db"))
SOCKET_PATH = Path(os.environ.get("R2_TV_SOCKET", "/home/x/R2_Home/R2/tv-control.sock"))
MEDIA_DIR = Path(os.environ.get("R2_VCR_MEDIA_DIR", "/home/x/R2_Home/VCR_Tapes"))
POLL_MS = 1000
POSITION_SAVE_SECONDS = 5
VCR_SEEK_TIMEOUT_SECONDS = 15
# VLC reports media time in milliseconds; allow small decoder/keyframe drift
# rather than treating mere seek submission as proof of resumed playback.
VCR_SEEK_TOLERANCE_MS = 250
VCR_PATH_MAX_BYTES = os.pathconf("/", "PC_PATH_MAX") - 1
MEDIA_EXTENSIONS = (
    ".avi", ".mkv", ".mp4", ".m4v", ".mov", ".mpeg", ".mpg", ".wmv",
    ".webm", ".ogv", ".flv", ".ts", ".vob", ".3gp", ".asf", ".m2ts",
    ".mts",
)


def _valid_x11_id(value: str) -> bool:
    return (value.isascii() and value.isdecimal() and len(value) <= 10
            and 1 <= int(value) <= 0xFFFFFFFF)


def _valid_media_path(path: str) -> bool:
    return (Path(path).is_absolute() and not any(c in path for c in "\r\n\0")
            and len(path.encode("utf-8")) <= VCR_PATH_MAX_BYTES)


def send_control_command(command: str) -> str:
    """Send a bounded request to R2; C routes it through Reality APIs."""
    allowed = {"display opened", "display closed", "power on", "power off", "vcr play", "vcr pause", "vcr stop", "vcr eject"}
    valid = command in allowed
    valid = valid or (
        command.startswith("input ") and command[6:].isascii()
        and len(command[6:]) == 1 and command[6:].isdecimal()
        and 1 <= int(command[6:]) <= 4
    )
    valid = valid or (
        command.startswith("tune ") and command[5:].isascii()
        and 1 <= len(command[5:]) <= 2 and command[5:].isdecimal()
        and 2 <= int(command[5:]) <= 13
    )
    if command.startswith("vcr insert "):
        valid = _valid_media_path(command[11:])
    if command.startswith(("media ", "media-path ")):
        if command.startswith("media-path "):
            action, separator, path = command[11:].partition("|")
            valid_window = True
        else:
            action, separator, path_and_window = command[6:].partition("|")
            path, marker, window_id = path_and_window.rpartition("|x11=")
            valid_window = not marker or _valid_x11_id(window_id)
            if not marker:
                path = path_and_window
        valid = (
            separator != ""
            and valid_window
            and action in {
                "vcr_playback_started", "vcr_playback_paused",
                "vcr_playback_stopped", "vcr_playback_ended",
                "vcr_playback_error",
            }
            and _valid_media_path(path)
        )
    if command.startswith("renderer "):
        window_id, marker, path = command[9:].partition("|")
        valid = (command == "renderer 0"
                 or bool(marker) and _valid_x11_id(window_id)
                 and _valid_media_path(path))
    if command.startswith("vcr tape-position "):
        seconds, marker, path = command[18:].partition("|")
        try:
            valid = bool(marker) and 0 <= float(seconds) <= 604800 and _valid_media_path(path)
        except ValueError:
            valid = False
    if command.startswith("vcr position "):
        try:
            seconds = float(command[13:])
            valid = 0 <= seconds <= 604800
        except ValueError:
            valid = False
    if not valid or len(command.encode("utf-8")) > VCR_PATH_MAX_BYTES + 127:
        raise ValueError("Unsupported TV/VCR control command")
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(2.0)
        client.connect(str(SOCKET_PATH))
        client.sendall((command + "\n").encode("utf-8"))
        chunks: list[bytes] = []
        total = 0
        while total < 16384:
            chunk = client.recv(min(4096, 16384 - total))
            if not chunk:
                break
            chunks.append(chunk)
            total += len(chunk)
        if total >= 16384:
            raise RuntimeError("R2 control response exceeded the 16 KiB safety limit")
        response = b"".join(chunks).decode("utf-8")
    if response != "OK" and not response.startswith("OK "):
        raise RuntimeError(response or "R2 returned no response")
    return response


class CRTDisplay:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        root.title("R2-3PO — CRT Television")
        root.geometry("760x700")
        root.minsize(560, 590)
        root.configure(bg="#242424")

        bezel = tk.Frame(root, bg="#343434", padx=26, pady=24,
                         highlightthickness=2, highlightbackground="#555555")
        bezel.pack(fill="both", expand=True, padx=18, pady=18)

        self.screen = tk.Frame(bezel, bg="#080b08", highlightthickness=5,
                               highlightbackground="#111111")
        self.screen.pack(fill="both", expand=True)
        self.video_surface = tk.Frame(self.screen, bg="#080b08")
        self.video_surface.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.picture = tk.Label(
            self.screen, text="TV OFF", bg="#080b08", fg="#a6b99c",
            font=("DejaVu Sans Mono", 24, "bold"), justify="center",
        )
        self.picture.place(relx=0, rely=0, relwidth=1, relheight=1)
        self.video_surface.lower()

        self.status = tk.Label(
            root, text="Reading persistent TV state…", anchor="w",
            bg="#242424", fg="#dddddd", font=("DejaVu Sans Mono", 10),
            padx=24, pady=4,
        )
        self.status.pack(fill="x")

        controls = tk.Frame(root, bg="#242424", padx=18, pady=8)
        controls.pack(fill="x")
        power_row = tk.Frame(controls, bg="#242424")
        power_row.pack(fill="x", pady=(0, 5))
        self._button(power_row, "POWER ON", lambda: self.control("power on")).pack(side="left", padx=(0, 6))
        self._button(power_row, "POWER OFF", lambda: self.control("power off")).pack(side="left")

        input_row = tk.Frame(controls, bg="#242424")
        input_row.pack(fill="x", pady=(0, 5))
        tk.Label(input_row, text="AV INPUT", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        for number in range(1, 5):
            self._button(input_row, str(number), lambda n=number: self.control(f"input {n}")).pack(side="left", padx=2)

        rf_row = tk.Frame(controls, bg="#242424")
        rf_row.pack(fill="x")
        tk.Label(rf_row, text="RF CHANNEL", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        self.rf_channel = tk.Spinbox(rf_row, from_=2, to=13, width=4, font=("DejaVu Sans Mono", 10))
        self.rf_channel.pack(side="left", padx=(0, 8))
        self._button(rf_row, "TUNE", self.tune_rf).pack(side="left")

        vcr_row = tk.Frame(controls, bg="#242424")
        vcr_row.pack(fill="x", pady=(5, 0))
        tk.Label(vcr_row, text="BUILT-IN VCR", bg="#242424", fg="#dddddd").pack(side="left", padx=(0, 8))
        self._button(vcr_row, "LOAD TAPE", self.load_tape).pack(side="left", padx=2)
        self._button(vcr_row, "PLAY", lambda: self.control("vcr play")).pack(side="left", padx=2)
        self._button(vcr_row, "PAUSE", lambda: self.control("vcr pause")).pack(side="left", padx=2)
        self._button(vcr_row, "STOP", lambda: self.control("vcr stop")).pack(side="left", padx=2)
        self._button(vcr_row, "EJECT", lambda: self.control("vcr eject")).pack(side="left", padx=2)

        self.feedback = tk.Label(
            controls, text="Controls require R2 to be running.", anchor="w",
            bg="#242424", fg="#dddddd", font=("DejaVu Sans Mono", 9),
        )
        self.feedback.pack(fill="x", pady=(6, 0))

        self._vlc = None
        self._vlc_instance = None
        self._player = None
        self._loaded_path: str | None = None
        self._last_transport: str | None = None
        self._last_position_save = time.monotonic()
        self._vlc_error_shown = False
        self._pending_vcr_seek: object | None = None
        self._last_logged_vlc_state: tuple[str, str] | None = None
        self._pending_playback_stop = False
        self._renderer_published = False
        root.protocol("WM_DELETE_WINDOW", self.close)
        # Record the GUI opening through Reality, even while the modeled TV is off.
        # The event also represents the requested forced-attention cue.
        root.after(100, self.display_opened)
        self.refresh()

    @staticmethod
    def _button(parent: tk.Widget, text: str, command) -> tk.Button:
        return tk.Button(parent, text=text, command=command, bg="#444444", fg="#eeeeee",
                         activebackground="#606060", activeforeground="#ffffff",
                         relief="raised", padx=8, pady=3)

    def control(self, command: str) -> None:
        try:
            # Save the outgoing tape before transport changes or inserting another.
            if command.startswith("vcr ") and not command.startswith("vcr position "):
                self._save_vcr_position(force=True)
            response = send_control_command(command)
            self.feedback.configure(text=response, fg="#a6b99c")
            self.refresh(schedule=False)
        except (OSError, RuntimeError, ValueError) as exc:
            self.feedback.configure(text=f"Control unavailable: {exc}", fg="#e0b0a0")

    def load_tape(self) -> None:
        path = filedialog.askopenfilename(
            parent=self.root, title="Insert a video/audio tape",
            initialdir=str(MEDIA_DIR if MEDIA_DIR.is_dir() else Path.home()),
            filetypes=[("VLC media", " ".join(f"*{ext}" for ext in MEDIA_EXTENSIONS)),
                       ("All files", "*.*")],
        )
        if path:
            self.control("vcr insert " + str(Path(path).resolve()))

    def tune_rf(self) -> None:
        try:
            channel = int(self.rf_channel.get())
        except ValueError:
            self.feedback.configure(text="Choose an RF channel from 2 to 13.", fg="#e0b0a0")
            return
        self.control(f"tune {channel}")

    def read_state(self) -> tuple[int, str, int, str | None]:
        uri = DB_PATH.resolve().as_uri() + "?mode=ro"
        with closing(sqlite3.connect(uri, uri=True, timeout=1.5)) as db:
            db.execute("BEGIN")
            return self._read_state(db)

    @staticmethod
    def _read_state(db: sqlite3.Connection) -> tuple[int, str, int, str | None]:
        row = db.execute(
            "SELECT power, source_kind, source_value FROM r2_tv_state WHERE id=1"
        ).fetchone()
        if row is None:
            raise RuntimeError("TV state row is missing")
        power, kind, value = row
        if (type(power) is not int or power not in (0, 1)
                or type(kind) is not str or kind not in ("input", "rf", "vcr")
                or type(value) is not int
                or not (1 <= value <= 4 if kind == "input" else
                        2 <= value <= 13 if kind == "rf" else value == 1)):
            raise ValueError("Stored TV selection is invalid")
        device = None
        if kind == "rf":
            match = db.execute(
                "SELECT name FROM r2_tv_devices "
                "WHERE connected=1 AND connection_kind='rf' AND port=? LIMIT 1",
                (value,),
            ).fetchone()
            device = match[0] if match else None
        elif kind == "input" and value > 1:
            match = db.execute(
                "SELECT name FROM r2_tv_devices "
                "WHERE connected=1 AND connection_kind='input' AND port=? LIMIT 1",
                (value,),
            ).fetchone()
            device = match[0] if match else None
        if device is not None and (type(device) is not str or not device or "\0" in device):
            raise ValueError("Stored TV device identity is invalid")
        return power, kind, value, device

    def read_vcr_state(self) -> tuple[str | None, str, float]:
        uri = DB_PATH.resolve().as_uri() + "?mode=ro"
        with closing(sqlite3.connect(uri, uri=True, timeout=1.5)) as db:
            db.execute("BEGIN")
            return self._read_vcr_state(db)

    @staticmethod
    def _read_vcr_state(db: sqlite3.Connection) -> tuple[str | None, str, float]:
        row = db.execute(
            "SELECT cassette_path, transport FROM r2_tv_vcr_state WHERE id=1"
        ).fetchone()
        if row is None:
            raise RuntimeError("VCR state row is missing")
        path, transport = row
        if path is not None and (type(path) is not str or not path or
                                 any(c in path for c in "\r\n\0")):
            raise ValueError("Stored VCR cassette identity is invalid")
        if type(transport) is not str or transport not in ("play", "pause", "stop"):
            raise ValueError("Stored VCR transport is invalid")
        if path is None:
            if transport != "stop":
                raise ValueError("Stored VCR transport requires a cassette")
            return None, transport, 0.0
        tape = db.execute(
            "SELECT position_seconds FROM r2_tv_vcr_tapes WHERE media_path=? COLLATE BINARY",
            (path,),
        ).fetchone()
        if tape is None:
            raise ValueError("Saved VCR position row is missing")
        if type(tape[0]) not in (int, float):
            raise ValueError("Saved VCR position is invalid")
        try:
            position = float(tape[0])
        except (ValueError, OverflowError) as exc:
            raise ValueError("Saved VCR position is invalid") from exc
        if not math.isfinite(position) or not 0 <= position <= 604800:
            raise ValueError("Saved VCR position is outside the supported range")
        return path, transport, position

    def read_display_snapshot(self) -> tuple[tuple[int, str, int, str | None],
                                             tuple[str | None, str, float]]:
        uri = DB_PATH.resolve().as_uri() + "?mode=ro"
        with closing(sqlite3.connect(uri, uri=True, timeout=1.5)) as db:
            # One SQLite read transaction: source, tape, and position must agree.
            db.execute("BEGIN")
            return self._read_state(db), self._read_vcr_state(db)

    def _ensure_player(self) -> bool:
        if self._player is not None:
            return True
        instance = None
        player = None
        try:
            import vlc
            instance = vlc.Instance("--no-video-title-show", "--quiet")
            if instance is None:
                raise RuntimeError("VLC could not initialize")
            player = instance.media_player_new()
            if player is None:
                raise RuntimeError("VLC could not create a player")
            self.root.update_idletasks()
            player.set_xwindow(self.video_surface.winfo_id())
        except (ImportError, OSError, RuntimeError, tk.TclError) as exc:
            for resource in (player, instance):
                if resource is not None:
                    try:
                        resource.release()
                    except (OSError, RuntimeError):
                        pass
            if not self._vlc_error_shown:
                self.feedback.configure(
                    text=f"VCR playback needs VLC and python3-vlc: {exc}", fg="#e0b0a0"
                )
                self._vlc_error_shown = True
            return False
        self._vlc, self._vlc_instance, self._player = vlc, instance, player
        self._vlc_error_shown = False
        return True

    def _abandon_vcr_load(self) -> None:
        # A failed or superseded load cannot retain the identity of the old
        # tape or leave a delayed callback applying to replacement media.
        self._pending_vcr_seek = None
        self._loaded_path = None
        self._last_transport = None
        if self._player is not None:
            try:
                self._player.audio_set_mute(True)
                self._player.stop()
            except (OSError, RuntimeError):
                pass

    def _prepare_media(self, path: str, position: float, transport: str,
                       audible: bool) -> None:
        if not self._ensure_player():
            return
        self._pending_vcr_seek = None
        self._loaded_path = None
        self._last_transport = None
        try:
            self._player.audio_set_mute(True)
            media = self._vlc_instance.media_new(path)
            if media is None:
                raise RuntimeError("VLC could not open the selected tape")
            try:
                self._player.set_media(media)
            finally:
                # The player retains its own reference to the selected media.
                media.release()
            if self._player.play() != 0:
                raise RuntimeError("VLC rejected the selected tape")
            self._player.set_pause(1)
        except (OSError, RuntimeError):
            self._abandon_vcr_load()
            raise
        self._loaded_path = path
        seek_request = object()
        self._pending_vcr_seek = seek_request
        seek_target = None
        seek_deadline = time.monotonic() + VCR_SEEK_TIMEOUT_SECONDS

        def seek_and_apply() -> None:
            nonlocal seek_target
            if self._player is None or self._pending_vcr_seek is not seek_request:
                return
            # The transport may have changed while VLC initialized. Re-read
            # Reality at callback time instead of applying stale captured state.
            try:
                tv_state, vcr_state = self.read_display_snapshot()
                power, kind, value, _device = tv_state
                current_path, current_transport, current_position = vcr_state
            except (sqlite3.Error, OSError, RuntimeError, ValueError, OverflowError):
                self._player.audio_set_mute(True)
                self._player.set_pause(1)
                self.root.after(500, seek_and_apply)
                return
            if current_path != path:
                self._abandon_vcr_load()
                return
            try:
                target = int(current_position * 1000)
                if seek_target != target:
                    if not self._player.is_seekable() and target != 0:
                        if time.monotonic() >= seek_deadline:
                            raise RuntimeError("VLC could not seek to the saved tape position")
                        self.root.after(100, seek_and_apply)
                        return
                    # VLC 3 returns None; newer versions can return an error.
                    result = self._player.set_time(target)
                    if result is not None and result < 0:
                        raise RuntimeError("VLC rejected the saved tape position")
                    seek_target = target
                actual = self._player.get_time()
                if actual < 0 or abs(actual - seek_target) > VCR_SEEK_TOLERANCE_MS:
                    if time.monotonic() >= seek_deadline:
                        raise RuntimeError("VLC did not reach the saved tape position")
                    self.root.after(100, seek_and_apply)
                    return
                audible_now = bool(power and kind in ("input", "vcr") and value == 1)
                self._player.set_pause(0 if current_transport == "play" else 1)
                self._player.audio_set_mute(not (audible_now and current_transport == "play"))
                self._last_transport = current_transport
                self._pending_vcr_seek = None
            except (OSError, RuntimeError, ValueError, OverflowError) as exc:
                self._abandon_vcr_load()
                self.feedback.configure(text=f"VCR playback unavailable: {exc}", fg="#e0b0a0")

        self.root.after(500, seek_and_apply)

    def _save_vcr_position(self, force: bool = False) -> None:
        if not self._player or not self._loaded_path or self._last_transport != "play":
            return
        now = time.monotonic()
        if not force and now - self._last_position_save < POSITION_SAVE_SECONDS:
            return
        try:
            milliseconds = self._player.get_time()
            if milliseconds >= 0:
                send_control_command(f"vcr tape-position {milliseconds / 1000:.3f}|{self._loaded_path}")
                self._last_position_save = now
        except (OSError, RuntimeError, ValueError):
            pass

    def sync_vcr_player(self, path: str | None, transport: str, position: float,
                        audible: bool) -> None:
        if path != self._loaded_path:
            self._save_vcr_position(force=True)
            if path is None:
                self._pending_vcr_seek = None
                if self._player:
                    self._player.stop()
                self._loaded_path = None
                self._last_transport = None
            else:
                self._prepare_media(path, position, transport, audible)
        elif (path and self._player and self._pending_vcr_seek is None
              and transport != self._last_transport):
            self._save_vcr_position(force=True)
            self._player.audio_set_mute(True)
            if transport == "play":
                if self._player.play() != 0:
                    self._last_transport = None
                    raise RuntimeError("VLC rejected playback")
            else:
                self._player.set_pause(1)
            self._last_transport = transport
            self._player.audio_set_mute(not (audible and transport == "play"))
        self._save_vcr_position()

    def _send_media_event(self, action: str, path: str) -> bool:
        try:
            send_control_command(f"media-path {action}|{path}")
            return True
        except (OSError, RuntimeError, ValueError):
            # The player must remain usable if the Life Log socket is unavailable.
            return False

    def _publish_renderer(self, path: str | None) -> None:
        try:
            if path and self._loaded_path == path and self._last_transport == "play" \
                    and self._player.get_state() == self._vlc.State.Playing:
                window = self.video_surface.winfo_id()
                if window > 0:
                    self._renderer_published = True
                    send_control_command(f"renderer {window}|{path}")
                    return
            if self._renderer_published:
                send_control_command("renderer 0")
                self._renderer_published = False
        except (AttributeError, OSError, RuntimeError, ValueError):
            pass

    def _record_playback_state(self, path: str | None) -> None:
        previous = self._last_logged_vlc_state
        if previous and previous[1] == "playing" and (self._pending_playback_stop or previous[0] != path):
            self._pending_playback_stop = True
            if not self._send_media_event("vcr_playback_stopped", previous[0]):
                return
            self._last_logged_vlc_state = None
            self._pending_playback_stop = False
            previous = None
        if not path:
            self._last_logged_vlc_state = None
            return
        if self._player is None or self._vlc is None:
            return

        try:
            state = self._player.get_state()
            state_map = {
                self._vlc.State.Playing: ("playing", "vcr_playback_started"),
                self._vlc.State.Paused: ("paused", "vcr_playback_paused"),
                self._vlc.State.Stopped: ("stopped", "vcr_playback_stopped"),
                self._vlc.State.Ended: ("ended", "vcr_playback_ended"),
                self._vlc.State.Error: ("error", "vcr_playback_error"),
            }
            current = state_map.get(state)
        except (AttributeError, OSError, RuntimeError):
            return
        if current is None:
            return

        state_name, action = current
        if previous and previous[0] != path:
            previous = None
            self._last_logged_vlc_state = None

        if previous == (path, state_name):
            return
        if previous is None or previous[0] != path:
            # Establish a baseline for loaded-but-not-playing media. Do not log
            # a false stop/pause event just because a tape was inserted.
            if state_name not in ("playing", "error"):
                self._last_logged_vlc_state = (path, state_name)
                return
        elif state_name != "playing" and previous[1] != "playing" and state_name != "error":
            self._last_logged_vlc_state = (path, state_name)
            return

        if self._send_media_event(action, path):
            self._last_logged_vlc_state = (path, state_name)

    def display_opened(self) -> None:
        try:
            response = send_control_command("display opened")
            self.feedback.configure(text=response, fg="#a6b99c")
        except (OSError, RuntimeError, ValueError) as exc:
            self.feedback.configure(text=f"Could not record TV display opening: {exc}", fg="#e0b0a0")

    def close(self) -> None:
        self._save_vcr_position(force=True)
        self._record_playback_state(None)
        self._publish_renderer(None)
        try:
            send_control_command("display closed")
        except (OSError, RuntimeError, ValueError):
            # Closing the window must still work if R2 has already stopped.
            pass
        self._pending_vcr_seek = None
        player, instance = self._player, self._vlc_instance
        self._player = self._vlc_instance = self._vlc = None
        if player is not None:
            try:
                player.stop()
            except (OSError, RuntimeError):
                pass
        for resource in (player, instance):
            if resource is not None:
                try:
                    resource.release()
                except (OSError, RuntimeError):
                    pass
        self.root.destroy()

    def refresh(self, schedule: bool = True) -> None:
        try:
            tv_state, vcr_state = self.read_display_snapshot()
            power, kind, value, device = tv_state
            tape_path, transport, position = vcr_state
            audible = bool(power and kind in ("input", "vcr") and value == 1)
            if self._player and not (audible and tape_path == self._loaded_path
                                     and transport == self._last_transport == "play"
                                     and self._pending_vcr_seek is None):
                self._player.audio_set_mute(True)
            self.sync_vcr_player(tape_path, transport, position, audible)
            if self._pending_vcr_seek is None:
                self._record_playback_state(self._loaded_path)
            elif self._last_logged_vlc_state and self._last_logged_vlc_state[0] != self._loaded_path:
                self._record_playback_state(None)

            ready_to_play = bool(tape_path and tape_path == self._loaded_path
                                 and transport == self._last_transport == "play"
                                 and self._pending_vcr_seek is None
                                 and self._player is not None
                                 and self._vlc is not None
                                 and self._player.get_state() == self._vlc.State.Playing)
            if not power:
                headline = "TV OFF"
                source = "Standby"
            elif kind == "rf":
                source = f"RF CHANNEL {value}"
                headline = device or "NO SIGNAL"
            elif value == 1:
                source = "AV INPUT 1 — BUILT-IN VCR"
                if not tape_path:
                    headline = "VCR EMPTY\nNO TAPE INSERTED"
                elif transport == "play":
                    headline = ("" if ready_to_play else
                                "VCR LOADING" if self._player is not None else
                                "VLC PLAYBACK UNAVAILABLE")
                elif transport == "pause":
                    headline = "VCR PAUSED"
                else:
                    headline = "VCR STOPPED"
            else:
                source = f"AV INPUT {value}"
                headline = device or "NO SIGNAL"

            showing_vcr = audible and ready_to_play
            if self._player:
                self._player.audio_set_mute(not showing_vcr)
            if showing_vcr:
                self.video_surface.lift()
            else:
                self.picture.lift()
                self.picture.configure(
                    text=headline,
                    fg="#a6b99c" if power and device else "#b8c0b2",
                    bg="#080b08" if power else "#050605",
                )

            self._publish_renderer(tape_path if showing_vcr else None)
            tape_name = Path(tape_path).name if tape_path else "no tape"
            self.status.configure(
                text=f"{'POWER ON' if power else 'POWER OFF'}   |   {source}"
                     + (f"   |   {device}" if device and value != 1 else "")
                     + f"   |   VCR: {tape_name} [{transport}]"
                     + "   |   Reality-linked controls"
            )
        except (sqlite3.Error, OSError, RuntimeError, ValueError, OverflowError) as exc:
            # With no authoritative TV state, hold the existing media silent.
            # Reapply the authoritative transport when reads recover.
            if self._player:
                self._last_transport = None
                try:
                    self._player.audio_set_mute(True)
                    self._player.set_pause(1)
                except (OSError, RuntimeError):
                    pass
            self._publish_renderer(None)
            self.picture.lift()
            self.picture.configure(text="TV STATE UNAVAILABLE", fg="#b8c0b2")
            self.status.configure(text=f"Database: {DB_PATH}   |   {exc}")
        if schedule:
            self.root.after(POLL_MS, self.refresh)


def main() -> None:
    root = tk.Tk()
    CRTDisplay(root)
    root.mainloop()


if __name__ == "__main__":
    main()
