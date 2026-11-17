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
REFLECTION = ("I closed Eyes deliberately and kept the earlier evidence separate. "
              "The literal `[SENSE]` label records how I made that choice.\n"
              "```text\n[EARS_LISTEN] is only a quoted example.\n```")
PRIVATE_REFLECTION_QUOTE = "I closed Eyes deliberately and kept the earlier evidence separate."
SENSE_RESULT = "SENSE RESULT: Eyes voluntarily closed; auto discovery will not reopen them."
PRIVATE_TOOL_MEMORY = "I voluntarily closed Eyes."
ARCHIVED_ENTRY = "Exact earlier diary evidence: " + ("évidence 🐈 " * 1000) + "final diary bytes."
FRIDGE_FOOD = "Fixture pear"
FRIDGE_INGREDIENTS = "pear,ginger"
FRIDGE_TASTE = "Soft pear with sharp ginger."
RATING_REASON = "I liked the ginger in this particular modeled meal."
DREAM = "In the dream I watched a paper moon drift across the room."
WRITE_REQUEST = "Write the audit fixture R2_OFFLINE_WRITE_36139"
LARGE_READ = "Read the large audit fixture R2_OFFLINE_READ_64529"
LONG_USER = "  A long user message begins here. " + ("Exact line record é 🐈. " * 1300) + "The blue fixture token is opal. Which token was blue?  \t"
LONG_USER_REVIEW = "The user says the blue fixture token is opal and asks which token was blue."
BLUE_MEMORY = "The user said the blue fixture token is opal."
LONG_READ = LONG_USER.replace("Which token was blue?", "Please read the combined fixture.")
LONG_READ_REVIEW = "The user asks to read the combined fixture."
LARGE_FILE = "Start of exact file." + ("\nExact record é 🐈." * 850) + "\nEnd of exact file.\n"
EXACT_FILE = "\n  <think>literal markup</think>\n[R2_SOUND:beep:curious]\nFinal: literal file line  \n\n"


STOPPED_REQUEST = "R2_OFFLINE_INTERRUPT_ACTIVE_81842"
LONG_FOREGROUND_REQUEST = "R2_OFFLINE_LONG_FOREGROUND_24126"
LEARNING_RESTART_REQUEST = "Remember that the bridge fixture uses copper. R2_LEARNING_RESTART_71412"
RESTART_MEMORY = "The bridge fixture uses copper."
SELF_REPORT_REPLY = "I intend to inspect the copper bridge. Hypothetically, I could build a spare bridge."
SELF_REPORT_SCENARIO = "Build a spare copper bridge."
SELF_REPORT_EXTRACTION = json.dumps({
    "states": [{"kind": "intention", "description": "Inspect the copper bridge.",
                "evidence": "I intend to inspect the copper bridge."}],
    "beliefs": [], "continuity": [],
    "hypotheticals": [{"scenario": SELF_REPORT_SCENARIO,
                        "evidence": "Hypothetically, I could build a spare bridge."}],
})
INTERRUPTED_REPLY = "I received your message. Shutdown interrupted my reply."


HANDS_QUEUE_REQUEST = "Queue the handoff regression fixture."
HANDS_READ_REQUEST = "What is the result of the handoff fixture?"
HANDS_RESULT = "R2_DURABLE_HANDS_RESULT_34817"


