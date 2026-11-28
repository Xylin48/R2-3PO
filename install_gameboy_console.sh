#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
TARGET_DIR="${R2_GAMEBOY_DIR:-/home/x/R2_Home/Devices/GameBoyAdvance}"
POCKET_DIR="${R2_POCKET_DIR:-/home/x/R2_Home/Pockets}"
DESKTOP_DIR="${R2_DESKTOP_DIR:-/home/x/.local/share/applications}"
MGBA_PATH="${R2_MGBA_EXECUTABLE:-/usr/games/mgba-qt}"

# A launcher must use an absolute executable path even if installation was
# requested relative to the invoking shell's working directory.
TARGET_DIR="$(python3 - "$TARGET_DIR" <<'PY'
import sys
from pathlib import Path
target = str(Path(sys.argv[1]).resolve())
# Validate before command substitution can strip a trailing newline.
if "=" in target or any(not 32 <= ord(ch) <= 126 for ch in target):
    raise SystemExit("Game Boy launcher path cannot be represented in a Desktop Entry Exec executable.")
print(target)
PY
)"
LAUNCHER_TMP="$(mktemp)"
trap 'rm -f -- "$LAUNCHER_TMP"' EXIT
# Render the desktop file before touching the installed device. Desktop Entry
# string escaping and Exec-token quoting are separate from shell/sed syntax.
python3 - "$SOURCE_DIR/GameBoyAdvance.desktop" "$TARGET_DIR/GameBoyAdvance" "$LAUNCHER_TMP" <<'PY'
import sys
from pathlib import Path
template, executable, output = sys.argv[1:]
if "=" in executable or any(not 32 <= ord(ch) <= 126 for ch in executable):
    raise SystemExit("Game Boy launcher path cannot be represented in a Desktop Entry Exec executable.")
reserved = " \"'\\><~|&;$*?#()\x60"
value = executable.replace("%", "%%")
if any(ch in reserved for ch in executable):
    value = '"' + "".join("\\" + ch if ch in '"\\$\x60' else ch for ch in value) + '"'
    value = value.replace("\\", "\\\\")
lines = Path(template).read_text(encoding="utf-8").splitlines()
if sum(line.startswith("Exec=") for line in lines) != 1:
    raise SystemExit("Game Boy desktop template must have exactly one Exec entry.")
Path(output).write_text("\n".join("Exec=" + value if line.startswith("Exec=") else line
                                  for line in lines) + "\n", encoding="utf-8")
PY

mkdir -p -- "$TARGET_DIR/Cartridges" "$TARGET_DIR/Saves" "$TARGET_DIR/State" "$POCKET_DIR" "$DESKTOP_DIR"
# Catch syntax problems before replacing the installed console executable.
python3 -m py_compile "$SOURCE_DIR/GameBoyAdvance.py"
install -m 0755 -- "$SOURCE_DIR/GameBoyAdvance.py" "$TARGET_DIR/GameBoyAdvance"
test -x "$TARGET_DIR/GameBoyAdvance"

# Keep identical desktop-entry copies in the pocket and application directory.
install -m 0644 -- "$LAUNCHER_TMP" "$DESKTOP_DIR/GameBoyAdvance.desktop"
install -m 0644 -- "$LAUNCHER_TMP" "$POCKET_DIR/GameBoyAdvance.desktop"
cmp -s -- "$LAUNCHER_TMP" "$DESKTOP_DIR/GameBoyAdvance.desktop"
cmp -s -- "$LAUNCHER_TMP" "$POCKET_DIR/GameBoyAdvance.desktop"

# Verify the actual emulator path without silently substituting another binary.
if [[ -x "$MGBA_PATH" ]]; then
  echo "Verified executable mGBA path: $MGBA_PATH"
else
  echo "WARNING: mGBA is not executable at $MGBA_PATH."
  echo "Set R2_MGBA_EXECUTABLE to the installed mGBA executable before powering on."
fi

# R2 is launched under the separate Linux account "r2". Give that account
# write access to device state/saves and read access to the pocket launcher.
if getent group r2 >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
  if sudo chgrp -R r2 "$TARGET_DIR"; then
    sudo find "$TARGET_DIR" -type d -exec chmod 2775 {} +
    sudo find "$TARGET_DIR" -type f -exec chmod g+rw {} +
    sudo chgrp r2 "$POCKET_DIR/GameBoyAdvance.desktop"
    sudo chmod 0644 "$POCKET_DIR/GameBoyAdvance.desktop"
  else
    echo "WARNING: group permissions were not changed."
    echo "If R2 runs as Linux user r2, grant that account write access to $TARGET_DIR."
  fi
else
  echo "WARNING: could not configure the r2 group automatically."
  echo "If R2 runs as a separate Linux account, grant it access to the device directory."
fi

cat <<EOF
Installed R2's Game Boy Advance console:
  Console executable: $TARGET_DIR/GameBoyAdvance
  Cartridge slot DB:  $TARGET_DIR/gameboy.db (created on first launch)
  Loose cartridges:  $TARGET_DIR/Cartridges/
  Normal game saves: $TARGET_DIR/Saves/
  Runtime/config:    $TARGET_DIR/State/
  Pocket launcher:   $POCKET_DIR/GameBoyAdvance.desktop
  Desktop launcher:  $DESKTOP_DIR/GameBoyAdvance.desktop

No cartridge is assumed to be owned or inserted. Once mGBA is installed,
verify the configured path with:
EOF
printf '  R2_MGBA_EXECUTABLE=%q %q --json verify\n' "$MGBA_PATH" "$TARGET_DIR/GameBoyAdvance"
printf 'The external emulator remains at %s by default.\n' "$MGBA_PATH"
