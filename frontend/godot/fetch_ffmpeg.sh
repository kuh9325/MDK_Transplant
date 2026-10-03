#!/bin/sh
# fetch_ffmpeg.sh — stage the minimal FFmpeg 9.0.1 arm64 dylibs the
# GDExtension's MvePlayer links against (Phase 19E).
#
#   ./fetch_ffmpeg.sh [INSTALL_PREFIX]
#
# INSTALL_PREFIX defaults to ~/Developer/ffmpeg-build/install-arm64 —
# the tree produced by building ffmpeg-9.0.1 from source with:
#
#   ./configure --prefix=PREFIX --arch=arm64 --cc=clang \
#     --enable-shared --disable-static --disable-programs \
#     --disable-doc --disable-everything --disable-autodetect \
#     --disable-network --enable-protocol=file \
#     --enable-demuxer=ipmovie --enable-decoder=interplay_video \
#     --enable-decoder=interplay_dpcm
#   make -j && make install            (License: LGPL v2.1+)
#
# Stages into this repo (gitignored build products):
#   gdextension/third_party/ffmpeg/include   public headers
#   gdextension/third_party/ffmpeg/lib       libav*.dylib (@rpath ids)
#   bin/Darwin-arm64/                        same dylibs, for dev runs
#
# install_name rewrites the absolute install paths to @rpath so the
# same staged libs work next to libmdkbridge.dylib in dev and inside
# Contents/Frameworks in the exported .app.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC=${1:-"$HOME/Developer/ffmpeg-build/install-arm64"}
DST="$SCRIPT_DIR/gdextension/third_party/ffmpeg"
BINDIR="$SCRIPT_DIR/bin/Darwin-arm64"

for f in libavcodec libavformat libavutil; do
  [ -f "$SRC/lib/$f.dylib" ] || {
    echo "fetch_ffmpeg: $SRC/lib/$f.dylib missing — build FFmpeg first" >&2
    exit 1; }
done

rm -rf "$DST"
mkdir -p "$DST/include" "$DST/lib" "$BINDIR"
cp -R "$SRC/include/" "$DST/include/"

# Copy the versioned real files under their soname names.
for f in libavcodec libavformat libavutil; do
  real=$(cd "$SRC/lib" && readlink "$f.dylib" || echo "$f.dylib")
  case "$real" in
    "") real="$f.dylib" ;;
  esac
  soname=$(otool -D "$SRC/lib/$real" | tail -1 | xargs basename)
  cp "$SRC/lib/$real" "$DST/lib/$soname"
  # self-id -> @rpath, then same for every av* dependency.
  install_name_tool -id "@rpath/$soname" "$DST/lib/$soname"
  for dep in $(otool -L "$DST/lib/$soname" | awk '{print $1}' |
               grep "libav.*\.dylib" || true); do
    depname=$(basename "$dep")
    install_name_tool -change "$dep" "@rpath/$depname" \
      "$DST/lib/$soname"
  done
  cp "$DST/lib/$soname" "$BINDIR/$soname"
done

# Verify the rewrite produced relocatable deps.
for so in "$DST"/lib/libav*.dylib "$BINDIR"/libav*.dylib; do
  if otool -L "$so" | tail -n +2 | \
      grep -q "$HOME\|/usr/local\|/opt/homebrew"; then
    echo "fetch_ffmpeg: $so still has an absolute dependency" >&2
    otool -L "$so" >&2
    exit 1
  fi
done

echo "staged:"; ls -la "$DST/lib" "$BINDIR"/libav*.dylib