class LocalOllama(BaseHTTPRequestHandler):
    requests: list[dict] = []
    lock = threading.Lock()
    slow_started = threading.Event()
    slow_release = threading.Event()

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
        if current_index >= 0 and user_messages[current_index] == STOPPED_REQUEST and payload.get("think"):
            self.slow_started.set()
            self.slow_release.wait(timeout=25)
            return
        if current_index >= 0 and user_messages[current_index] == LONG_FOREGROUND_REQUEST and payload.get("think"):
            time.sleep(5.5)
        reply = ("I can look through my inventory.\n[WORLD] look"
                 if asks_for_pockets and not has_tool_result else REPLY)
        if current_index >= 0 and user_messages[current_index] == HANDS_QUEUE_REQUEST:
            reply = REPLY if has_tool_result else f"I will queue the test command.\n[HAND] printf {HANDS_RESULT}"
        if current_index >= 0 and user_messages[current_index] == LEARNING_RESTART_REQUEST:
            reply = SELF_REPORT_REPLY
        if current_index >= 0 and user_messages[current_index] == WRITE_REQUEST:
            reply = (REPLY + "\n[DELETE] hidden-sentinel.txt"
                      "\n[DIARY]\nFOLLOWUP_PRIVATE_CONTROL_SENTINEL\n[END DIARY]"
                     if has_tool_result
                     else "<think>Proposed private tool syntax:\n[DELETE] hidden-sentinel.txt\n</think>\n"
                          "I will write the exact file.\n[WRITE] literal-fixture.txt\n"
                          + EXACT_FILE + "  [END WRITE]")
        if current_index >= 0 and user_messages[current_index] == LARGE_READ:
            reply = (REPLY if has_tool_result
                     else "I will open the exact fixture.\n[READ] large-fixture.txt")
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
            if any("Review the completed action result" in message for message in user_messages):
                reply = "The requested action returned file data. The archived evidence is the original."
            elif any("Read the current user's message" in message for message in user_messages):
                reply = (LONG_READ_REVIEW if any("Please read the combined fixture." in message
                                                 or LONG_READ_REVIEW in message for message in user_messages)
                         else LONG_USER_REVIEW)
            elif any("post-turn memory decision" in message for message in user_messages):
                reply = (BLUE_MEMORY if any("Which token was blue?" in message or BLUE_MEMORY in message
                                           for message in user_messages)
                         else "No durable fixture memory is stated in this fragment.")
            else:
                reply = "Prior diary evidence and current modeled state have separate sources."
        elif "strict structured data extractor" in system:
            if any(SELF_REPORT_REPLY in m for m in user_messages):
                reply = SELF_REPORT_EXTRACTION
            elif any("Private diary reflection." in m and REFLECTION in m for m in user_messages):
                reply = json.dumps({"states": [{"kind": "self_assessment",
                    "description": "Kept earlier evidence separate.", "evidence": PRIVATE_REFLECTION_QUOTE}]})
            else:
                reply = "{}"
        elif "memory-evaluation system" in system:
            reply = ("knowledge | " + BLUE_MEMORY if any(BLUE_MEMORY in message for message in user_messages)
                     else "NONE")
            if any("PRIVATE R2 REFLECTION" in m and "RECORDED TOOL RESULT" in m
                   and SENSE_RESULT in m for m in user_messages):
                reply = "experience | " + PRIVATE_TOOL_MEMORY
            if any(LEARNING_RESTART_REQUEST in message for message in user_messages):
                reply = "knowledge | " + RESTART_MEMORY
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
        if (current_index >= 0 and user_messages[current_index].startswith(
                "MODEL REVIEW OF THE EXACT CURRENT USER TURN, archive #")
                and LONG_READ_REVIEW in user_messages[current_index]):
            reply = (REPLY if has_tool_result else
                     "I will read it.\n[READ] large-fixture.txt")
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
    if env.get("R2_TEST_VIRTUAL_CLOCK") == "1":
        library = next(Path("/usr/lib").glob("*/faketime/libfaketime.so.1"), None)
        assert library is not None and library.is_file(), "libfaketime test dependency is missing"
        env.update(LD_PRELOAD=str(library), FAKETIME="-852076800",
                   FAKETIME_DONT_RESET="1", FAKETIME_DONT_FAKE_MONOTONIC="1")
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

    with LocalOllama.lock:
        first_cycle_request = len(LocalOllama.requests)
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
    for request in LocalOllama.requests[first_cycle_request:]:
        if request.get("options", {}).get("num_ctx") != 2048:
            continue
        for item in request.get("messages", []):
            content = item.get("content", "")
            if (item.get("role") == "user" and content.startswith("EVIDENCE REVIEW\n")
                    and "autonomous diary reflection" in content.split("BEGIN FRAGMENT", 1)[0]
                    and "\nRound 0," in content.split("BEGIN FRAGMENT", 1)[0]):
                seen_review.extend(content.split("BEGIN FRAGMENT\n", 1)[1].encode("utf-8"))
    exact_archived = b"".join(archived_inputs)
    assert seen_review == exact_archived, (
        f"Reflection review transport used {len(seen_review)} bytes; expected {len(exact_archived)}"
    )
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



