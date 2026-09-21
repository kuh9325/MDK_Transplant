#!/bin/sh
# setup-runtime.sh — build the writable DOS runtime copy from BUILD_A.
#
# Copies original/installed (BUILD_A, PROVISIONAL_RUNTIME_SOURCE — a
# third-party repack, not verified pristine) into runtime-private/dos/mdk
# and applies documented config changes to the COPY only:
#   * MDK.CFG sound device -> Sound Blaster 16 (HMI id 0xE015, 220/5/1)
#     to match the DOSBox-X [sblaster] sbtype=sb16 emulation in
#     scripts/runtime/dosbox-x-mdkdos.conf.
#   * MDK.CFG controls -> the verified everyday WASD layout (below).
#     BUILD_A's MDK.CFG carries NO Key* lines, so the factory defaults
#     apply (arrows to move, A/Z shared by look AND zoom — a real
#     factory duplicate). Without this block a reset silently reverts
#     to arrow-key movement. The values below are the set verified to
#     drive movement/look/zoom cleanly under DOSBox-X.
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

# Config change (COPY ONLY): write the verified everyday control block.
# BUILD_A's MDK.CFG has NO Key* lines, so MDK falls back to its factory
# table (arrows move; A and Z are shared by look AND zoom). These values
# are MDK internal key codes = DOS set-1 scancodes:
#   W=17 S=31 A=30 D=32   T=20 G=34   R=19 F=33   E=18 Q=16
# so the block below yields  W/S move, A/D strafe, T/G look, R/F zoom,
# E next-item, Q use-item, with no WASD conflict. MouseDButtMapC=0 clears
# the middle-button action that otherwise sits on the zoom/movement path.
#
# MDK.CFG is a DOS file: keep CRLF endings. The awk below normalises every
# line to CRLF (the Sound* sed above strips CR on the lines it rewrites),
# drops any stale control lines for idempotency, then inserts the block so
# the result matches the verified copy: MouseDButtMapC before
# MouseDButtMapD, the Key* block before ForcePCorrect.
if [ -f "$DST/MDK.CFG" ]; then
    awk 'BEGIN{ORS="\r\n"}
        { sub(/\r$/,"") }                            # normalise line ending
        /^MouseDButtMapC/ || /^Key[A-Z]/ { next }    # drop stale control lines
        /^MouseDButtMapD/ && !mb { print "MouseDButtMapC = 0"; mb=1 }
        /^ForcePCorrect/ && !kb {
            print "KeyUp = 17"
            print "KeyDown = 31"
            print "KeyLookUp = 20"
            print "KeyLookDown = 34"
            print "KeyZoomIn = 19"
            print "KeyZoomOut = 33"
            print "KeyItemNext = 18"
            print "KeyItemUse = 16"
            print "KeySideL = 30"
            print "KeySideR = 32"
            kb=1
        }
        { print }
    ' "$DST/MDK.CFG" > "$DST/MDK.CFG.tmp" && mv "$DST/MDK.CFG.tmp" "$DST/MDK.CFG"
fi

# Start each session with no prior-user savegames in the writable copy.
rm -f "$DST"/SAVES/*.SAV 2>/dev/null || true

echo "runtime copy ready: $DST"
