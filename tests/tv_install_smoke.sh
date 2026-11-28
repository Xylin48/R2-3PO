#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TMP_ROOT="$(mktemp -d)"
trap 'rm -rf "$TMP_ROOT"' EXIT

R2_HOME="$TMP_ROOT" bash "$SOURCE_DIR/install_tv.sh"

TV_DIR="$TMP_ROOT/room/TV"
test -x "$TV_DIR/TV.py"
test -x "$TV_DIR/run_tv.sh"
test -x "$TV_DIR/TV.desktop"
test -d "$TMP_ROOT/VCR_Tapes"
grep -Fq "R2 Reality" "$TV_DIR/device.txt"
grep -Fq "$TMP_ROOT/R2/r2_reality.db" "$TV_DIR/device.txt"
grep -Fq "$TV_DIR/TV.py" "$TV_DIR/run_tv.sh"
grep -Fq "Exec=/bin/bash" "$TV_DIR/TV.desktop"
grep -Fq 'sudo -u r2 env -u XAUTHORITY' "$TV_DIR/run_tv.sh"
grep -Fq 'DISPLAY="${DISPLAY:-}"' "$TV_DIR/run_tv.sh"
grep -Fq 'export R2_TV_SOCKET="${R2_TV_SOCKET:-$default_tv_socket}"' "$TV_DIR/run_tv.sh"
grep -Fx "default_tv_socket=$TMP_ROOT/R2/tv-control.sock" "$TV_DIR/run_tv.sh" >/dev/null
test "$(cat "$TV_DIR/.installer-sha256")" = "$(sha256sum < "$SOURCE_DIR/install_tv.sh" | cut -d ' ' -f1)"
grep -Fq 'Terminal=true' "$TV_DIR/TV.desktop"

# Exercise actual generated shell argv with a fake sudo; never touch the host
# r2 account or start the GUI. Shell metacharacters must remain path data.
SPECIAL="$TMP_ROOT/"'tv home & "$` \ 100%'
R2_HOME="$SPECIAL" bash "$SOURCE_DIR/install_tv.sh"
SPECIAL_TV="$SPECIAL/room/TV"
mkdir -p "$TMP_ROOT/mockbin"
cat > "$TMP_ROOT/mockbin/id" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat > "$TMP_ROOT/mockbin/sudo" <<'EOF'
#!/usr/bin/env bash
printf '%s\0' "$@" > "$R2_TV_TEST_ARGV"
EOF
chmod +x "$TMP_ROOT/mockbin/id" "$TMP_ROOT/mockbin/sudo"
(
    unset R2_REALITY_DB R2_TV_SOCKET R2_VCR_MEDIA_DIR
    export PATH="$TMP_ROOT/mockbin:$PATH"
    export R2_TV_TEST_ARGV="$TMP_ROOT/default-argv" DISPLAY=:99
    bash "$SPECIAL_TV/run_tv.sh"
    R2_TV_SOCKET="$TMP_ROOT/override socket" \
    R2_TV_TEST_ARGV="$TMP_ROOT/override-argv" bash "$SPECIAL_TV/run_tv.sh"
)
python3 - "$SPECIAL" "$TMP_ROOT/default-argv" "$TMP_ROOT/override-argv" "$TMP_ROOT/override socket" <<'PY'
import sys
from pathlib import Path
root = sys.argv[1]
for index in (2, 3):
    args = Path(sys.argv[index]).read_bytes().decode().split("\0")[:-1]
    socket = root + "/R2/tv-control.sock" if index == 2 else sys.argv[4]
    assert args == [
        "-u", "r2", "env", "-u", "XAUTHORITY", "HOME=" + root,
        "USER=r2", "LOGNAME=r2", "DISPLAY=:99",
        "R2_REALITY_DB=" + root + "/R2/r2_reality.db",
        "R2_TV_SOCKET=" + socket, "R2_VCR_MEDIA_DIR=" + root + "/VCR_Tapes",
        "/usr/bin/env", "python3", root + "/room/TV/TV.py",
    ], args
raw = (Path(root) / "room/TV/TV.desktop").read_text(encoding="utf-8").splitlines()
path, = [line[5:] for line in raw if line.startswith("Path=")]
assert path.replace("\\s", " ").replace("\\\\", "\\") == root + "/room/TV"
exec_line, = [line[len("Exec=/bin/bash "):] for line in raw if line.startswith("Exec=/bin/bash ")]
# Undo the two backslash escape layers and the literal field-code escape.
token = exec_line.replace("\\\\", "\\")
assert token[0] == token[-1] == '"'
token = token[1:-1]
decoded = ""
i = 0
while i < len(token):
    if token[i] in ("\\", "%"):
        escape = token[i]
        i += 1
        assert token[i] in ('"\\$\x60' if escape == "\\" else "%")
    decoded += token[i]
    i += 1
assert decoded == root + "/room/TV/run_tv.sh"
PY

grep -Fq 'TV_INSTALLER_HASH="$(sha256sum < "$TV_INSTALLER" | cut -d '"'"' '"'"' -f1)"' "$SOURCE_DIR/R2_Launch_Code.sh"
grep -Fq '"$TV_INSTALLER_HASH" != "$INSTALLED_TV_HASH"' "$SOURCE_DIR/R2_Launch_Code.sh"

echo "TV installer smoke test passed."