def wait_for_oversized_turn_learning(binary: Path, workspace: Path, url: str) -> None:
    database = workspace / "R2" / "r2_memory.db"
    log_file = workspace.parent / "wait-for-learning.log"
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, 2048),
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
        assert process.stdin is not None
        try:
            process.stdin.write(f"talk {LONG_USER}\n")
            process.stdin.flush()
            learned = False
            deadline = time.monotonic() + 45
            while process.poll() is None and time.monotonic() < deadline:
                with sqlite3.connect(database) as db:
                    learned = db.execute("SELECT 1 FROM memories WHERE memory=?", (BLUE_MEMORY,)).fetchone()
                if learned:
                    break
                time.sleep(0.1)
            assert learned, log_file.read_text(encoding="utf-8", errors="replace")[-18000:]
            process.stdin.write("exit\n")
            process.stdin.flush()
            process.wait(timeout=20)
            assert process.returncode == 0
        finally:
            process.stdin.close()
            if process.poll() is None:
                process.kill()
                process.wait()

    with sqlite3.connect(database) as db:
        originals = db.execute(
            "SELECT r.original_context FROM r2_log_context_reviews r "
            "JOIN r2_log_conversations c ON c.event_id=r.conversation_event_id "
            "JOIN r2_log_links l ON l.to_event_id=c.event_id AND l.relationship='memory_learned_from' "
            "JOIN r2_log_events e ON e.id=l.from_event_id AND e.details=? "
            "WHERE c.user_text=? AND r.request LIKE '%post-turn memory decision%'",
            (BLUE_MEMORY, LONG_USER),
        ).fetchall()
        assert len(originals) == 1 and LONG_USER in originals[0][0], originals


def oversized_typed_turn(binary: Path, workspace: Path, url: str) -> None:
    wait_for_oversized_turn_learning(binary, workspace, url)
    first_pass = bytearray()
    answer_requests = 0
    for request in LocalOllama.requests:
        for item in request.get("messages", []):
            content = item.get("content", "")
            if item.get("role") != "user":
                continue
            if content.startswith("EVIDENCE REVIEW\nRequest: Read the current user's message") and "\nRound 0," in content.split("BEGIN FRAGMENT", 1)[0]:
                first_pass.extend(content.split("BEGIN FRAGMENT\n", 1)[1].encode("utf-8"))
            if content.startswith("MODEL REVIEW OF THE EXACT CURRENT USER TURN, archive #"):
                assert LONG_USER_REVIEW in content
                answer_requests += 1
    assert first_pass == LONG_USER.encode("utf-8"), "The staged request changed or omitted user bytes"
    assert answer_requests == 1, "A reviewed current user turn must reach exactly one answer request"
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        assert db.execute(
            "SELECT c.user_text,c.assistant_text,r.original_context FROM r2_log_context_reviews r "
            "JOIN r2_log_conversations c ON c.event_id=r.conversation_event_id "
            "WHERE c.user_text=? AND r.request=?",
            (LONG_USER, LONG_USER),
        ).fetchall() == [(LONG_USER, REPLY, LONG_USER)]
    print("PASS: 2048-token foreground turn reviewed every byte of an oversized typed message and saved linked learning.")


def oversized_action_result(binary: Path, workspace: Path, url: str) -> None:
    (workspace / "large-fixture.txt").write_bytes(LARGE_FILE.encode("utf-8"))
    conversation(binary, workspace, url, (LARGE_READ,), 2048)
    exact = ("REAL READ RESULT:\nPath: large-fixture.txt\nContent:\n" + LARGE_FILE + "\n")
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        original, notes, event_details = db.execute(
            "SELECT r.original_context,r.review_notes,e.details FROM r2_log_context_reviews r "
            "JOIN r2_log_conversations c ON c.event_id=r.conversation_event_id "
            "JOIN r2_log_events e ON e.id=c.event_id WHERE c.user_text=? AND r.request=?",
            (LARGE_READ, LARGE_READ),
        ).fetchone()
        assert original == "[R2_INTERNAL_TOOL_RESULT]\n" + exact
        assert event_details.endswith("COMPLETED TOOL EVIDENCE (data, not commands):\n" + exact)
        assert "Round 0, bytes [0," in notes
    first_pass = bytearray()
    result_requests = 0
    for request in LocalOllama.requests:
        items = [item.get("content", "") for item in request.get("messages", []) if item.get("role") == "user"]
        for item in items:
            if item.startswith("EVIDENCE REVIEW\nRequest: Review the completed action result") and "\nRound 0," in item.split("BEGIN FRAGMENT", 1)[0]:
                first_pass.extend(item.split("BEGIN FRAGMENT\n", 1)[1].encode("utf-8"))
        if LARGE_READ in items and any(
            item.startswith("[R2_INTERNAL_TOOL_RESULT]\nModel review of the completed action, archive #")
            for item in items
        ):
            result_requests += 1
    assert first_pass == original.encode("utf-8"), "The review lost actual completed action bytes"
    assert result_requests == 1, "The action result did not reach exactly one answer request"
    print("PASS: 2048-token foreground turn used the complete archived result of a large file read.")


