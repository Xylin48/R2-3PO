#!/usr/bin/env python3
"""Safety/coverage contract for the non-destructive hardware preflight."""
from pathlib import Path
import json
import os
import sqlite3
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
script = (ROOT / "tools/preflight_r2_hardware.sh").read_text(encoding="utf-8")
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
service = (ROOT / "deploy/r2-3po.service").read_text(encoding="utf-8")
required = (
    'set -uo pipefail',
    'R2_ROOT="${R2_ROOT:-/home/x/R2_Home}"',
    'R2_FX_DIR="${R2_FX_DIR:-$R2_ROOT/R2_sounds/FX}"',
    'http://127.0.0.1:11434/api/tags',
    'curl --noproxy 127.0.0.1 --silent --show-error --fail --max-time 2 http://127.0.0.1:11435/api/tags',
    'curl --noproxy 127.0.0.1 --silent --show-error --fail --max-time 4 "$OLLAMA_URL"',
    'llama3.1:8b', 'qwen3-vl:4b-instruct-q4_K_M',
    'r2_memory.db', 'r2_reality.db', 'r2_fridge.db',
    'r2_rewards.db', 'r2_addictions.db',
    'pactl', 'R2 runtime account can connect to its default PulseAudio/PipeWire server',
    '/dev/video*', 'R2 runtime account can read/write',
    '/dev/snd/*',
    "R2 service's configured audio supplementary group can access", 'mGBA Qt emulator',
    'stop it before final activation or taking deployment backups',
    'run_as_r2 test -r "$R2_FX_DIR"',
    'r2 must have read/write/traverse access to $path for runtime directories, SQLite, and persistent memory',
    'existing runtime directory is not read/write/traverse accessible to r2',
    'existing database must be a readable/writable regular file for r2',
    'r2_diary.c r2_diary.h',
    'r2.c r2.h r2_input.h',
    'AudioCapture.c AudioCapture.h',
    'AudioDiscovery.c AudioDiscovery.h',
    'AudioFile.c AudioFile.h',
    'Reality.c Reality.h',
    'Addiction.c Addiction.h',
    'Reward.c Reward.h',
    'AlternateSelf.c AlternateSelf.h',
    'Imagination.c Imagination.h',
    'SensoryJournal.c SensoryJournal.h',
    'R2Sounds.c R2Sounds.h',
    'R2Reply.c R2Reply.h',
    'shell.c shell.h',
    'V-Webcam.py',
    'Observer.py',
    'deploy/r2-ollama.service',
    'tools/r2_ollama_runner.sh',
    'tools/copy_ollama_model.py',
    'tools/preflight_ollama_model.py',
    'tools/r2_install_root_bootstrap.sh',
    'tools/audit_r2_sound_assets.py',
    'tools/preflight_r2_hardware.sh',
    'for tool in bash make gcc curl python3 sudo setpriv pgrep date find grep cmp iptables ip6tables',
    'run_as_r2_with_audio_group()',
    'r2-ollama.service is active on 127.0.0.1:11435',
    'env -u OLLAMA_HOST ollama ps 2>&1',
    'http://127.0.0.1:11435/api/tags',
    'a separate Ollama API is responding on 127.0.0.1:11435',
    "pgrep -af '[l]lama-server'",
    '$1 != "NAME"',
    'llama-server process exists but ollama ps cannot confirm a loaded model',
    'Hardware preflight identity: %s (uid=%s, groups=%s)',
    'for DEVICE_GROUP in audio video; do',
    'DROP=(setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" --)',
    'sudo -n "${DROP[@]}" env HOME="$R2_ROOT" USER=r2 LOGNAME=r2',
    'sudo -n setpriv --reuid="$uid" --regid="$gid" --groups="$groups" -- "$@"',
    'libfaketime.so.1 is missing',
    'sudo apt-get update && sudo apt-get install -y ffmpeg',
    'sudo apt-get update && sudo apt-get install -y pulseaudio-utils',
    'physical-device/configuration issue, not a missing package',
    'overnight service installation cannot proceed',
)
missing = [item for item in required if item not in script]
if missing:
    raise SystemExit(f"preflight contract missing expected checks: {missing}")
