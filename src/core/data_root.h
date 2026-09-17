// Read-only data-root seam for original-data access.
//
// ORIGINAL ENGINE OBSERVATION: the original resolves game data through
// cddata/hddata roots via mdkfopen.c (Phase 2B: FUN_0041ae50 copies
// cddata->hddata; FUN_0041ad14/FUN_0041adb8 join a configured root to
// DOS-style relative paths). Resource strings are DOS-style, relative,
// and the original ran on case-insensitive filesystems.
//
// NATIVE PORT PROJECT DECISIONS:
//   - a single user-supplied data root (--data-path); the original's
//     CD->HD copy-up mechanism is NOT reproduced (this layer never
//     writes to the data root — strictly read-only).
//   - '/' and '\' are both accepted as separators, matching the
//     original's mixed usage.
//   - components are matched case-insensitively (ASCII fold) via a
//     per-directory index, so resolution does not depend on the host
//     filesystem being case-insensitive.
//   - requests cannot escape the root: '..' components, absolute paths,
//     drive-letter paths, and ':' are rejected, and the resolved
//     candidate is canonicalized and verified to remain inside the
//     root (symlink escapes rejected).

#ifndef MDK_CORE_DATA_ROOT_H
#define MDK_CORE_DATA_ROOT_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mdk {

class DataRoot {
public:
  // Validate `path` as a data root. Returns a DataRoot on success; on
  // failure returns nullopt and fills `error` if non-null.
  static std::optional<DataRoot> open(const std::filesystem::path& path,
                                      std::string* error = nullptr);

  const std::filesystem::path& path() const { return path_; }

  // Resolve an MDK-relative resource request to a real on-disk path.
  // The target must exist. On failure returns nullopt and fills
  // `error` if non-null. The returned path is canonical.
  std::optional<std::filesystem::path> resolve(const std::string& rel,
                                               std::string* error = nullptr) const;

  // Size of a resolved regular file.
  std::optional<std::uintmax_t> fileSize(const std::string& rel,
                                         std::string* error = nullptr) const;

  // Bounded whole-file read (fails if the file exceeds `maxBytes`).
  // Opens strictly read-only; never writes to the data root.
  std::optional<std::vector<std::byte>> readFile(const std::string& rel,
                                                 std::size_t maxBytes,
                                                 std::string* error = nullptr) const;

  // Bounded prefix read: at most `n` bytes from the start of the file.
  std::optional<std::vector<std::byte>> readPrefix(const std::string& rel,
                                                   std::size_t n,
                                                   std::string* error = nullptr) const;

private:
  explicit DataRoot(std::filesystem::path path) : path_(std::move(path)) {}

  struct DirIndex {
    // ASCII-folded component name -> actual on-disk name.
    std::unordered_map<std::string, std::string> byFolded;
    // Folded names claimed by more than one distinct entry.
    std::unordered_set<std::string> ambiguous;
  };

  // Lazily-built per-directory index; nullopt when the directory
  // cannot be listed (missing/not-a-directory). Negative results are
  // cached for the life of the DataRoot.
  const DirIndex* dirIndex(const std::filesystem::path& dir) const;

  std::filesystem::path path_; // canonical
  mutable std::unordered_map<std::string, std::optional<DirIndex>> dirs_;
};

} // namespace mdk

#endif // MDK_CORE_DATA_ROOT_H