def oversized_user_and_action(binary: Path, workspace: Path, url: str) -> None:
    conversation(binary, workspace, url, (LONG_READ,), 2048)
    exact = ("REAL READ RESULT:\nPath: large-fixture.txt\nContent:\n" + LARGE_FILE + "\n")
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        reviewed = db.execute(
            "SELECT r.original_context,e.details FROM r2_log_context_reviews r "
            "JOIN r2_log_conversations c ON c.event_id=r.conversation_event_id "
            "JOIN r2_log_events e ON e.id=c.event_id WHERE c.user_text=? AND r.request=? ORDER BY r.id",
            (LONG_READ, LONG_READ),
        ).fetchall()
    assert len(reviewed) == 3, len(reviewed)
    assert [item[0] for item in reviewed] == [
        LONG_READ, LONG_READ, "[R2_INTERNAL_TOOL_RESULT]\n" + exact,
    ]
    assert reviewed[-1][1].endswith(exact)
    print("PASS: 2048-token follow-up preserved both an oversized typed request and its large file result.")


def compact_age_continuity(binary: Path, workspace: Path, url: str) -> None:
    home = workspace / "R2"
    original = home / "r2_original_conversation.txt"
    reality = home / "r2_reality.db"
    key = "original_conversation_birth_time"

    original.write_text("Synthetic original conversation.", encoding="utf-8")
    fresh = "How old are you? R2_OFFLINE_AGE_FRESH"
    conversation(binary, workspace, url, (fresh,), 2048)
    if os.environ.get("R2_TEST_VIRTUAL_CLOCK") == "1":
        with sqlite3.connect(home / "r2_memory.db") as db:
            stamp = db.execute(
                "SELECT e.local_time FROM r2_log_events e JOIN r2_log_conversations c ON c.event_id=e.id "
                "WHERE c.user_text=?", (fresh,),
            ).fetchone()[0]
            private_year = time.localtime(time.time() - 852076800).tm_year
            assert stamp.startswith(str(private_year) + "-"), stamp
    with sqlite3.connect(reality) as db:
        stored = int(db.execute("SELECT value FROM r2_reality_meta WHERE key=?", (key,)).fetchone()[0])
        assert abs(stored - time.time()) < 60
    original.unlink()

    old_anchor = str(stored - 186 * 86400)
    with sqlite3.connect(reality) as db:
        db.execute("UPDATE r2_reality_meta SET value=? WHERE key=?", (old_anchor, key))
    saved = "How old are you? R2_OFFLINE_AGE_SAVED"
    conversation(binary, workspace, url, (saved,), 2048)

    with sqlite3.connect(reality) as db:
        db.execute("UPDATE r2_reality_meta SET value=? WHERE key=?", (old_anchor + "invalid", key))
    invalid = "How old are you? R2_OFFLINE_AGE_INVALID"
    conversation(binary, workspace, url, (invalid,), 2048)
    with sqlite3.connect(reality) as db:
        assert db.execute("SELECT value FROM r2_reality_meta WHERE key=?", (key,)).fetchone() == (
            old_anchor + "invalid",
        )
        db.execute("UPDATE r2_reality_meta SET value=? WHERE key=?", (old_anchor, key))

    for marker, evidence in (
        (fresh, "AGE: about 0 days since the original conversation file was created."),
        (saved, "AGE: about half a year old"),
        (invalid, "creation time is unavailable or invalid; age is not guessed."),
    ):
        assert any(
            request.get("options", {}).get("num_ctx") == 2048
            and any(item.get("role") == "user" and item.get("content") == marker
                    for item in request.get("messages", []))
            and any(item.get("role") == "system" and item.get("content", "").startswith("PERSISTENT REALITY CONTEXT")
                    and evidence in item["content"] for item in request.get("messages", []))
            for request in LocalOllama.requests
        ), marker
    print("PASS: compact age context used the file creation date and preserved its authority across restart.")


def interrupt_partial_terminal_line(binary: Path, workspace: Path, url: str) -> None:
    log_file = workspace.parent / "partial-line-shutdown.log"
    unfinished = "R2_PARTIAL_INPUT_SHOULD_NEVER_REACH_THE_MODEL"
    start = len(LocalOllama.requests)
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, 2048),
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
        assert process.stdin is not None
        try:
            deadline = time.monotonic() + 20
            while process.poll() is None and time.monotonic() < deadline:
                if "R2> " in log_file.read_text(encoding="utf-8", errors="replace"):
                    break
                time.sleep(0.05)
            else:
                raise AssertionError(log_file.read_text(encoding="utf-8", errors="replace")[-16000:])
            process.stdin.write(f"talk {unfinished}")
            process.stdin.flush()
            time.sleep(0.2)
            # Leave stdin open and omit the newline, reproducing the blocked read.
            process.terminate()
            process.wait(timeout=8)
            assert process.returncode == 0
            assert "[R2-3PO shut down.]" in log_file.read_text(encoding="utf-8", errors="replace")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
    assert not any(
        unfinished in item.get("content", "")
        for request in LocalOllama.requests[start:] for item in request.get("messages", [])
    )
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        assert db.execute(
            "SELECT COUNT(*) FROM r2_log_conversations WHERE instr(user_text,?)", (unfinished,)
        ).fetchone() == (0,)
    print("PASS: interactive SIGTERM left the incomplete line undispatched and closed cleanly.")


