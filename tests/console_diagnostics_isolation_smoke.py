#!/usr/bin/env python3
"""Keep background diagnostics out of R2's interactive conversation terminal."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
shell = (ROOT / "shell.c").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

def require(ok: bool, message: str) -> None:
    if not ok:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")

require(
    '"$R2_ROOT/R2_Log"' in launcher,
    "launcher creates the canonical R2_Log directory as the runtime user",
)
require(
    'run_as_r2 env' in launcher and 'exec "$1" 2>>"$2"' in launcher
    and '"$R2_ROOT/R2_Log/Debug.txt" "$R2_MIRROR_DIAGNOSTICS"' in launcher
    and 'R2_MIRROR_DIAGNOSTICS=0' in launcher,
    "R2 appends stderr to its private log by default under the shared runtime user",
)
require(
    '2> >(tee -a -- "$2" >&2)' in launcher,
    "opt-in live diagnostics show stderr while retaining the log",
)
require(
    'if [[ "${R2_LIVE_DIAGNOSTICS:-0}" == 1 ]]; then' in launcher
    and 'tail --pid="$$" -n +1 -f --' in launcher
    and 'exec {R2_CORE_STDERR}>>"$R2_DIAGNOSTICS_DIR/Debug.txt"' in launcher
    and '2>&"$R2_CORE_STDERR"' in launcher,
    "live diagnostics use the separate terminal stream without writing over conversation output",
)
require(
    '2>>"$2"' in launcher and '1>>"$2"' not in launcher,
    "only diagnostic stderr is redirected; the user-facing reply stream remains on stdout",
)
require(
    '[R2 Input Audit] shell_message_chars=%zu' in shell
    and '[R2 Input Audit] r2_talk_received_chars=%zu' in core,
    "input length is audited at both shell and conversation-core boundaries without logging message contents",
)
hand_marker = core.find('"[HAND %s] %.*s"')
hand_prefix = core[max(0, hand_marker - 100):hand_marker]
require(
    hand_marker >= 0 and "fprintf" in hand_prefix and "stderr" in hand_prefix,
    "background HANDS output goes to diagnostics rather than the interactive reply stream",
)
diary_marker = core.find("The full reflection is stored in the diary.")
diary_end = core.find("} else {", diary_marker)
diary_block = core[diary_marker:diary_end] if diary_marker >= 0 and diary_end > diary_marker else ""
require(
    diary_marker >= 0 and "fprintf(stderr" in diary_block and "\n            printf(" not in diary_block,
    "background diary reflections are archived without printing their contents over the conversation terminal",
)
print("R2 console diagnostic isolation smoke test passed.")
