#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT

cat > "$TEMP_DIR/fake-mgba" <<'EOF'
#!/usr/bin/env sh
exit 0
EOF
chmod 0755 "$TEMP_DIR/fake-mgba"

R2_GAMEBOY_DIR="$TEMP_DIR/device" \
R2_POCKET_DIR="$TEMP_DIR/Pockets" \
R2_DESKTOP_DIR="$TEMP_DIR/applications" \
R2_MGBA_EXECUTABLE="$TEMP_DIR/fake-mgba" \
  bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/install.log" 2>&1

test -x "$TEMP_DIR/device/GameBoyAdvance"
test -d "$TEMP_DIR/device/Cartridges"
test -d "$TEMP_DIR/device/Saves"
test -d "$TEMP_DIR/device/State"
test -f "$TEMP_DIR/applications/GameBoyAdvance.desktop"
test -f "$TEMP_DIR/Pockets/GameBoyAdvance.desktop"
grep -Fx "Exec=$TEMP_DIR/device/GameBoyAdvance" \
  "$TEMP_DIR/applications/GameBoyAdvance.desktop" >/dev/null
grep -Fx "Exec=$TEMP_DIR/device/GameBoyAdvance" \
  "$TEMP_DIR/Pockets/GameBoyAdvance.desktop" >/dev/null
grep -F "Verified executable mGBA path: $TEMP_DIR/fake-mgba" \
  "$TEMP_DIR/install.log" >/dev/null

# The custom device path is a single executable token, not sed replacement
# text, multiple desktop arguments, field codes, or shell interpolation.
SPECIAL="$TEMP_DIR/"'device & | "$` \ 100%'
R2_GAMEBOY_DIR="$SPECIAL" \
R2_POCKET_DIR="$TEMP_DIR/special-pockets" \
R2_DESKTOP_DIR="$TEMP_DIR/special-apps" \
R2_MGBA_EXECUTABLE="$TEMP_DIR/fake-mgba" \
  bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/special.log" 2>&1
test -x "$SPECIAL/GameBoyAdvance"
cmp -s "$TEMP_DIR/special-apps/GameBoyAdvance.desktop" "$TEMP_DIR/special-pockets/GameBoyAdvance.desktop"
python3 - "$TEMP_DIR/special-apps/GameBoyAdvance.desktop" "$SPECIAL/GameBoyAdvance" <<'PY'
import sys
from pathlib import Path
lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
raw, = [line[5:] for line in lines if line.startswith("Exec=")]
# Independently undo the desktop string escape layer, then quoted-token
# escapes, then the field-code escape. No shell eval is involved.
unescaped = ""
i = 0
while i < len(raw):
    if raw[i] == "\\":
        assert raw[i + 1] == "\\"
        i += 1
    unescaped += raw[i]
    i += 1
assert unescaped[0] == unescaped[-1] == '"'
token = unescaped[1:-1]
decoded = ""
i = 0
while i < len(token):
    if token[i] == "\\":
        i += 1
        assert token[i] in '"\\$\x60'
    elif token[i] == "%":
        i += 1
        assert token[i] == "%"
    else:
        assert token[i] != '"'
    decoded += token[i]
    i += 1
assert decoded == sys.argv[2], (decoded, sys.argv[2])
PY

# Relative installation paths must produce an absolute launcher.
(
    cd "$TEMP_DIR"
    R2_GAMEBOY_DIR="relative-device" \
    R2_POCKET_DIR="$TEMP_DIR/relative-pockets" \
    R2_DESKTOP_DIR="$TEMP_DIR/relative-apps" \
    R2_MGBA_EXECUTABLE="$TEMP_DIR/fake-mgba" \
      bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/relative.log" 2>&1
)
grep -Fx "Exec=$TEMP_DIR/relative-device/GameBoyAdvance" \
  "$TEMP_DIR/relative-apps/GameBoyAdvance.desktop" >/dev/null

# '=' is not legal in a Desktop Entry executable path. Reject the invalid
# installation before replacing an existing launcher or creating a device.
if R2_GAMEBOY_DIR="$TEMP_DIR/bad=device" \
   R2_POCKET_DIR="$TEMP_DIR/Pockets" \
   R2_DESKTOP_DIR="$TEMP_DIR/applications" \
   bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/rejected.log" 2>&1; then
    echo "Invalid launcher path unexpectedly accepted" >&2
    exit 1
fi
test ! -e "$TEMP_DIR/bad=device"
BAD_NEWLINE="$TEMP_DIR/"$'bad-newline\n'
if R2_GAMEBOY_DIR="$BAD_NEWLINE" \
   R2_POCKET_DIR="$TEMP_DIR/Pockets" \
   R2_DESKTOP_DIR="$TEMP_DIR/applications" \
   bash "$REPO_DIR/install_gameboy_console.sh" >"$TEMP_DIR/rejected-newline.log" 2>&1; then
    echo "Trailing newline path unexpectedly accepted" >&2
    exit 1
fi
test ! -e "$BAD_NEWLINE"
test ! -e "$TEMP_DIR/bad-newline"
grep -Fx "Exec=$TEMP_DIR/device/GameBoyAdvance" \
  "$TEMP_DIR/applications/GameBoyAdvance.desktop" >/dev/null
echo "Game Boy installer, pocket launcher, and desktop-entry smoke test passed."