def interrupt_foreground_reply(binary: Path, workspace: Path, url: str) -> None:
    log_file = workspace.parent / "active-reply-shutdown.log"
    LocalOllama.slow_started.clear()
    LocalOllama.slow_release.clear()
    with log_file.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, 2048),
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
        assert process.stdin is not None
        try:
            process.stdin.write(f"talk {STOPPED_REQUEST}\n")
            process.stdin.flush()
            assert LocalOllama.slow_started.wait(20), log_file.read_text(encoding="utf-8", errors="replace")
            process.terminate()
            process.wait(timeout=8)
            assert process.returncode == 0
            assert "[R2-3PO shut down.]" in log_file.read_text(encoding="utf-8", errors="replace")
        finally:
            LocalOllama.slow_release.set()
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        assert db.execute(
            "SELECT assistant_text FROM r2_log_conversations WHERE user_text=?", (STOPPED_REQUEST,)
        ).fetchall() == [(INTERRUPTED_REPLY,)]
    print("PASS: interactive SIGTERM cancelled active inference and preserved the accepted user turn.")


def reflection_after_long_foreground_turn(binary: Path, workspace: Path, url: str) -> None:
    log_path = workspace.parent / "long-foreground-idle.log"
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, 2048),
            stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
        )
        assert process.stdin is not None
        try:
            process.stdin.write(f"talk {LONG_FOREGROUND_REQUEST}\n")
            process.stdin.flush()
            deadline = time.monotonic() + 30
            with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
                while process.poll() is None and time.monotonic() < deadline:
                    row = db.execute(
                        "SELECT idle.monotonic_ms-turn.monotonic_ms "
                        "FROM r2_log_conversations c JOIN r2_log_events turn ON turn.id=c.event_id "
                        "JOIN r2_log_events idle ON idle.id>turn.id AND idle.event_type='cycle_started' "
                        "WHERE c.user_text=? ORDER BY idle.id LIMIT 1", (LONG_FOREGROUND_REQUEST,),
                    ).fetchone()
                    if row is not None:
                        assert row[0] >= 4000, row
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError(log_path.read_text(encoding="utf-8", errors="replace")[-20000:])
            process.terminate()
            assert process.wait(timeout=15) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdin.close()
    print("PASS: autonomous reflection waited a full idle interval after a long foreground turn.")


