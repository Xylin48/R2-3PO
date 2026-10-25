#!/usr/bin/env python3
"""Keep GTX 1050 Ti context defaults and recovery aligned across launch paths."""
from pathlib import Path

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
    assert "export R2_OLLAMA_NUM_CTX_DEFAULTED" in source, label
    assert "export R2_OLLAMA_NUM_GPU_DEFAULTED" in source, label

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
