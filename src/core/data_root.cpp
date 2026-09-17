#include "core/data_root.h"

namespace mdk {

std::optional<DataRoot> DataRoot::open(const std::filesystem::path& path,
                                       std::string* error) {
  std::error_code ec;
  const auto status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(status)) {
    if (error) {
      *error = "data path does not exist: " + path.string();
    }
    return std::nullopt;
  }
  if (!std::filesystem::is_directory(status)) {
    if (error) {
      *error = "data path is not a directory: " + path.string();
    }
    return std::nullopt;
  }
  auto canonical = std::filesystem::weakly_canonical(path, ec);
  return DataRoot(ec ? path : canonical);
}

std::filesystem::path
DataRoot::resolve(const std::filesystem::path& rel) const {
  return path_ / rel;
}

} // namespace mdk
