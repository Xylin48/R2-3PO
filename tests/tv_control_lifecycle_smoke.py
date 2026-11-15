#!/usr/bin/env python3
"""Guard TV control socket framing and bounded shutdown behavior."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
shell = (ROOT / "shell.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


start = shell.find("static void *tv_control_server(void *unused)")
end = shell.find("static int tv_control_start(void)", start)
if start < 0 or end <= start:
    raise SystemExit("FAIL: could not isolate tv_control_server()")
server = shell[start:end]

require("#include <poll.h>" in shell,
        "TV server uses poll-based bounded waits")
require("poll(&listener, 1, 250)" in server
        and "if (ready == 0) continue;" in server,
        "idle listener checks its stop flag instead of blocking forever in accept")
require("poll(&input, 1, 250)" in server
        and "idle_polls >= 8" in server,
        "connected clients have a bounded idle-read timeout")
require("int complete_line = 0;" in server
        and "complete_line = 1;" in server
        and "if (!complete_line || atomic_load(&tv_control_stop) || memchr(command," in server,
        "partial or oversized socket commands cannot trigger TV actions")
require("int client = accept(server_fd, NULL, NULL);" in server
        and "POLLIN" in server,
        "accept occurs only after listener readiness is polled")
require("poll(&output, 1, 250)" in server
        and "reply_idle >= 8" in server
        and "reply_sent < reply_len && !atomic_load(&tv_control_stop)" in server
        and "MSG_NOSIGNAL | MSG_DONTWAIT" in server,
        "a client that stops reading cannot hold the shutdown join indefinitely")

stop_start = shell.find("static void tv_control_stop_server(void)")
startup = shell[end:stop_start]
bind_failure = startup.find("if (bind(fd,")
owned_socket = startup.find("if (chmod(tv_control_path", bind_failure)
require(bind_failure >= 0 and owned_socket > bind_failure
        and "return -1;" in startup[bind_failure:owned_socket]
        and "unlink(" not in startup[bind_failure:owned_socket],
        "a failed bind leaves the other listener's socket path intact")
stop_end = shell.find("/* ============================================================\n   SIGNAL HANDLING", stop_start)
if stop_start < 0 or stop_end <= stop_start:
    raise SystemExit("FAIL: could not isolate tv_control_stop_server()")
stop = shell[stop_start:stop_end]
join = stop.find("pthread_join(tv_control_thread, NULL)")
close = stop.find("close(tv_control_fd)")
require(join >= 0 and close > join
        and "atomic_store(&tv_control_stop, 1);" in stop,
        "listener descriptor remains open until the polling server thread joins")

print("TV control socket lifecycle smoke test passed.")
