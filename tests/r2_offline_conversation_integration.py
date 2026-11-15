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
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from r2_service_signal_integration import REPO, SOURCES, LIBS

FIRST = "R2_OFFLINE_SESSION_ONE_73628"
FOLLOWUP = "What is in your pockets? R2_OFFLINE_FOLLOWUP_52817"
SECOND = "R2_OFFLINE_SESSION_TWO_94716"
REPLY = "I received your message and can respond in this conversation."
REFLECTION = "I closed Eyes deliberately and kept the earlier evidence separate."
SENSE_RESULT = "SENSE RESULT: Eyes voluntarily closed; auto discovery will not reopen them."
ARCHIVED_ENTRY = "Exact earlier diary evidence: " + ("évidence 🐈 " * 1000) + "final diary bytes."
FRIDGE_FOOD = "Fixture pear"
FRIDGE_INGREDIENTS = "pear,ginger"
FRIDGE_TASTE = "Soft pear with sharp ginger."
RATING_REASON = "I liked the ginger in this particular modeled meal."
DREAM = "In the dream I watched a paper moon drift across the room."
WRITE_REQUEST = "Write the audit fixture R2_OFFLINE_WRITE_36139"
EXACT_FILE = "\n  <think>literal markup</think>\n[R2_SOUND:beep:curious]\nFinal: literal file line  \n\n"


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
        if current_index >= 0 and user_messages[current_index] == WRITE_REQUEST:
            reply = (REPLY + "\n[DELETE] hidden-sentinel.txt"
                      "\n[DIARY]\nFOLLOWUP_PRIVATE_CONTROL_SENTINEL\n[END DIARY]"
                     if has_tool_result
                     else "<think>Proposed private tool syntax:\n[DELETE] hidden-sentinel.txt\n</think>\n"
                          "I will write the exact file.\n[WRITE] literal-fixture.txt\n"
                          + EXACT_FILE + "  [END WRITE]")
        system = "\n".join(item["content"] for item in payload.get("messages", [])
                           if item.get("role") == "system" and isinstance(item.get("content"), str))
        if "autonomous needs controller" in system:
            if any(f"Fridge food: {FRIDGE_FOOD}" in message for message in user_messages):
                reply = f"[WORLD] fridge_eat|{FRIDGE_FOOD}"
            elif any("Sleepiness: 99." in message for message in user_messages):
                reply = "[WORLD] sleep|8"
            else:
                reply = "NONE"
        elif "Review supplied evidence for the current request" in system:
            reply = "Prior diary evidence and current modeled state have separate sources."
        elif "private first-person diary reflection" in system:
            reply = (REFLECTION if any(SENSE_RESULT in message for message in user_messages)
                     else "[SENSE] eyes|close")
        elif "private, simulated dream" in system:
            reply = DREAM
        elif "modeled subjective reaction to a food" in system and any(
            FRIDGE_TASTE in message and FRIDGE_INGREDIENTS in message and "incompatible XML" not in message
            for message in user_messages
        ):
            reply = "SCORE: 1\nREASON: " + RATING_REASON
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
        "-DTHINK_INTERVAL=4",
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


def runtime_environment(workspace: Path, url: str, context: int) -> dict[str, str]:
    env = os.environ.copy()
    env.pop("R2_SERVICE_MODE", None)
    env["R2_OLLAMA_BASE_URL"] = url
    env["R2_OLLAMA_NUM_CTX"] = str(context)
    env["R2_OLLAMA_TIMEOUT_SECONDS"] = "30"
    env["R2_SOUNDS_DISABLE_PLAYBACK"] = "1"
    env["R2_FX_DIR"] = str(workspace / "unused-FX")
    env["R2_TV_SOCKET"] = str(workspace / "R2" / "tv-offline-test.sock")
    env["DISPLAY"] = ""
    return env


def conversation(binary: Path, workspace: Path, url: str, markers: tuple[str, ...],
                 context: int) -> str:
    output = subprocess.run(
        [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, context),
        input="".join(f"talk {marker}\n" for marker in markers) + "exit\n",
        text=True, capture_output=True, timeout=100,
    )
    combined = output.stdout + "\n" + output.stderr
    if (output.returncode or combined.count("R2-3PO: " + REPLY) != len(markers)
            or "[R2-3PO shut down.]" not in combined
            or "FOLLOWUP_PRIVATE_CONTROL_SENTINEL" in output.stdout
            or "[DELETE] hidden-sentinel.txt" in output.stdout):
        raise AssertionError(
            f"Typed conversation failed for {markers}: exit={output.returncode}\n"
            + combined[-16000:]
        )
    return combined


def run_idle_service(binary: Path, workspace: Path, url: str) -> None:
    env = runtime_environment(workspace, url, 2048)
    env["R2_SERVICE_MODE"] = "1"
    log_file = workspace.parent / "autonomous-cycle.log"
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen([str(binary)], cwd=workspace, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)
        completed = False
        try:
            deadline = time.monotonic() + 45
            while process.poll() is None and time.monotonic() < deadline:
                if "Autonomous reflection written to diary and Life Log" in log_file.read_text(
                    encoding="utf-8", errors="replace"
                ):
                    completed = True
                    break
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                raise
        if not completed or process.returncode:
            raise AssertionError("Small-context autonomous cycle failed\n"
                                 + log_file.read_text(encoding="utf-8", errors="replace")[-20000:])