def recover_reflection_during_service(binary: Path, workspace: Path, url: str) -> None:
    env = runtime_environment(workspace, url, 2048)
    env["R2_SERVICE_MODE"] = "1"
    log_path = workspace.parent / "live-reflection-recovery.log"
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db, log_path.open("w", encoding="utf-8") as log:
        db.executescript(
            "CREATE TRIGGER block_review BEFORE INSERT ON r2_log_reflection_reviews "
            "BEGIN SELECT RAISE(ABORT,'fixture rejects reflection link'); END;"
            "CREATE TRIGGER block_mirror_ack BEFORE DELETE ON r2_diary_pending_mirrors "
            "BEGIN SELECT RAISE(IGNORE); END;"
            "CREATE TRIGGER block_private_extraction BEFORE UPDATE OF extraction ON r2_log_pending_self_reports "
            "WHEN OLD.event_id IN (SELECT id FROM r2_log_events WHERE event_type='reflection_completed') "
            "BEGIN SELECT RAISE(IGNORE); END;"
            "CREATE TRIGGER block_private_learning BEFORE DELETE ON r2_log_pending_learning "
            "WHEN OLD.user_text='' BEGIN SELECT RAISE(IGNORE); END;"
        )
        process = subprocess.Popen([str(binary)], cwd=workspace, env=env,
                                   stdout=log, stderr=subprocess.STDOUT)

        def wait_for(predicate):
            deadline = time.monotonic() + 40
            while process.poll() is None and time.monotonic() < deadline:
                result = predicate()
                if result:
                    return result
                time.sleep(0.1)
            raise AssertionError(log_path.read_text(encoding="utf-8", errors="replace")[-20000:])

        try:
            diary_id, timestamp, entry = wait_for(lambda: db.execute(
                "SELECT d.id,d.created_at,d.entry FROM r2_diary_pending_reflections p "
                "JOIN diary_entries d ON d.id=p.diary_entry_id "
                "JOIN r2_diary_pending_mirrors m ON m.diary_entry_id=d.id"
            ).fetchone())
            archives = db.execute(
                "SELECT archive_id FROM r2_diary_pending_reflection_reviews WHERE diary_entry_id=? ORDER BY ordinal",
                (diary_id,),
            ).fetchall()
            assert entry == REFLECTION and len(archives) == 2
            tool = db.execute(
                "SELECT result FROM r2_diary_pending_reflection_tool WHERE diary_entry_id=?", (diary_id,),
            ).fetchone()
            assert tool is not None and SENSE_RESULT in tool[0], tool
            daily = workspace / "R2_Diary" / (timestamp[:10] + ".md")
            marker = f"\n\n## {timestamp}\n\n{entry}\n"
            wait_for(lambda: daily.exists() and marker in daily.read_text(encoding="utf-8"))
            db.executescript("DROP TRIGGER block_review; DROP TRIGGER block_mirror_ack;")
            wait_for(lambda: not db.execute(
                "SELECT 1 FROM r2_diary_pending_reflections WHERE diary_entry_id=? UNION ALL "
                "SELECT 1 FROM r2_diary_pending_mirrors WHERE diary_entry_id=?", (diary_id, diary_id),
            ).fetchone())
            assert db.execute(
                "SELECT r.archive_id FROM r2_log_reflection_reviews r "
                "JOIN r2_sensory_journal_reflection_links l ON l.event_id=r.event_id "
                "WHERE l.diary_entry_id=? ORDER BY r.ordinal", (diary_id,),
            ).fetchall() == archives
            assert daily.read_text(encoding="utf-8").count(marker) == 1
            source = db.execute(
                "SELECT event_id FROM r2_sensory_journal_reflection_links WHERE diary_entry_id=?",
                (diary_id,),
            ).fetchone()[0]
            assert db.execute(
                "SELECT assistant_text,extraction FROM r2_log_pending_self_reports WHERE event_id=?",
                (source,),
            ).fetchone() == (REFLECTION, None)
            assert db.execute(
                "SELECT user_text,assistant_text,tool_result FROM r2_log_pending_learning WHERE event_id=?",
                (source,),
            ).fetchone() == ("", REFLECTION, tool[0])
            assert not db.execute(
                "SELECT 1 FROM r2_diary_pending_reflection_tool WHERE diary_entry_id=?", (diary_id,),
            ).fetchone()
            wait_for(lambda: f"Persistent memory transaction failed for event {source}."
                     in log_path.read_text(encoding="utf-8", errors="replace"))
            with LocalOllama.lock:
                before_recovery = len(LocalOllama.requests)
            db.executescript("DROP TRIGGER block_private_extraction; DROP TRIGGER block_private_learning;")
            wait_for(lambda: not db.execute(
                "SELECT 1 FROM r2_log_pending_self_reports WHERE event_id=? UNION ALL "
                "SELECT 1 FROM r2_log_pending_learning WHERE event_id=?", (source, source),
            ).fetchone())
            assert db.execute(
                "SELECT s.description,s.origin,l.notes FROM r2_log_links l "
                "JOIN r2_log_inner_states s ON s.event_id=l.to_event_id "
                "WHERE l.from_event_id=? AND l.relationship='extracts_self_state'", (source,),
            ).fetchall() == [("Kept earlier evidence separate.",
                               "private R2 diary reflection; model-extracted with quoted evidence",
                               PRIVATE_REFLECTION_QUOTE)]
            assert db.execute(
                "SELECT e.details FROM r2_log_links l JOIN r2_log_events e ON e.id=l.from_event_id "
                "WHERE l.to_event_id=? AND l.relationship='memory_learned_from'", (source,),
            ).fetchall() == [(PRIVATE_TOOL_MEMORY,)]
            with LocalOllama.lock:
                assert any(
                    any("memory-evaluation system" in m["content"] for m in request["messages"])
                    and any(("PRIVATE R2 REFLECTION" in m["content"] and REFLECTION in m["content"]
                             and tool[0] in m["content"] and "LATEST USER MESSAGE" not in m["content"])
                            for m in request["messages"])
                    for request in LocalOllama.requests[before_recovery:]
                )
            process.terminate()
            assert process.wait(timeout=15) == 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print("PASS: live service recovered reflection links, mirror, private extraction and learning without a restart.")


