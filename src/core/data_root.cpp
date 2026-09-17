#include "core/data_root.h"

#include <algorithm>
#include <fstream>
#include <system_error>

namespace mdk {

namespace {

// ASCII-only case fold: the original ran on DOS/Win95 case-insensitive
// filesystems (uppercase 8.3 names). Non-ASCII bytes compare exactly.
std::string foldAscii(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

bool isWithin(const std::filesystem::path& canon,
              const std::filesystem::path& root) {
  const auto rel = canon.lexically_relative(root);
  if (rel.empty()) {
    return false;
  }
  return *rel.begin() != "..";
}

} // namespace

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
  if (ec) {
    if (error) {
      *error = "cannot canonicalize data path: " + path.string();
    }
    return std::nullopt;
  }
  return DataRoot(canonical);
}

const DataRoot::DirIndex*
DataRoot::dirIndex(const std::filesystem::path& dir) const {
  const auto key = dir.string();
  if (const auto it = dirs_.find(key); it != dirs_.end()) {
    return it->second ? &*it->second : nullptr;
  }

  std::optional<DirIndex> idx;
  std::error_code ec;
  std::filesystem::directory_iterator it(dir, ec);
  if (!ec) {
    idx.emplace();
    for (const auto& entry : it) {
      const std::string name = entry.path().filename().string();
      const std::string folded = foldAscii(name);
      if (idx->ambiguous.count(folded)) {
        continue;
      }
      auto [pos, inserted] = idx->byFolded.emplace(folded, name);
      if (!inserted && pos->second != name) {
        // Two distinct on-disk entries collide under ASCII folding.
        idx->byFolded.erase(pos);
        idx->ambiguous.insert(folded);
      }
    }
  }
  auto [slot, _] = dirs_.emplace(key, std::move(idx));
  (void)_;
  return slot->second ? &*slot->second : nullptr;
}

std::optional<std::filesystem::path>
DataRoot::resolve(const std::string& rel, std::string* error) const {
  auto fail = [&](const char* msg) -> std::optional<std::filesystem::path> {
    if (error) {
      *error = std::string(msg) + ": " + rel;
    }
    return std::nullopt;
  };

  if (rel.empty()) {
    return fail("empty resource path");
  }
  if (rel.front() == '/' || rel.front() == '\\') {
    return fail("absolute resource path rejected");
  }
  if (rel.find(':') != std::string::npos || rel.find('\0') != std::string::npos) {
    return fail("invalid character in resource path");
  }

  std::filesystem::path cur = path_;
  std::size_t i = 0;
  std::size_t components = 0;
  while (i <= rel.size()) {
    const auto sep = rel.find_first_of("/\\", i);
    const auto end = sep == std::string::npos ? rel.size() : sep;
    const std::string_view comp(rel.data() + i, end - i);
    i = end + 1;
    if (comp.empty() || comp == ".") {
      continue;
    }
    if (comp == "..") {
      return fail("'..' not allowed in resource path");
    }

    const DirIndex* idx = dirIndex(cur);
    if (idx == nullptr) {
      return fail("missing directory in resource path");
    }
    const std::string folded = foldAscii(comp);
    if (idx->ambiguous.count(folded)) {
      return fail("ambiguous case-insensitive name");
    }
    const auto found = idx->byFolded.find(folded);
    if (found == idx->byFolded.end()) {
      return fail("resource not found");
    }
    cur /= found->second;
    ++components;
  }
  if (components == 0) {
    return fail("empty resource path");
  }

  std::error_code ec;
  const auto canon = std::filesystem::weakly_canonical(cur, ec);
  if (ec) {
    return fail("cannot canonicalize resolved path");
  }
  if (!isWithin(canon, path_)) {
    return fail("resolved path escapes data root");
  }
  return canon;
}

std::optional<std::uintmax_t>
DataRoot::fileSize(const std::string& rel, std::string* error) const {
  const auto p = resolve(rel, error);
  if (!p) {
    return std::nullopt;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(*p, ec) || ec) {
    if (error) {
      *error = "not a regular file: " + rel;
    }
    return std::nullopt;
  }
  const auto sz = std::filesystem::file_size(*p, ec);
  if (ec) {
    if (error) {
      *error = "cannot stat file: " + rel;
    }
    return std::nullopt;
  }
  return sz;
}

std::optional<std::vector<std::byte>>
DataRoot::readFile(const std::string& rel, std::size_t maxBytes,
                   std::string* error) const {
  auto fail = [&](const char* msg) -> std::optional<std::vector<std::byte>> {
    if (error) {
      *error = std::string(msg) + ": " + rel;
    }
    return std::nullopt;
  };

  const auto p = resolve(rel, error);
  if (!p) {
    return std::nullopt;
  }
  const auto sz = fileSize(rel, error);
  if (!sz) {
    return std::nullopt;
  }
  if (*sz > maxBytes) {
    return fail("file exceeds read bound");
  }
  std::ifstream f(*p, std::ios::binary);
  if (!f) {
    return fail("cannot open file");
  }
  std::vector<std::byte> buf(static_cast<std::size_t>(*sz));
  if (!buf.empty()) {
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(buf.size()));
    if (f.gcount() != static_cast<std::streamsize>(buf.size())) {
      return fail("short read");
    }
  }
  return buf;
}

std::optional<std::vector<std::byte>>
DataRoot::readPrefix(const std::string& rel, std::size_t n,
                     std::string* error) const {
  const auto p = resolve(rel, error);
  if (!p) {
    return std::nullopt;
  }
  const auto sz = fileSize(rel, error);
  if (!sz) {
    return std::nullopt;
  }
  std::ifstream f(*p, std::ios::binary);
  if (!f) {
    if (error) {
      *error = "cannot open file: " + rel;
    }
    return std::nullopt;
  }
  const auto want =
      static_cast<std::size_t>(std::min<std::uintmax_t>(*sz, n));
  std::vector<std::byte> buf(want);
  if (want != 0) {
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(want));
    if (f.gcount() != static_cast<std::streamsize>(want)) {
      if (error) {
        *error = "short read: " + rel;
      }
      return std::nullopt;
    }
  }
  return buf;
}

} // namespace mdk
