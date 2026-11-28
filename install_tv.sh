#!/usr/bin/env bash
set -euo pipefail

SOURCE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
R2_HOME="${R2_HOME:-/home/x/R2_Home}"
TV_DIR="${R2_TV_DIR:-$R2_HOME/room/TV}"
MEDIA_DIR="${R2_VCR_MEDIA_DIR:-$R2_HOME/VCR_Tapes}"

if [[ ! -f "$SOURCE_DIR/TV.py" ]]; then
    echo "TV.py was not found beside this installer." >&2
    exit 1
fi

# Defaults are installation-time paths; normalize them before generating
# launchers, whose working directory will differ from the invoking shell.
absolute_device_path() {
    python3 - "$1" <<'PY'
import sys
from pathlib import Path
path = str(Path(sys.argv[1]).resolve())
if any(ord(ch) < 32 or ord(ch) == 127 for ch in path):
    raise SystemExit("TV device paths must not contain control characters.")
print(path)
PY
}
R2_HOME="$(absolute_device_path "$R2_HOME")"
TV_DIR="$(absolute_device_path "$TV_DIR")"
MEDIA_DIR="$(absolute_device_path "$MEDIA_DIR")"

DESKTOP_TMP="$(mktemp)"
trap 'rm -f -- "$DESKTOP_TMP"' EXIT
python3 - "$TV_DIR" "$DESKTOP_TMP" <<'PY'
import sys
from pathlib import Path
root, output = sys.argv[1:]
if any(not 32 <= ord(ch) <= 126 for ch in root):
    raise SystemExit("TV launcher path cannot be represented in a Desktop Entry string.")
script = root + "/run_tv.sh"
# Desktop Entry general string escaping precedes Exec-token quoting.
token = script.replace("%", "%%")
token = '"' + "".join("\\" + ch if ch in '"\\$\x60' else ch for ch in token) + '"'
token = token.replace("\\", "\\\\")
working = root.replace("\\", "\\\\").replace(" ", "\\s")
Path(output).write_text(
    "[Desktop Entry]\nType=Application\nName=R2's CRT Television\n"
    "Comment=Operate the CRT television and built-in VCR in R2's room\n"
    f"Exec=/bin/bash {token}\nPath={working}\nTerminal=true\nCategories=AudioVideo;Video;\n",
    encoding="utf-8",
)
PY

mkdir -p -- "$TV_DIR" "$MEDIA_DIR"
install -m 0755 -- "$SOURCE_DIR/TV.py" "$TV_DIR/TV.py"

# Serialize literal defaults as Bash assignments, not interpolated source
# inside parameter expansions. Runtime overrides still win.
{
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail'
    printf 'device_home=%q\n' "$R2_HOME"
    printf 'default_reality_db=%q\n' "$R2_HOME/R2/r2_reality.db"
    printf 'default_tv_socket=%q\n' "$R2_HOME/R2/tv-control.sock"
    printf 'default_media_dir=%q\n' "$MEDIA_DIR"
    printf 'tv_program=%q\n' "$TV_DIR/TV.py"
    cat <<'EOF'
export R2_REALITY_DB="${R2_REALITY_DB:-$default_reality_db}"
export R2_TV_SOCKET="${R2_TV_SOCKET:-$default_tv_socket}"
export R2_VCR_MEDIA_DIR="${R2_VCR_MEDIA_DIR:-$default_media_dir}"
# The R2 shell owns the private Reality database and mode-0600 control socket.
# Run the GUI as that same account; R2_Launch_Code.sh grants r2 access to the
# current X display while R2 is running. Keep the terminal attached so sudo
# can request authentication if its credential cache has expired.
if ! id r2 >/dev/null 2>&1; then
    echo "Linux user 'r2' does not exist; start R2's normal setup first." >&2
    exit 1
fi
exec sudo -u r2 env -u XAUTHORITY \
    HOME="$device_home" USER=r2 LOGNAME=r2 \
    DISPLAY="${DISPLAY:-}" \
    R2_REALITY_DB="$R2_REALITY_DB" \
    R2_TV_SOCKET="$R2_TV_SOCKET" \
    R2_VCR_MEDIA_DIR="$R2_VCR_MEDIA_DIR" \
    /usr/bin/env python3 "$tv_program"
EOF
} > "$TV_DIR/run_tv.sh"
bash -n "$TV_DIR/run_tv.sh"
chmod 0755 "$TV_DIR/run_tv.sh"
install -m 0755 -- "$DESKTOP_TMP" "$TV_DIR/TV.desktop"

cat > "$TV_DIR/device.txt" <<EOF
Object: TV
Location: Room
Type: CRT television with built-in VCR
Reality database: $R2_HOME/R2/r2_reality.db
Control socket: $R2_HOME/R2/tv-control.sock
VCR media directory: $MEDIA_DIR
State authority: R2 Reality; this file is descriptive metadata only.
EOF

# Publish the installer identity only after all generated files are complete.
# The interactive launcher compares it as well as the TV.py source.
sha256sum < "$SOURCE_DIR/install_tv.sh" | cut -d ' ' -f1 > "$TV_DIR/.installer-sha256"

echo "Installed the TV device files in: $TV_DIR"
echo "VCR media directory: $MEDIA_DIR"
echo "The R2 shell must be running before the GUI can change TV state."
echo "Open TV.desktop from R2's room to use the device."