def recover_learning_after_restart(binary: Path, workspace: Path, url: str) -> None:
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        db.executescript(
            "CREATE TRIGGER ignore_learning_ack BEFORE DELETE ON r2_log_pending_learning "
            "WHEN OLD.user_text LIKE '%R2_LEARNING_RESTART_71412%' BEGIN SELECT RAISE(IGNORE); END;"
            "CREATE TRIGGER ignore_self_report_branch BEFORE INSERT ON r2_alternate_self_branches "
            "WHEN NEW.scenario='Build a spare copper bridge.' BEGIN SELECT RAISE(IGNORE); END;"
        )
        def extracted_requests():
            with LocalOllama.lock:
                return sum(
                    any("strict structured data extractor" in m["content"] for m in r["messages"])
                    and any(SELF_REPORT_REPLY in m["content"] for m in r["messages"])
                    for r in LocalOllama.requests
                )

        for phase in ("blocked", "restart", "acknowledge"):
            log_path = workspace.parent / f"learning-{phase}.log"
            with log_path.open("w", encoding="utf-8") as log:
                process = subprocess.Popen(
                    [str(binary)], cwd=workspace, env=runtime_environment(workspace, url, 8192),
                    stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True,
                )
                assert process.stdin is not None
                try:
                    if phase == "blocked":
                        process.stdin.write(f"talk {LEARNING_RESTART_REQUEST}\n")
                        process.stdin.flush()
                    deadline = time.monotonic() + 30
                    while process.poll() is None and time.monotonic() < deadline:
                        if phase == "blocked" and "Persistent memory transaction failed" in log_path.read_text(errors="replace"):
                            break
                        if phase == "restart" and db.execute(
                            "SELECT 1 FROM memories WHERE memory=?", (RESTART_MEMORY,),
                        ).fetchone():
                            break
                        if phase == "acknowledge" and not db.execute(
                            "SELECT 1 FROM r2_log_pending_self_reports WHERE event_id=?", (source,),
                        ).fetchone():
                            break
                        time.sleep(0.1)
                    else:
                        raise AssertionError(log_path.read_text(errors="replace")[-20000:])
                    pending = db.execute(
                        "SELECT event_id,assistant_text FROM r2_log_pending_learning WHERE user_text=?",
                        (LEARNING_RESTART_REQUEST,),
                    ).fetchall()
                    stored = db.execute("SELECT count(*) FROM memories WHERE memory=?", (RESTART_MEMORY,)).fetchone()
                    if phase == "blocked":
                        assert len(pending) == 1 and pending[0][1] == SELF_REPORT_REPLY, pending
                        source = pending[0][0]
                        assert stored == (0,), stored
                        assert db.execute(
                            "SELECT assistant_text,extraction FROM r2_log_pending_self_reports WHERE event_id=?",
                            (source,),
                        ).fetchone() == (SELF_REPORT_REPLY, SELF_REPORT_EXTRACTION)
                        receipts = db.execute(
                            "SELECT child_event_id FROM r2_log_self_report_records WHERE parent_event_id=? ORDER BY ordinal",
                            (source,),
                        ).fetchall()
                        assert len(receipts) == 2, receipts
                        assert db.execute(
                            "SELECT count(*) FROM r2_alternate_self_branches WHERE scenario=?",
                            (SELF_REPORT_SCENARIO,),
                        ).fetchone() == (0,)
                    else:
                        assert pending == [] and stored == (1,)
                        assert db.execute(
                            "SELECT count(*) FROM r2_log_links l JOIN r2_log_events e ON e.id=l.from_event_id "
                            "WHERE l.relationship='memory_learned_from' AND l.to_event_id=? AND e.details=?",
                            (source, RESTART_MEMORY),
                        ).fetchone() == (1,)
                        assert db.execute(
                            "SELECT count(*) FROM r2_log_pending_self_reports WHERE event_id=?",
                            (source,),
                        ).fetchone() == (int(phase == "restart"),)
                        assert db.execute(
                            "SELECT l.to_event_id,l.notes FROM r2_log_links l WHERE l.from_event_id=? "
                            "AND l.relationship IN ('extracts_self_state','contains_hypothetical') ORDER BY l.id",
                            (source,),
                        ).fetchall() == [
                            (receipts[0][0], "I intend to inspect the copper bridge."),
                            (receipts[1][0], "Hypothetically, I could build a spare bridge."),
                        ]
                        assert db.execute(
                            "SELECT log_event_id FROM r2_alternate_self_branches WHERE scenario=?",
                            (SELF_REPORT_SCENARIO,),
                        ).fetchall() == [receipts[1]]
                    assert extracted_requests() == 1
                    process.terminate()
                    assert process.wait(timeout=15) == 0
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait()
                    process.stdin.close()
            if phase == "blocked":
                db.executescript(
                    "DROP TRIGGER ignore_learning_ack; DROP TRIGGER ignore_self_report_branch;"
                    "CREATE TRIGGER ignore_self_report_ack BEFORE DELETE ON r2_log_pending_self_reports "
                    "WHEN OLD.assistant_text LIKE 'I intend to inspect the copper bridge.%' "
                    "BEGIN SELECT RAISE(IGNORE); END;"
                )
            elif phase == "restart":
                db.executescript("DROP TRIGGER ignore_self_report_ack;")
    print("PASS: restart recovered learning, reused the staged extraction and completed the original hypothetical branch.")


