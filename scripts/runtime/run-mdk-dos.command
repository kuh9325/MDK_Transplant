#!/bin/sh
# run-mdk-dos.command — double-clickable macOS launcher for the MDK DOS lane.
#
# Same launch path as run-mdk-dos.sh, but Finder-double-clickable: opens a
# Terminal window for status, then the game runs in its own DOSBox-X window.
# If the runtime copy is missing it is built from BUILD_A first.
# The full play log is written under runtime-private/logs/.
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
exec "$HERE/run-mdk-dos.sh"
