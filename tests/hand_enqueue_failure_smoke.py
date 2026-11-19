#!/usr/bin/env python3
"""Regression check: never report a HAND task as started when enqueue failed."""
from pathlib import Path
import re

source = Path("r2.c").read_text(encoding="utf-8")
calls = list(re.finditer(r"start_background_task\s*\([^;{}]*\)\s*;", source))
assert len(calls) == 3, f"expected 3 background enqueue call sites, found {len(calls)}"

for call in calls:
    branches = re.match(r"\s*if\s*\(id\)\s*\{([^{}]*)\}\s*else\s*\{([^{}]*)\}",
                        source[call.end():])
    assert branches, "task enqueue result is not checked before reporting success"
    success, failure = branches.groups()
    assert "Background task was queued." in success
    assert "Background task was queued." not in failure
    assert "The command passed validation, but the task could not be queued." in failure, (
        "enqueue failure must be reported as a failure, not a successful task start"
    )

print("Background task enqueue failures are reported accurately at all three dispatch sites.")
