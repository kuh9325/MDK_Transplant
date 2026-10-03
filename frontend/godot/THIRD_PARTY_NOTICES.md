# Third-Party Notices — MDK.app

## FFmpeg (interplay MVE playback, Phase 19E)

`Contents/Frameworks/libavcodec.63.dylib`,
`Contents/Frameworks/libavformat.63.dylib`, and
`Contents/Frameworks/libavutil.61.dylib` are unmodified builds of
**FFmpeg 9.0.1**, used to demux and decode the original game's
`MISC/FLIC/MDKBZK.MVE` (Interplay MVE video + DPCM audio).

- License: **GNU Lesser General Public License v2.1 or later**
  (<https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>).
  The FFmpeg libraries are dynamically linked; the rest of the
  application is not a derived work of FFmpeg.
- Copyright © 2000–2026 the FFmpeg developers.
- Source: <https://ffmpeg.org/download.html> (release tarball
  `ffmpeg-9.0.1.tar.xz`).
- No FFmpeg component was modified. The libraries are loaded as
  separate dylibs and may be replaced with a compatible LGPL build.
- Build configuration (reproducible via `fetch_ffmpeg.sh`):

      ./configure --arch=arm64 --cc=clang --enable-shared \
        --disable-static --disable-programs --disable-doc \
        --disable-everything --disable-autodetect \
        --disable-network --enable-protocol=file \
        --enable-demuxer=ipmovie \
        --enable-decoder=interplay_video \
        --enable-decoder=interplay_dpcm
      make -j && make install

  Enabled components: `ipmovie` demuxer, `interplay_video` and
  `interplay_dpcm` decoders, `file` protocol. No encoders, filters,
  muxers, network, or hardware acceleration are included.

FFmpeg itself incorporates code licensed under several other
permissive and copyleft licenses; see `LICENSE.md` inside the
FFmpeg source distribution for the complete list.

## Godot Engine

The application shell is exported from **Godot Engine 4.7.2**
(MIT License, <https://godotengine.org/license>).
