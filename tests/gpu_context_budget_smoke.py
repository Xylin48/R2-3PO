#!/usr/bin/env python3
"""Keep GTX 1050 Ti context defaults and recovery aligned across launch paths."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
launcher = (ROOT / "R2_Launch_Code.sh").read_text(encoding="utf-8")
runner = (ROOT / "tools/r2_service_runner.sh").read_text(encoding="utf-8")
core = (ROOT / "r2.c").read_text(encoding="utf-8")

for label, source in (("interactive launcher", launcher), ("overnight service", runner)):
    assert "nvidia-smi --query-gpu=name --format=csv,noheader" in source, label
    assert "grep -qi 'GTX 1050 Ti'" in source, label
    assert "export R2_OLLAMA_NUM_CTX=8192" in source, label
    assert '[[ -z "${R2_OLLAMA_NUM_CTX:-}" && "$R2_NVIDIA_QUERY_OK" == "1" ]]' in source, label

# Interactive and overnight launch paths must use the same bounded retry matrix.
for label, source in (("interactive launcher", launcher), ("overnight service", runner)):
    assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in source, label
    assert 'GPU_LAYER_CANDIDATES=("$R2_OLLAMA_NUM_GPU_INITIAL" 12 8 4)' in source, label
    assert "CONTEXT_CANDIDATES=(4096 2048)" in source, label
    assert "CONTEXT_CANDIDATES=(8192 4096 2048)" in source, label
    assert 'export R2_OLLAMA_NUM_GPU="$GPU_LAYERS"' in source, label
    assert 'export R2_OLLAMA_NUM_CTX="$CONTEXT_TOKENS"' in source, label
    assert 'R2_OLLAMA_NUM_CTX_DEFAULTED" == "1' in source, label
    assert "explicitly configured context" in source, label
    assert "break 2" in source, label

    start = source.index("if run_as_r2 env" if source is launcher else
                         'if python3 "$R2_SOURCE/tools/preflight_ollama_model.py"',
                         source.index('echo "[preflight] Checking' if source is launcher else
                                      'log "Waiting for local Qwen'))
    block = source[start:source.index("\nfi\n", start) + len("\nfi\n")]
    setup = """
set -euo pipefail
R2_SOURCE=/fixture
R2_OLLAMA_BASE_URL=http://127.0.0.1:11434
R2_NVIDIA_GPU_NAMES="NVIDIA GeForce GTX 1050 Ti"
R2_NVIDIA_QUERY_OK=1
R2_OLLAMA_NUM_GPU_DEFAULTED=1
R2_OLLAMA_NUM_CTX_DEFAULTED=1
R2_OLLAMA_NUM_GPU=20
R2_OLLAMA_NUM_CTX=8192
R2_OLLAMA_PREFLIGHT_ARGS=()
PREFLIGHT_GPU_ARGS=()
replies=("$@")
attempt=0
probe() {
    printf 'probe %s %s\n' "$R2_OLLAMA_NUM_GPU" "$R2_OLLAMA_NUM_CTX"
    local status=${replies[attempt]:-99}
    attempt=$((attempt + 1))
    return "$status"
}
run_as_r2() { probe; }
python3() { probe; }
log() { printf '%s\n' "$*"; }
fail() { printf '%s\n' "$*" >&2; exit 1; }
"""
    for codes, expected, success in (
        (["1"], ["probe 20 8192"], False),
        (["75", "1"], ["probe 20 8192", "probe 20 4096"], False),
        (["75", "75", "0"], ["probe 20 8192", "probe 20 4096", "probe 20 2048"], True),
    ):
        result = subprocess.run(["bash", "-c", setup + block, "gpu-test", *codes],
                                stdin=subprocess.DEVNULL, text=True, capture_output=True, timeout=10)
        assert (result.returncode == 0) == success, (label, result)
        assert [line for line in result.stdout.splitlines() if line.startswith("probe ")] == expected, (
            label, result)

bootstrap = (ROOT / "tools/r2_install_root_bootstrap.sh").read_text(encoding="utf-8")
start = bootstrap.index("if run_as_r2 env", bootstrap.index('log "Preflight GPU/context policy:'))
block = bootstrap[start:bootstrap.index("\nfi\n", start) + len("\nfi\n")]
installer_setup = setup + """
R2_MODEL=llama3.1:8b
PREFLIGHT_GPU=20
PREFLIGHT_CTX=8192
PREFLIGHT_POLICY_DEFAULTED=1
root_run() { :; }
run_as_r2() {
    R2_OLLAMA_NUM_GPU="$PREFLIGHT_GPU" R2_OLLAMA_NUM_CTX="$PREFLIGHT_CTX" probe
}
"""
for codes, expected, success in (
    (["1"], ["probe 20 8192"], False),
    (["75", "0"], ["probe 20 8192", "probe 12 4096"], True),
):
    result = subprocess.run(["bash", "-c", installer_setup + block, "gpu-test", *codes],
                            stdin=subprocess.DEVNULL, text=True, capture_output=True, timeout=10)
    assert (result.returncode == 0) == success, result
    assert [line for line in result.stdout.splitlines() if line.startswith("probe ")] == expected, result

assert "export R2_OLLAMA_NUM_CTX_DEFAULTED" in runner
assert "export R2_OLLAMA_NUM_GPU_DEFAULTED" in runner
assert '"R2_OLLAMA_NUM_CTX=${R2_OLLAMA_NUM_CTX:-}"' in runner
assert "without overriding explicit operator settings" in launcher
assert "never alter an explicit context" in runner
assert 'R2_OLLAMA_NUM_CTX_DEFAULTED="${R2_OLLAMA_NUM_CTX_DEFAULTED:-0}"' in runner
assert 'R2_OLLAMA_NUM_GPU_DEFAULTED="${R2_OLLAMA_NUM_GPU_DEFAULTED:-0}"' in runner
assert "R2_OLLAMA_NUM_CTX_DEFAULTED=0" in launcher
assert "R2_OLLAMA_NUM_GPU_DEFAULTED=0" in launcher
assert 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' in launcher
assert 'requested >= 2048 && requested <= 16384' in core
assert 'safe_max = (int)requested;' in core
print("GTX 1050 Ti context defaults and recovery parity checks passed.")