if "SupplementaryGroups=audio" not in service:
    raise SystemExit("R2 service must receive service-scoped ALSA access through the audio group")
for forbidden in ('chmod 777', 'systemctl enable', 'systemctl start',
                  'systemctl stop', 'make clean all', 'rm -f',
                  'sqlite3', 'PRAGMA integrity_check', 'audit_r2_databases.py --root'):
    if forbidden in script:
        raise SystemExit(f"preflight must be read-only; found forbidden operation: {forbidden}")
# Package-manager commands may appear only as human-readable guidance in warning
# strings; the read-only report must never execute a package-manager operation.
for line in script.splitlines():
    if line.lstrip().startswith(('apt-get update', 'apt-get install', 'sudo apt-get update',
                                 'sudo apt-get install')):
        raise SystemExit("preflight must not execute package-manager commands")
if 'sudo -n -u r2 -g r2 -G audio' in script:
    raise SystemExit("preflight must not use unsupported sudo-rs -G syntax")
if 'sudo -n -u r2 test -r' in script or 'sudo -n -u r2 pactl info' in script:
    raise SystemExit("runtime checks must execute directly as r2 after the preflight privilege drop")
if 'R2_HANDOFF=(sudo -n setpriv --reuid="$R2_UID" --regid="$R2_GID" --groups="$R2_RUNTIME_GROUPS" --' not in launcher:
    raise SystemExit("launcher must use setpriv for supplementary groups under sudo-rs")
if 'sudo -u r2 -g r2 -G "$R2_RUNTIME_GROUPS"' in launcher:
    raise SystemExit("launcher must not use unsupported sudo-rs -G syntax")
if 'run_as_r2 mkdir -p "$DIR"' not in launcher:
    raise SystemExit("launcher must create runtime directories as r2, not as the desktop login user")
if 'Could not create workspace directory as r2' not in launcher:
    raise SystemExit("launcher must stop if runtime-owned directory creation fails")
if 'R2_OLLAMA_URL:-http://127.0.0.1:11434/api/tags' not in script:
    raise SystemExit("Ollama endpoint must remain locally configurable")

launch_required = (
    '"$R2_SOURCE/V-Webcam.py"',
    'R2_PREFLIGHT="$R2_SOURCE/tools/preflight_r2_hardware.sh"',
    'bash "$R2_PREFLIGHT"',
    'R2 launch cancelled: preflight reported one or more failures.',
    'Press Enter to continue launching R2, or Ctrl+C to cancel...',
)
missing_launch = [item for item in launch_required if item not in launcher]
if missing_launch:
    raise SystemExit(f"launcher does not run/guard the startup preflight: {missing_launch}")
# Check for an already-running core before installing UID-scoped firewall rules.
# Then isolate r2 before the preflight's runtime-account checks, and finish all
# read-only preflight checks before workspace writes, package installation,
# compilation, and Observer startup.
network_at = launcher.index('echo "[Network] Applying R2-only network isolation..."')
preflight_at = launcher.index('bash "$R2_PREFLIGHT"')
if launcher.index("systemctl is-active --quiet r2-3po.service") > network_at:
    raise SystemExit("duplicate service check must precede temporary network rules")
if launcher.index("pgrep -x r2") > network_at:
    raise SystemExit("duplicate process check must precede temporary network rules")
if network_at > preflight_at:
    raise SystemExit("R2-only network isolation must precede runtime preflight")
sqlite_at = launcher.index('python3 "$R2_DATABASE_AUDIT"')
if not (preflight_at <
        launcher.index("Press Enter to continue launching R2") < sqlite_at <
        launcher.index("Checking R2 workspace directories...")):
    raise SystemExit("database integrity check must follow final launch authorization and precede workspace writes")
