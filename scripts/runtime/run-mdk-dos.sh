#!/bin/sh
# run-mdk-dos.sh — launch the MDK DOS lane (MDKDOS.EXE under DOSBox-X).
#
# Expects the runtime copy built by setup-runtime.sh. DOSBox-X resolves
# relative paths in the conf against its working directory, so this script
# must run from the repository root — it enforces that itself.
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$ROOT"

if ! command -v dosbox-x >/dev/null 2>&1; then
    echo "error: dosbox-x not found on PATH" >&2
    echo "install the native ARM64 build, e.g.: brew install dosbox-x" >&2
    exit 1
fi

if [ ! -f "$ROOT/runtime-private/dos/mdk/MDKDOS.EXE" ]; then
    echo "runtime copy missing — running setup first" >&2
    "$ROOT/scripts/runtime/setup-runtime.sh"
fi

LOG="$ROOT/runtime-private/logs/dosbox-x-$(date +%Y%m%d-%H%M%S).log"
mkdir -p "$ROOT/runtime-private/logs"
echo "logging to $LOG"
exec dosbox-x -conf "$ROOT/scripts/runtime/dosbox-x-mdkdos.conf" \
    -defaultdir "$ROOT" >"$LOG" 2>&1
