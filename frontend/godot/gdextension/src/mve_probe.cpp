// mve_probe — standalone MvePlayer verification harness.
// Usage: mve_probe FILE.mve [--dump first|mid|last.ppm]
// Reports streams, frame/audio counts, timing, errors; optionally
// dumps representative frames as PPM for visual diffing.

#include "mve_player.h"

#include <cstdio>
#include <cstring>
#include <string>

using mdkbridge::MvePlayer;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s FILE.mve [--dump PREFIX]\n",
                 argv[0]);
    return 2;
  }
  const char* path = argv[1];
  std::string prefix;
  for (int i = 2; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--dump") == 0) prefix = argv[i + 1];

  MvePlayer p;
  std::string err;
  if (!p.open(path, &err)) {
    std::fprintf(stderr, "open failed: %s\n", err.c_str());
    return 1;
  }
  std::printf("video: %dx%d packets=%lld\n", p.width(), p.height(),
              (long long)p.videoPackets());
  std::printf("audio: %dHz x%d ch, %.2fs, packets=%lld errs=%lld\n",
              p.audioRate(), p.audioChannels(), p.audioDurationSec(),
              (long long)p.audioPackets(),
              (long long)p.audioDecodeErrors());

  std::vector<std::uint8_t> idx;
  std::array<std::uint8_t, 768> pal{};
  bool palDirty = false;
  double pts = 0;
  double firstPts = -1, prevPts = 0;
  double minDelta = 1e9, maxDelta = 0;
  int64_t n = 0;
  int palChanges = 0;
  const int64_t total = p.videoPackets();
  while (p.nextVideoFrame(&idx, &pal, &palDirty, &pts, &err)) {
    if (palDirty) ++palChanges;
    if (n == 0) firstPts = pts;
    if (n > 0) {
      const double d = pts - prevPts;
      if (d < minDelta) minDelta = d;
      if (d > maxDelta) maxDelta = d;
    }
    prevPts = pts;
    if (!prefix.empty() &&
        (n == 0 || n == total / 2 || n == total - 1)) {
      char fn[512];
      std::snprintf(fn, sizeof(fn), "%s_%lld.ppm", prefix.c_str(),
                    (long long)n);
      FILE* f = std::fopen(fn, "wb");
      if (f) {
        std::fprintf(f, "P6\n%d %d\n255\n", p.width(), p.height());
        for (std::size_t i = 0; i < idx.size(); ++i)
          std::fwrite(&pal[idx[i] * 3], 3, 1, f);
        std::fclose(f);
      }
    }
    ++n;
  }
  std::printf(
      "decoded: %lld frames, pts %.1f..%.1f ms (delta %.1f..%.1f), "
      "pal_changes=%d, verr=%lld, eof=%d\n",
      (long long)n, firstPts, prevPts, minDelta, maxDelta,
      palChanges, (long long)p.videoDecodeErrors(),
      p.eof() ? 1 : 0);
  if (!err.empty()) std::printf("last-err: %s\n", err.c_str());
  return (n > 0 && p.eof()) ? 0 : 1;
}
