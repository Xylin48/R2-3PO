#!/usr/bin/env python3
"""Guard shared bounded shell input and visible overflow handling."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
shell = (root / "shell.c").read_text(encoding="utf-8")
reader = (root / "r2_input.h").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        raise SystemExit(f"FAIL: {message}")

require("R2_MAX_TURN_INPUT_BYTES" in shell and "char *line = NULL;" in shell,
        "shell bounds dynamic input with the same limit as the direct console")
loop = shell.find("int r2_shell_run(void)")
read = shell.find("r2_read_complete_line(stdin, &line, &line_capacity, R2_MAX_TURN_INPUT_BYTES, &input_bytes)", loop)
check = shell.find("if (input_status == R2_INPUT_LINE_TOO_LONG)", read)
dispatch = shell.find("shell_dispatch(line);", check)
require(min(loop, read, check, dispatch) >= 0 and read < check < dispatch,
        "bounded input is checked before command dispatch")
guard = shell[check:dispatch]
require("Nothing from this line was sent to R2." in guard and
        "fflush(stdout);" in guard,
        "overflow rejection is visible on stdout and never silently truncates")
require("fgetc(stream)" in reader and "R2_INPUT_LINE_EMBEDDED_NUL" in reader and
        "too_long = 1;" in reader,
        "shared reader bounds storage, drains full lines, and rejects embedded NUL")
print("Shell input boundary regression passed.")
