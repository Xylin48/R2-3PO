#!/usr/bin/env python3
"""Keep the interactive launcher, Makefile, model, and cleanup policy in sync."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
header = (ROOT / "r2.h").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        print(f"FAIL: {message}", file=sys.stderr)
        raise SystemExit(1)
    print(f"PASS: {message}")


sources_match = re.search(r"^SOURCES\s*=\s*(.+)$", makefile, re.MULTILINE)
headers_match = re.search(r"^HEADERS\s*=\s*(.+)$", makefile, re.MULTILINE)
required_match = re.search(
    r"^REQUIRED_FILES=\(\n(?P<body>.*?)\n\)",
    launcher,
    re.MULTILINE | re.DOTALL,
)
compile_start = launcher.find('run_as_r2 gcc -std=c11 -Wall -Wextra -Werror -O2')
compile_end = launcher.find("\nstatus=$?", compile_start)
require(sources_match is not None, "Makefile declares its complete production source list")
require(headers_match is not None, "Makefile declares its complete public/internal header list")
require(required_match is not None, "launcher checks its complete required-file inventory")
require(compile_start >= 0 and compile_end > compile_start,
        "launcher has a bounded strict-build command")
if not all((sources_match, headers_match, required_match)) or compile_end <= compile_start:
    raise SystemExit(1)

sources = sources_match.group(1).split()
headers = headers_match.group(1).split()
compile_block = launcher[compile_start:compile_end]
compiled = re.findall(r'"\$R2_SOURCE/([^"]+\.c)"', compile_block)
required = re.findall(r'"\$R2_SOURCE/([^"]+)"', required_match.group("body"))
require(compiled == sources,
        "launcher compiles every Makefile source exactly once and in the same order")
require(len(compiled) == len(set(compiled)),
        "launcher compile list contains no duplicate source files")
require(all(name in required for name in headers),
        "launcher preflight requires every Makefile header before compiling")
require(all(name in required for name in sources),
        "launcher preflight requires every compiled C source before compiling")

model_match = re.search(r'^#define\s+R2_OLLAMA_MODEL\s+"([^"]+)"',
                        header, re.MULTILINE)
vision_match = re.search(r'^#define\s+R2_VISION_DEFAULT_MODEL\s+"([^"]+)"',
                        header, re.MULTILINE)
require(model_match is not None and vision_match is not None,
        "independent conversation and vision model constants exist")
if model_match and vision_match:
    conversation, vision = model_match.group(1), vision_match.group(1)
    require(f'R2_VISION_MODEL="{vision}"' in launcher,
            "launcher selects the compiled dedicated vision model")
    require(f"Conversation model: {conversation}" in launcher
            and f"Visual perception model: {vision}" in launcher,
            "launcher status text identifies both compiled models")

require('R2_OLLAMA_TIMEOUT_SECONDS="${R2_OLLAMA_TIMEOUT_SECONDS:-}"' in launcher,
        "launcher passes the configured long-running inference timeout")
require('R2_OLLAMA_TRACE_PROMPTS="${R2_OLLAMA_TRACE_PROMPTS:-}"' in launcher,
        "launcher forwards the opt-in prompt trace setting to the dedicated account")
require('R2_FX_DIR="$R2_FX_DIR"' in launcher
        and 'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"' in launcher,
        "launcher uses the canonical, overridable sound-effects directory")
require('trap r2_network_cleanup EXIT' in launcher
        and 'r2_network_cleanup() {' in launcher
        and 'r2_observer_cleanup' in launcher,
        "EXIT cleanup always handles observer/display permissions and network rules")
require(re.search(r"\bsudo -u r2\b", launcher) is None,
        "all optional r2 helper/observer launches are non-interactive and cannot hang on a sudo prompt")
cleanup_start = launcher.find("r2_observer_cleanup() {")
cleanup_end = launcher.find("\nr2_remove_network_jumps()", cleanup_start)
cleanup = launcher[cleanup_start:cleanup_end]
require('kill "$OBSERVER_PID"' in cleanup
        and 'wait "$OBSERVER_PID"' in cleanup
        and 'xhost -SI:localuser:r2' in cleanup,
        "observer cleanup terminates/reaps its process and revokes X access")
require('run_as_r2 mktemp -d -- "$R2_ROOT/.r2-build.XXXXXXXX"' in launcher
        and 'TEMP_EXEC="$R2_BUILD_DIR/r2"' in launcher
        and 'mv -fT -- "$TEMP_EXEC" "$R2_EXEC"' in launcher
        and 'r2_build_cleanup' in launcher,
        "launcher preserves the last known-good executable until a successful build")
print("Launcher/core/module consistency smoke test passed.")

conflict_start = launcher.index("# Check from the desktop account before restricting r2")
isolation_start = launcher.index("if ! r2_apply_network_lock; then")
conflict_check = launcher[conflict_start:isolation_start]
require(
    conflict_start < isolation_start
    and "systemctl is-active --quiet r2-ollama.service" in conflict_check
    and "http://127.0.0.1:11435/api/tags" in conflict_check
    and "run_as_r2" not in conflict_check
    and "exit 1" in conflict_check,
    "launcher checks for an active service or second Ollama API before restricting the runtime account",
)
