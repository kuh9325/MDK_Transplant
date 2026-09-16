#!/bin/sh
# setup-runtime.sh — build the writable DOS runtime copy from BUILD_A.
#
# Copies original/installed (BUILD_A, PROVISIONAL_RUNTIME_SOURCE — a
# third-party repack, not verified pristine) into runtime-private/dos/mdk
# and applies a documented config change to the COPY only:
#   MDK.CFG sound device -> Sound Blaster 16 (HMI id 0xE015, 220/5/1)
# to match the DOSBox-X [sblaster] sbtype=sb16 emulation in
# scripts/runtime/dosbox-x-mdkdos.conf.
#
# BUILD_A under original/ is never modified.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
SRC="$ROOT/original/installed"
DST="$ROOT/runtime-private/dos/mdk"

if [ ! -d "$SRC" ]; then
    echo "error: $SRC not found — supply BUILD_A first" >&2
    exit 1
fi

if [ -d "$DST" ]; then
    echo "runtime copy already exists at $DST"
    echo "use scripts/runtime/reset-dos.sh to rebuild from scratch"
    exit 0
fi

mkdir -p "$DST"
cp -R "$SRC"/. "$DST"/

# Config change (COPY ONLY): prior user's MDK.CFG selected HMI device
# 0xE024 @ 240/5/3; point it at the SB16 that DOSBox-X emulates.
if [ -f "$DST/MDK.CFG" ]; then
    sed -i '' \
        -e 's/^SoundID = .*/SoundID = 0xE015/' \
        -e 's/^SoundIRQ = .*/SoundIRQ = 5/' \
        -e 's/^SoundDMA = .*/SoundDMA = 1/' \
        -e 's/^SoundPort = .*/SoundPort = 0x220/' \
        "$DST/MDK.CFG"
fi

# Start each session with no prior-user savegames in the writable copy.
rm -f "$DST"/SAVES/*.SAV 2>/dev/null || true

echo "runtime copy ready: $DST"
