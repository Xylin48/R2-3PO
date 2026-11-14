#!/usr/bin/env python3
"""Exercise real R2 typed conversation twice with offline, scripted Ollama replies.

Runs the production C core and its persistent SQLite stores. Only local model
inference is substituted by a deterministic loopback HTTP server; no peripheral,
download, desktop session, or installed model is required.
"""
from __future__ import annotations

import json
import os
import sqlite3
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from r2_service_signal_integration import REPO, SOURCES, LIBS

FIRST = "R2_OFFLINE_SESSION_ONE_73628"
SECOND = "R2_OFFLINE_SESSION_TWO_94716"
REPLY = "I received your message and can respond in this conversation."


class LocalOllama(BaseHTTPRequestHandler):
    requests: list[dict] = []
    lock = threading.Lock()

    def do_POST(self) -> None:
        if self.path != "/api/chat":
            self.send_error(404)
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if length <= 0 or length > 10 * 1024 * 1024:
                self.send_error(413)
                return
            payload = json.loads(self.rfile.read(length))
        except (ValueError, json.JSONDecodeError):
            self.send_error(400)
            return
        with self.lock:
            self.requests.append(payload)
        body = json.dumps({
            "model": "qwen3.5:9b", "done": True, "done_reason": "stop",
            "message": {"role": "assistant", "content": REPLY},
        }).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_args: object) -> None:
        pass


def serve() -> tuple[ThreadingHTTPServer, threading.Thread]:
    for port in (11435, 11434):
        try:
            server = ThreadingHTTPServer(("127.0.0.1", port), LocalOllama)
            break
        except OSError:
            continue
    else:
        raise AssertionError("Neither permitted loopback Ollama test port was available")
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server, thread


def compile_core(base: Path) -> Path:
    workspace = base / "workspace"
    home = workspace / "R2"
    binary = base / "r2-offline-conversation"
    workspace.mkdir()
    macros = [
        f'-DR2_ROOT="{workspace}"',
        f'-DR2_HOME="{home}"',
        f'-DR2_WORKSPACE="{workspace}"',
        f'-DR2_DIARY_DATABASE="{home / "r2_memory.db"}"',
        f'-DR2_DIARY_DIR="{workspace / "R2_Diary"}"',
    ]
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
         "-I", str(REPO), *macros,
         *(str(REPO / source) for source in SOURCES),
         "-o", str(binary), *LIBS],
        cwd=REPO, check=True, timeout=120,
    )
    return binary


def conversation(binary: Path, workspace: Path, url: str, marker: str) -> str:
    env = os.environ.copy()
    env.pop("R2_SERVICE_MODE", None)
    env["R2_OLLAMA_BASE_URL"] = url
    env["R2_OLLAMA_TIMEOUT_SECONDS"] = "15"
    env["R2_SOUNDS_DISABLE_PLAYBACK"] = "1"
    env["R2_FX_DIR"] = str(workspace / "unused-FX")
    env["R2_TV_SOCKET"] = str(workspace / "R2" / "tv-offline-test.sock")
    env["DISPLAY"] = ""
    output = subprocess.run(
        [str(binary)], cwd=workspace, env=env,
        input=f"talk {marker}\nexit\n",
        text=True, capture_output=True, timeout=100,
    )
    combined = output.stdout + "\n" + output.stderr
    if output.returncode or REPLY not in combined or "[R2-3PO shut down.]" not in combined:
        raise AssertionError(
            f"Typed conversation failed for {marker}: exit={output.returncode}\n"
            + combined[-16000:]
        )
    return combined


def main() -> int:
    LocalOllama.requests = []
    with tempfile.TemporaryDirectory(prefix="r2-offline-chat-") as tmp:
        base = Path(tmp)
        binary = compile_core(base)
        workspace = base / "workspace"
        server, thread = serve()
        try:
            url = f"http://127.0.0.1:{server.server_port}"
            conversation(binary, workspace, url, FIRST)
            conversation(binary, workspace, url, SECOND)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
        requests = LocalOllama.requests
        if len(requests) < 2:
            raise AssertionError(f"Expected real chat HTTP requests in both sessions; got {len(requests)}")
        for marker in (FIRST, SECOND):
            if not any(marker in json.dumps(request, ensure_ascii=False) for request in requests):
                raise AssertionError(f"Active user turn {marker} never reached the Ollama request")
        memory_db = workspace / "R2" / "r2_memory.db"
        with sqlite3.connect(f"file:{memory_db}?mode=ro", uri=True) as db:
            rows = db.execute(
                "SELECT user_text,assistant_text FROM r2_log_conversations "
                "WHERE user_text IN (?,?) ORDER BY event_id", (FIRST, SECOND),
            ).fetchall()
            if len(rows) != 2 or [row[0] for row in rows] != [FIRST, SECOND]:
                raise AssertionError(f"Typed conversation history lost after restart: {rows!r}")
            if any(not row[1].strip() for row in rows):
                raise AssertionError("Persisted conversation contains an empty assistant turn")
    print("PASS: production core completed and retained two real typed turns across restart without peripherals.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
