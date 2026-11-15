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
FOLLOWUP = "What is in your pockets? R2_OFFLINE_FOLLOWUP_52817"
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
        user_messages = [item["content"] for item in payload.get("messages", [])
                         if item.get("role") == "user" and isinstance(item.get("content"), str)]
        result_prefix = "[R2_INTERNAL_TOOL_RESULT]\n"
        current_index = next((i for i in range(len(user_messages) - 1, -1, -1)
                              if not user_messages[i].startswith(result_prefix)), -1)
        asks_for_pockets = current_index >= 0 and user_messages[current_index].startswith(FOLLOWUP + " CTX_")
        has_tool_result = any(message.startswith(result_prefix)
                              for message in user_messages[current_index + 1:])
        reply = ("I can look through my inventory.\n[WORLD] look"
                 if asks_for_pockets and not has_tool_result else REPLY)
        body = json.dumps({
            "model": "qwen3.5:9b", "done": True, "done_reason": "stop",
            "message": {"role": "assistant", "content": reply},
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
    # Opt in to both sanitizers for the same real compiled-core integration
    # path. The regular run remains unchanged to preserve baseline coverage.
    sanitizer_flags = (
        ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        if os.environ.get("R2_TEST_SANITIZERS") == "1" else []
    )
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
         *sanitizer_flags, "-I", str(REPO), *macros,
         *(str(REPO / source) for source in SOURCES),
         "-o", str(binary), *LIBS],
        cwd=REPO, check=True, timeout=120,
    )
    return binary


def conversation(binary: Path, workspace: Path, url: str, markers: tuple[str, ...],
                 context: int) -> str:
    env = os.environ.copy()
    env.pop("R2_SERVICE_MODE", None)
    env["R2_OLLAMA_BASE_URL"] = url
    env["R2_OLLAMA_NUM_CTX"] = str(context)
    env["R2_OLLAMA_TIMEOUT_SECONDS"] = "30"
    env["R2_SOUNDS_DISABLE_PLAYBACK"] = "1"
    env["R2_FX_DIR"] = str(workspace / "unused-FX")
    env["R2_TV_SOCKET"] = str(workspace / "R2" / "tv-offline-test.sock")
    env["DISPLAY"] = ""
    output = subprocess.run(
        [str(binary)], cwd=workspace, env=env,
        input="".join(f"talk {marker}\n" for marker in markers) + "exit\n",
        text=True, capture_output=True, timeout=100,
    )
    combined = output.stdout + "\n" + output.stderr
    if (output.returncode or combined.count("R2-3PO: " + REPLY) != len(markers)
            or "[R2-3PO shut down.]" not in combined):
        raise AssertionError(
            f"Typed conversation failed for {markers}: exit={output.returncode}\n"
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
        contexts = (2048, 4096, 8192, 16384)
        selected_context = os.environ.get("R2_OLLAMA_NUM_CTX")
        if selected_context:
            contexts = (int(selected_context),)
        turns: list[tuple[int, str]] = []
        try:
            url = f"http://127.0.0.1:{server.server_port}"
            for context in contexts:
                first, followup, second = (
                    f"{marker} CTX_{context}" for marker in (FIRST, FOLLOWUP, SECOND)
                )
                conversation(binary, workspace, url, (first, followup), context)
                conversation(binary, workspace, url, (second,), context)
                turns.extend((context, marker) for marker in (first, followup, second))
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
        for context, marker in turns:
            requests = [
                request for request in LocalOllama.requests
                if request.get("options", {}).get("num_ctx") == context
                and any(isinstance(item, dict) and item.get("role") == "user"
                        and item.get("content") == marker for item in request.get("messages", []))
            ]
            if not requests:
                raise AssertionError(f"Exact turn {marker} never reached an answer request at {context}")
            if marker.startswith(FOLLOWUP) and not any(
                item.get("role") == "system" and item["content"].startswith("PERSISTENT REALITY CONTEXT")
                and "POCKETS" in item["content"].upper()
                for request in requests for item in request.get("messages", [])
                if isinstance(item, dict) and isinstance(item.get("content"), str)
            ):
                raise AssertionError(f"Pocket inventory view was missing in the answer request at {context}")
            if marker.startswith(FOLLOWUP) and not any(
                item.get("role") == "user" and item["content"].startswith("[R2_INTERNAL_TOOL_RESULT]\n")
                and "REAL WORLD RESULT" in item["content"]
                for request in requests for item in request.get("messages", [])
                if isinstance(item, dict) and isinstance(item.get("content"), str)
            ):
                raise AssertionError(f"Exact action outcome was missing from the follow-up at {context}")

        memory_db = workspace / "R2" / "r2_memory.db"
        expected = [marker for _, marker in turns]
        placeholders = ",".join("?" for _ in expected)
        with sqlite3.connect(f"file:{memory_db}?mode=ro", uri=True) as db:
            rows = db.execute(
                "SELECT user_text,assistant_text FROM r2_log_conversations "
                f"WHERE user_text IN ({placeholders}) ORDER BY event_id",
                expected,
            ).fetchall()
            if [row[0] for row in rows] != expected:
                raise AssertionError(f"Typed conversation history lost after restart: {rows!r}")
            if any(row[1] != REPLY for row in rows):
                raise AssertionError("Persisted assistant reply differs from the generated answer")
    print(f"PASS: production core retained {len(turns)} exact typed turns and replies "
          f"across peripheral-free restarts at contexts {contexts}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
