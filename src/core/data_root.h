// Data-root seam for future original-data access.
//
// ORIGINAL ENGINE OBSERVATION: the original resolves game data through
// cddata/hddata roots via mdkfopen (Phase 2B). This class is only the
// native-side seam — it does NOT parse any proprietary file.
//
// NATIVE PORT PROJECT DECISION: a single user-supplied data root
// (--data-path). The directory is treated as strictly READ-ONLY: nothing
// in this codebase writes to it, and nothing is copied into the app
// bundle.

#ifndef MDK_CORE_DATA_ROOT_H
#define MDK_CORE_DATA_ROOT_H

#include <filesystem>
#include <optional>
#include <string>

namespace mdk {

class DataRoot {
public:
  // Validate `path` as a data root. Returns a DataRoot on success; on
  // failure returns nullopt and fills `error` if non-null.
  static std::optional<DataRoot> open(const std::filesystem::path& path,
                                      std::string* error = nullptr);

  const std::filesystem::path& path() const { return path_; }

  // Resolve a path relative to the root. Existence is NOT checked —
  // consumers check what they actually need.
  std::filesystem::path resolve(const std::filesystem::path& rel) const;

private:
  explicit DataRoot(std::filesystem::path path) : path_(std::move(path)) {}
  std::filesystem::path path_;
};

} // namespace mdk

#endif // MDK_CORE_DATA_ROOT_H
