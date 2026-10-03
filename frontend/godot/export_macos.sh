#!/bin/sh
# frontend/godot/export_macos.sh — reproducible macOS export.
#
#   ./export_macos.sh OUT.app            # export + thin arm64 + ad-hoc sign
#   ./export_macos.sh OUT.app --zip      # also write OUT.zip next to it
#
# Requires: Godot 4.7.2 ($MDK_GODOT_BIN or /private/tmp/godot47,
# /Applications/Godot.app, PATH) + export templates at
# ~/Library/Application Support/Godot/export_templates/4.7.2.stable/
# and a built bin/Darwin-arm64/libmdkbridge.dylib (build.sh).
#
# The official template ships universal binaries only, so the export
# is universal -> lipo'd to arm64 (the GDExtension is arm64-only) and
# re-signed ad-hoc inside-out. Output layout:
#   OUT.app/Contents/MacOS/MDK            arm64 executable
#   OUT.app/Contents/Frameworks/libmdkbridge.dylib
#   OUT.app/Contents/Resources/MDK.pck
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:?"usage: export_macos.sh OUT.app [--zip]"}
MAKE_ZIP=0
[ "${2:-}" = "--zip" ] && MAKE_ZIP=1

if [ -n "${MDK_GODOT_BIN:-}" ]; then
  GODOT=$MDK_GODOT_BIN
elif [ -x /private/tmp/godot47/Godot.app/Contents/MacOS/Godot ]; then
  GODOT=/private/tmp/godot47/Godot.app/Contents/MacOS/Godot
elif [ -x /Applications/Godot.app/Contents/MacOS/Godot ]; then
  GODOT=/Applications/Godot.app/Contents/MacOS/Godot
elif command -v godot >/dev/null 2>&1; then
  GODOT=$(command -v godot)
else
  echo "export_macos.sh: no Godot 4.7.2 binary found" >&2; exit 1
fi
TPL_DIR="$HOME/Library/Application Support/Godot/export_templates/4.7.2.stable"
[ -f "$TPL_DIR/macos.zip" ] || {
  echo "export_macos.sh: macos.zip template missing in $TPL_DIR" >&2
  exit 1; }
ls "$SCRIPT_DIR"/bin/Darwin-arm64/libmdkbridge.dylib >/dev/null 2>&1 || {
  echo "export_macos.sh: run build.sh first" >&2; exit 1; }

case "$OUT" in *.app) ;; *) OUT=$OUT.app ;; esac
mkdir -p "$(dirname -- "$OUT")"
OUT=$(CDPATH= cd -- "$(dirname -- "$OUT")" && pwd)/$(basename -- "$OUT")
WORK=$(mktemp -d /tmp/mdk_export.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

"$GODOT" --headless --path "$SCRIPT_DIR" \
  --export-release "macOS" "$WORK/MDK.zip"
unzip -o -q "$WORK/MDK.zip" -d "$WORK/x"
# The export names the bundle after application/config/name.
SRC_APP=$(find "$WORK/x" -name "*.app" -maxdepth 1 | head -1)
[ -d "$SRC_APP" ] || { echo "export_macos.sh: no .app in zip" >&2; exit 1; }

rm -rf "$OUT"
mv "$SRC_APP" "$OUT"

# Thin to arm64 — the GDExtension only ships arm64, so the x86_64
# half could never run anyway.
MAIN_EXE="$OUT/Contents/MacOS/MDK"
lipo -thin arm64 "$MAIN_EXE" -o "$MAIN_EXE.thin"
mv "$MAIN_EXE.thin" "$MAIN_EXE"
chmod +x "$MAIN_EXE"

# Inside-out ad-hoc re-sign (lipo invalidated the exporter's sig).
find "$OUT/Contents/Frameworks" -name "*.dylib" -exec \
  codesign --sign - --force {} + 2>/dev/null || true
codesign --sign - --force "$OUT"
codesign --verify --deep --strict "$OUT"
echo "export_macos.sh: signed + verified -> $OUT"

if [ "$MAKE_ZIP" -eq 1 ]; then
  (cd "$(dirname "$OUT")" && ditto -c -k --keepParent \
    "$(basename "$OUT")" "$(basename "$OUT" .app).zip")
  echo "export_macos.sh: zip -> $OUT.zip"
fi
