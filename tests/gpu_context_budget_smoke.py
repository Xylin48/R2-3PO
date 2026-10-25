#!/usr/bin/env python3
"""Keep the target 4 GiB GPU's default context within a safer KV-cache budget."""
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
assert '"R2_OLLAMA_NUM_CTX=${R2_OLLAMA_NUM_CTX:-}"' in runner
assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in launcher
assert '"$R2_OLLAMA_NUM_CTX_DEFAULTED" == "1" ]]; then' in launcher
assert 'GPU_LAYER_CANDIDATES=("$R2_OLLAMA_NUM_GPU_INITIAL" 12 8 4)' in launcher
assert "CONTEXT_CANDIDATES=(4096 2048)" in launcher
assert "CONTEXT_CANDIDATES=(8192 4096 2048)" in launcher
assert 'export R2_OLLAMA_NUM_GPU="$GPU_LAYERS"' in launcher
assert 'export R2_OLLAMA_NUM_CTX="$CONTEXT_TOKENS"' in launcher

assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in runner
assert '"$R2_NVIDIA_QUERY_OK" == "1" ]]' in runner
assert "GPU_LAYER_CANDIDATES=(\"$R2_OLLAMA_NUM_GPU_INITIAL\" 12 8 4)" in runner
assert "CONTEXT_CANDIDATES=(4096 2048)" in runner
assert "CONTEXT_CANDIDATES=(8192 4096 2048)" in runner
assert 'export R2_OLLAMA_NUM_GPU="$GPU_LAYERS"' in runner
assert 'export R2_OLLAMA_NUM_CTX="$CONTEXT_TOKENS"' in runner
assert 'R2_OLLAMA_NUM_CTX_DEFAULTED" == "1' in runner
assert "explicitly configured context" in runner
assert 'break 2' in runner
assert "without overriding explicit operator settings" in launcher
assert "never alter an explicit context" in runner
assert 'R2_OLLAMA_NUM_CTX_DEFAULTED="${R2_OLLAMA_NUM_CTX_DEFAULTED:-0}"' in runner
assert 'R2_OLLAMA_NUM_GPU_DEFAULTED="${R2_OLLAMA_NUM_GPU_DEFAULTED:-0}"' in runner
assert "export R2_OLLAMA_NUM_CTX_DEFAULTED" in runner
assert "export R2_OLLAMA_NUM_GPU_DEFAULTED" in runner
assert "R2_OLLAMA_NUM_CTX_DEFAULTED=0" in launcher
assert "R2_OLLAMA_NUM_GPU_DEFAULTED=0" in launcher
assert '"$R2_OLLAMA_NUM_GPU_DEFAULTED" == "1" &&' in runner
assert '"$R2_OLLAMA_NUM_CTX_DEFAULTED" == "1" ]]; then' in runner
assert "export R2_OLLAMA_NUM_GPU=12" in runner
assert "export R2_OLLAMA_NUM_CTX=4096" in runner
assert "Never silently override an explicit operator policy" in runner
assert 'R2_OLLAMA_NUM_CTX="${R2_OLLAMA_NUM_CTX:-}"' in launcher
assert 'requested >= 2048 && requested <= 16384' in core
assert 'safe_max = (int)requested;' in core
print("GTX 1050 Ti context defaults and explicit override path checks passed.")
