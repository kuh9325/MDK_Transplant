#!/bin/sh
# Phase 7 (G1) — configure + build the mdk_bridge GDExtension.
#
# Usage:
#   ./build.sh                 # FetchContent godot-cpp @ pinned commit
#   GODOT_CPP_DIR=/path/to/godot-cpp ./build.sh   # reuse a checkout
#
# Output: frontend/godot/bin/Darwin-arm64/libmdkbridge.dylib
set -eu

cd "$(dirname "$0")/gdextension"

EXTRA=""
if [ -n "${GODOT_CPP_DIR:-}" ]; then
  EXTRA="-DGODOT_CPP_DIR=$GODOT_CPP_DIR"
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release $EXTRA
cmake --build build --parallel --target mdkbridge mdk_frontend_tests
echo "built: $(ls ../bin/*/libmdkbridge.dylib)"

# Godot game mode discovers GDExtensions via .godot/extension_list.cfg,
# a file normally produced by an EDITOR filesystem scan. Writing the
# entry here keeps the whole runtime path editor-free; a later real
# editor scan rewrites/extends the same file harmlessly.
mkdir -p ../.godot
if ! grep -qs 'res://gdextension/mdk_bridge.gdextension' \
    ../.godot/extension_list.cfg 2>/dev/null; then
  printf 'res://gdextension/mdk_bridge.gdextension\n' \
    >> ../.godot/extension_list.cfg
fi
