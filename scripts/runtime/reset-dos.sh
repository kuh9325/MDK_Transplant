#!/bin/sh
# reset-dos.sh — return the DOS runtime lane to its known initial state by
# deleting the writable copy and rebuilding it from BUILD_A.
#
# Persistent between runs otherwise: everything in runtime-private/dos/mdk
# (MDK.CFG edits, SAVES/* written by the game, captures dir contents).
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
DST="$ROOT/runtime-private/dos/mdk"

if [ -d "$DST" ]; then
    rm -rf "$DST"
    echo "removed $DST"
fi

"$ROOT/scripts/runtime/setup-runtime.sh"
