#!/bin/sh
# frontend/godot/run.sh — canonical Godot-frontend launcher.
#
#   ./run.sh                  interactive HMO_1 view (WASD, Esc quits)
#   ./run.sh --smoke          headless deterministic smoke
#   ./run.sh --screenshot P   real-renderer frame capture -> P.png
#   ./run.sh --frames N       run N frames then exit 0 (startup proof)
#
# Anything after the mode is forwarded as project user args
# (--data-path DIR, --level REL, --arena NAME).
#
# Godot resolution order: $MDK_GODOT_BIN, `godot` on PATH,
# /Applications/Godot.app. The frontend never needs an editor pass —
# the extension list is written here if absent.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)

# --- locate a Godot 4.7.x binary ---
if [ -n "${MDK_GODOT_BIN:-}" ]; then
  GODOT=$MDK_GODOT_BIN
elif command -v godot >/dev/null 2>&1; then
  GODOT=$(command -v godot)
elif [ -x /Applications/Godot.app/Contents/MacOS/Godot ]; then
  GODOT=/Applications/Godot.app/Contents/MacOS/Godot
else
  cat >&2 <<'EOF'
run.sh: Godot 4.7.x not found.
Install the official macOS build to /Applications/Godot.app, put
`godot` on PATH, or set MDK_GODOT_BIN=/path/to/Godot.
EOF
  exit 1
fi
if [ ! -x "$GODOT" ]; then
  echo "run.sh: $GODOT is not executable" >&2
  exit 1
fi

# --- extension built? ---
if ! ls "$SCRIPT_DIR"/bin/*/libmdkbridge.dylib >/dev/null 2>&1; then
  echo "run.sh: GDExtension missing — run frontend/godot/build.sh" >&2
  exit 1
fi

# --- extension discoverable without an editor scan ---
# Game mode only loads GDExtensions listed in
# .godot/extension_list.cfg (an editor artifact). Seed it if absent.
mkdir -p "$SCRIPT_DIR/.godot"
if ! grep -qs 'res://gdextension/mdk_bridge.gdextension' \
    "$SCRIPT_DIR/.godot/extension_list.cfg" 2>/dev/null; then
  printf 'res://gdextension/mdk_bridge.gdextension\n' \
    >> "$SCRIPT_DIR/.godot/extension_list.cfg"
fi

# --- engine args by mode ---
ENGINE_ARGS=""
case "${1:-}" in
  --smoke)
    # Dummy renderer + dummy audio: the audit-proven headless path.
    # MdkBridge still loads; rendering is not exercised.
    ENGINE_ARGS="--headless --rendering-driver dummy --audio-driver Dummy"
    ;;
esac

# --- default data path (repo-local ignored drop-zone) ---
HAS_DATA=0
for a in "$@"; do
  if [ "$a" = "--data-path" ]; then
    HAS_DATA=1
  fi
done
if [ "$HAS_DATA" -eq 0 ]; then
  DATA_ROOT=${MDK_DATA_ROOT:-$REPO_ROOT/original/installed}
  set -- "$@" --data-path "$DATA_ROOT"
fi

exec "$GODOT" $ENGINE_ARGS --path "$SCRIPT_DIR" -- "$@"