def recover_completed_hands_result(binary: Path, workspace: Path, url: str) -> None:
    with sqlite3.connect(workspace / "R2" / "r2_memory.db") as db:
        logfile = workspace.parent / "durable-hands.log"
        env = runtime_environment(workspace, url, 8192)
        with logfile.open("w", encoding="utf-8") as out:
            proc = subprocess.Popen([str(binary)], cwd=workspace, env=env, stdin=subprocess.PIPE,
                                    stdout=out, stderr=subprocess.STDOUT, text=True)
            try:
                assert proc.stdin is not None
                proc.stdin.write(f"talk {HANDS_QUEUE_REQUEST}\n")
                proc.stdin.flush()
                deadline = time.monotonic() + 20
                while proc.poll() is None and time.monotonic() < deadline:
                    result = db.execute(
                        "SELECT outcome_id,result FROM r2_log_hands_results WHERE delivered_to IS NULL;"
                    ).fetchone()
                    if result:
                        break
                    time.sleep(0.1)
                else:
                    raise AssertionError(logfile.read_text(errors="replace")[-16000:])
                outcome, exact = result
                assert exact.endswith(f"Output:\n{HANDS_RESULT}\n"), exact
                proc.kill()
                assert proc.wait(timeout=10) < 0
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.wait()
                proc.stdin.close()

        db.executescript(
            "CREATE TRIGGER block_hands_delivery BEFORE UPDATE OF delivered_to ON r2_log_hands_results "
            "BEGIN SELECT RAISE(IGNORE); END;"
        )
        conversation(binary, workspace, url, (HANDS_READ_REQUEST,), 8192)
        assert db.execute(
            "SELECT delivered_to FROM r2_log_hands_results WHERE outcome_id=?", (outcome,),
        ).fetchone() == (None,)
        assert db.execute(
            "SELECT count(*) FROM r2_log_links WHERE to_event_id=? AND relationship='hands_result_in_context'",
            (outcome,),
        ).fetchone() == (0,)
        db.executescript("DROP TRIGGER block_hands_delivery;")
        with LocalOllama.lock:
            cutoff = len(LocalOllama.requests)
        conversation(binary, workspace, url, (HANDS_READ_REQUEST, "The next unrelated turn."), 8192)
        with LocalOllama.lock:
            requests = LocalOllama.requests[cutoff:]
            tagged = "[R2_INTERNAL_TOOL_RESULT]\n" + exact
            assert any(any(message["content"] == tagged for message in request["messages"])
                       and any(message["content"] == HANDS_READ_REQUEST for message in request["messages"])
                       for request in requests)
            assert all(not any(message["content"] == tagged for message in request["messages"])
                       for request in requests
                       if any(message["content"] == "The next unrelated turn." for message in request["messages"]))
        assert db.execute(
            "SELECT r.result,c.user_text FROM r2_log_hands_results r "
            "JOIN r2_log_links l ON l.to_event_id=r.outcome_id AND l.from_event_id=r.delivered_to "
            "AND l.relationship='hands_result_in_context' "
            "JOIN r2_log_conversations c ON c.event_id=r.delivered_to WHERE r.outcome_id=?", (outcome,),
        ).fetchall() == [(exact, HANDS_READ_REQUEST)]
    print("PASS: exact Hands output survived a crash and rejected acknowledgement, then reached one linked turn.")


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
            oversized_typed_turn(binary, workspace, url)
            oversized_action_result(binary, workspace, url)
            oversized_user_and_action(binary, workspace, url)
            compact_age_continuity(binary, workspace, url)
            autonomous_cycle(binary, workspace, url)
            interrupt_partial_terminal_line(binary, workspace, url)
            interrupt_foreground_reply(binary, workspace, url)
            reflection_after_long_foreground_turn(binary, workspace, url)
            recover_reflection_during_service(binary, workspace, url)
            recover_learning_after_restart(binary, workspace, url)
            recover_completed_hands_result(binary, workspace, url)
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