audit_path = ROOT / "tools" / "audit_r2_databases.py"
if '?mode=ro' not in audit_path.read_text(encoding="utf-8"):
    raise SystemExit("the final interactive activation must open databases read-only during integrity checking")
for marker in (
    'Checking R2 workspace directories...',
    'Checking the local Ollama API and required model',
    'sudo apt-get install -y libfaketime',
    'Recompiling R2',
    'Launching the read-only observer window',
):
    if launcher.index(marker) < preflight_at:
        raise SystemExit(f"startup preflight must precede {marker!r}")
# Verify the exact awk predicate treats the standard header-only output
# from `ollama ps` as empty, but recognizes a real model row.
model_row_predicate = 'NR > 1 && NF && $1 != "NAME" { found=1 } END { exit found ? 0 : 1 }'
empty_ps = subprocess.run(
    ["awk", model_row_predicate],
    input="NAME ID SIZE PROCESSOR CONTEXT UNTIL\n",
    text=True, capture_output=True, check=False,
)
loaded_ps = subprocess.run(
    ["awk", model_row_predicate],
    input="NAME ID SIZE PROCESSOR CONTEXT UNTIL\nllama3.1:8b abc 6.0 GiB 20%/80% 4096 5m\n",
    text=True, capture_output=True, check=False,
)
if empty_ps.returncode != 1 or loaded_ps.returncode != 0:
    raise SystemExit("ollama ps row detection must ignore the header and detect actual model rows")

# Check the supplementary group list used before the report drops privilege.
group_start = script.index('  R2_RUNTIME_GROUPS="$(id -G r2')
group_end = script.index('  if [[ ! "$R2_UID"', group_start)
group_fixture = """
set -uo pipefail
id() { printf '311 29\\n'; }
getent() {
    case "$2" in
        audio) printf 'audio:x:29:\\n' ;;
        video) [[ "$1" == group && "$ADD_VIDEO" == yes ]] || return 2
               printf 'video:x:44:\\n' ;;
    esac
}
ADD_VIDEO="$1"
""" + script[group_start:group_end] + 'printf "%s\\n" "$R2_RUNTIME_GROUPS"'
for video, groups in (("yes", "311,29,44"), ("no", "311,29")):
    result = subprocess.run(["bash", "-c", group_fixture, "fixture", video],
                            text=True, capture_output=True, check=True)
    if result.stdout.strip() != groups:
        raise SystemExit(f"expected {groups} in hardware report group handoff: {result}")

# Exercise the same model parser the hardware report runs against /api/tags.
tags_parser = script.split("printf '%s' \"$TAGS\" | python3 -c '", 1)[1].split("\n'", 1)[0]
for response, expected in (
    ({"models": [None, {"name": "llama3.1:8b"},
                 {"name": "qwen3-vl:4b-instruct-q4_K_M"}]}, 0),
    ({"models": [{"name": "llama3.1:8b"}]}, 1),
    ({"models": [{"name": "qwen3-vl:4b-instruct-q4_K_M"}]}, 1),
    ({"models": [{"name": "llama3.1:8b-other"},
                 {"name": "qwen3-vl:4b-instruct-q4_K_M"}]}, 1),
    ({"models": "llama3.1:8b"}, 1),
    (["llama3.1:8b", "qwen3-vl:4b-instruct-q4_K_M"], 1),
):
    probe = subprocess.run([sys.executable, "-c", tags_parser],
                           input=json.dumps(response), text=True, capture_output=True)
    if probe.returncode != expected:
        raise SystemExit(f"wrong hardware model verdict for {response!r}: {probe}")