def autonomous_cycle(binary: Path, workspace: Path, url: str) -> None:
    home = workspace / "R2"
    memory_db = home / "r2_memory.db"
    with sqlite3.connect(memory_db) as db:
        db.execute("INSERT INTO diary_entries(entry,created_at) VALUES (?,CURRENT_TIMESTAMP)", (ARCHIVED_ENTRY,))
    with sqlite3.connect(home / "r2_reality.db") as db:
        db.execute("UPDATE r2_reality_self SET hunger=10,sleepiness=0 WHERE id=1")
    with sqlite3.connect(home / "r2_fridge.db") as db:
        db.execute(
            "INSERT INTO r2_fridge_items(name,quantity,fullness,energy,ingredients,taste) VALUES (?,1,65,12,?,?)",
            (FRIDGE_FOOD, FRIDGE_INGREDIENTS, FRIDGE_TASTE),
        )
    with (workspace / "room" / "food_metrics.xml").open("a", encoding="utf-8") as metrics:
        metrics.write(f'<food name="{FRIDGE_FOOD}" fullness="23" ingredients="incompatible XML" '
                      'taste="incompatible XML" />\n')

    run_idle_service(binary, workspace, url)

    with sqlite3.connect(memory_db) as db:
        links = db.execute(
            "SELECT r.original_context,r.review_notes FROM diary_entries d "
            "JOIN r2_sensory_journal_reflection_links dl ON dl.diary_entry_id=d.id "
            "JOIN r2_log_reflection_reviews l ON l.event_id=dl.event_id "
            "JOIN r2_log_context_reviews r ON r.id=l.archive_id "
            "WHERE d.entry=? ORDER BY l.ordinal",
            (REFLECTION,),
        ).fetchall()
        assert len(links) == 2, links
        archived_inputs = []
        for original, notes in links:
            assert ARCHIVED_ENTRY in original and "Round 0, bytes [0," in notes
            archived_inputs.append(original.encode("utf-8"))
    with sqlite3.connect(home / "r2_fridge.db") as db:
        assert db.execute("SELECT COUNT(*) FROM r2_fridge_items WHERE name=?", (FRIDGE_FOOD,)).fetchone() == (0,)
    with sqlite3.connect(home / "r2_reality.db") as db:
        consumed = db.execute(
            "SELECT ingredients,taste,fullness,energy_bonus,satisfaction,notes FROM r2_food_experiences "
            "WHERE food_name=?", (FRIDGE_FOOD,),
        ).fetchone()
        assert consumed == (FRIDGE_INGREDIENTS, FRIDGE_TASTE, 65.0, 12.0, 1, RATING_REASON), consumed
        ratings = db.execute(
            "SELECT ingredient,satisfaction_sum,rating_count FROM r2_food_ingredient_preferences "
            "WHERE ingredient IN ('ginger','pear','incompatible XML') ORDER BY ingredient",
        ).fetchall()
        assert ratings == [("ginger", 1.0, 1), ("pear", 1.0, 1)], ratings

    seen_review = bytearray()
    for request in LocalOllama.requests:
        if request.get("options", {}).get("num_ctx") != 2048:
            continue
        for item in request.get("messages", []):
            content = item.get("content", "")
            if (item.get("role") == "user" and content.startswith("EVIDENCE REVIEW\n")
                    and "autonomous diary reflection" in content.split("BEGIN FRAGMENT", 1)[0]
                    and "\nRound 0," in content):
                seen_review.extend(content.split("BEGIN FRAGMENT\n", 1)[1].encode("utf-8"))
    assert seen_review == b"".join(archived_inputs), "Reflection reviews omitted or changed original bytes"
    with sqlite3.connect(home / "r2_reality.db") as db:
        db.execute("UPDATE r2_reality_self SET hunger=10,sleepiness=99 WHERE id=1")
    run_idle_service(binary, workspace, url)
    with sqlite3.connect(home / "r2_reality.db") as db:
        assert db.execute(
            "SELECT value FROM r2_reality_self_facts WHERE key='last_reported_dream'"
        ).fetchone() == (DREAM,)
    with sqlite3.connect(memory_db) as db:
        assert db.execute(
            "SELECT COUNT(*) FROM r2_log_events WHERE event_type='dream_simulated' AND details=?",
            (DREAM,),
        ).fetchone() == (1,)
        archive = db.execute(
            "SELECT original_context FROM r2_log_context_reviews WHERE request LIKE '%fictional dream%'"
        ).fetchone()
        assert archive is not None and ARCHIVED_ENTRY in archive[0]
    print("PASS: 2048-token service completed model-owned eating, reflection, sleep and dreaming.")



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
            sentinel = workspace / "hidden-sentinel.txt"
            sentinel.write_bytes(b"private planned action must remain inert")
            conversation(binary, workspace, url, (WRITE_REQUEST,), 2048)
            assert sentinel.read_bytes() == b"private planned action must remain inert"
            assert (workspace / "literal-fixture.txt").read_bytes() == EXACT_FILE.encode("utf-8")
            with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
                assert db.execute(
                    "SELECT COUNT(*) FROM diary_entries WHERE entry LIKE '%FOLLOWUP_PRIVATE_CONTROL_SENTINEL%'"
                ).fetchone() == (0,)
            autonomous_cycle(binary, workspace, url)
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