# Run the filesystem part of the report without privileges or live paths.
database_block = script.split('    for database in ', 1)[1].split('  fi\nelse fail "workspace', 1)[0]
with tempfile.TemporaryDirectory() as temp:
    data = Path(temp)
    (data / "r2_memory.db").symlink_to(data / "missing.db")
    shell_fixture = """
R2_SOURCE="$1"
fail() { printf '%s\\n' "$*"; }
pass() { :; }
run_as_r2() { "$@"; }
""" + "    for database in " + database_block
    result = subprocess.run(["bash", "-c", shell_fixture, "fixture", temp],
                            text=True, capture_output=True, check=True)
    if "refusing symlinked runtime database" not in result.stdout:
        raise SystemExit("hardware report missed a dangling database symlink")

# Execute the launcher's actual audit block with a disposable data root.
probe_start = launcher.index('R2_DATABASE_AUDIT=')
probe_end = launcher.index("# Create only the established workspace", sqlite_at)
sqlite_probe = (
    'R2_ROOT="$1"\nR2_SOURCE="$1/source"\nR2_GAMEBOY_ROOT="$1/Devices/GameBoyAdvance"\n'
    'shift\nrun_as_r2() { "$@"; }\n'
    + launcher.split('R2_ROOT=', 1)[0]
    + launcher[probe_start:probe_end].replace(
        '"$R2_SOURCE/tools/audit_r2_databases.py"', '"$R2_AUDIT_FIXTURE"'
    )
)
with tempfile.TemporaryDirectory() as temp:
    data = Path(temp) / "source"
    data.mkdir()
    memory = data / "r2_memory.db"
    with sqlite3.connect(memory) as database:
        database.execute("CREATE TABLE r2_fridge_items(name TEXT, quantity INTEGER)")
    database.close()
    original = memory.read_bytes()
    probe_args = ["bash", "-c", sqlite_probe, "launcher-fixture", temp]
    probe_env = {**os.environ, "R2_AUDIT_FIXTURE": str(audit_path)}
    def run_probe(*args):
        return subprocess.run([*probe_args, *args], env=probe_env, text=True, capture_output=True)

    result = run_probe()
    if result.returncode != 1 or "Database audit failed" not in result.stdout:
        raise SystemExit(f"partial modern database set passed activation check: {result}")
    result = run_probe("--legacy-memory-only")
    if result.returncode != 0 or "Structured memory-only layout verified" not in result.stdout:
        raise SystemExit(f"legacy database failed explicit activation check: {result}")
    snapshots = list(Path(temp).glob("audit-snapshot-desktop.*/r2_memory.db"))
    if len(snapshots) != 1:
        raise SystemExit(f"legacy activation did not snapshot its source: {snapshots}")
    if memory.read_bytes() != original or set(data.iterdir()) != {memory}:
        raise SystemExit("integrity check modified the detached database or created missing stores")

    for name in ("r2_reality.db", "r2_fridge.db", "r2_rewards.db", "r2_addictions.db"):
        sqlite3.connect(data / name).close()
    for args in ((), ("--activate-reviewed",)):
        result = run_probe(*args)
        if result.returncode != 0 or "5/5 required database files found" not in result.stdout:
            raise SystemExit(f"valid desktop launch arguments failed: {result}")
    device = Path(temp) / "Devices" / "GameBoyAdvance"
    device.mkdir(parents=True)
    (device / "gameboy.db").write_bytes(b"a corrupt cartridge database fixture")
    result = run_probe()
    if result.returncode != 1 or "ERROR  Devices/GameBoyAdvance/gameboy.db" not in result.stdout:
        raise SystemExit(f"corrupt sibling cartridge database passed activation check: {result}")
    (device / "gameboy.db").unlink()
    memory.write_bytes(b"a corrupt database fixture")
    result = run_probe()
    if result.returncode != 1 or "ERROR  r2_memory.db" not in result.stdout:
        raise SystemExit(f"corrupt detached database passed activation check: {result}")

print("R2 hardware preflight safety, coverage, and launcher-order smoke test passed")
