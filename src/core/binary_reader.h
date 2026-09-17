// Bounded little-endian binary reader.
//
// NATIVE PORT PROJECT DECISION: a platform-neutral cursor over a
// caller-owned byte span. Every primitive read is bounds-checked and
// returns std::optional — malformed input produces a structured
// failure (nullopt), never UB. No unaligned host-struct casts, no
// dependence on host endianness, and all offset arithmetic is done in
// "n <= remaining()" form so it cannot overflow.
//
// ORIGINAL ENGINE OBSERVATION: original MDK data is little-endian
// (x86 builds; u32 length fields verified in BUILD_A file headers).

#ifndef MDK_CORE_BINARY_READER_H
#define MDK_CORE_BINARY_READER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace mdk {

class BinaryReader {
public:
  explicit BinaryReader(std::span<const std::byte> data) : data_(data) {}

  std::size_t size() const { return data_.size(); }
  std::size_t position() const { return pos_; }
  std::size_t remaining() const { return data_.size() - pos_; }
  bool atEnd() const { return pos_ == data_.size(); }

  // Absolute seek. Fails (returns false, position unchanged) past end.
  bool seek(std::size_t pos) {
    if (pos > data_.size()) {
      return false;
    }
    pos_ = pos;
    return true;
  }

  bool skip(std::size_t n) {
    if (n > remaining()) {
      return false;
    }
    pos_ += n;
    return true;
  }

  std::optional<std::uint8_t> u8() {
    if (remaining() < 1) {
      return std::nullopt;
    }
    return static_cast<std::uint8_t>(data_[pos_++]);
  }

  std::optional<std::uint16_t> u16le() {
    if (remaining() < 2) {
      return std::nullopt;
    }
    const auto v = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data_[pos_]) |
        static_cast<std::uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return v;
  }

  std::optional<std::uint32_t> u32le() {
    if (remaining() < 4) {
      return std::nullopt;
    }
    const auto v = static_cast<std::uint32_t>(data_[pos_]) |
                   static_cast<std::uint32_t>(data_[pos_ + 1]) << 8 |
                   static_cast<std::uint32_t>(data_[pos_ + 2]) << 16 |
                   static_cast<std::uint32_t>(data_[pos_ + 3]) << 24;
    pos_ += 4;
    return v;
  }

  // Non-advancing variants, used by header probes that inspect before
  // committing the cursor.
  std::optional<std::uint16_t> peekU16le(std::size_t offset) const {
    if (offset > data_.size() || data_.size() - offset < 2) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data_[offset]) |
        static_cast<std::uint16_t>(data_[offset + 1]) << 8);
  }

  std::optional<std::uint32_t> peekU32le(std::size_t offset) const {
    if (offset > data_.size() || data_.size() - offset < 4) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(data_[offset]) |
           static_cast<std::uint32_t>(data_[offset + 1]) << 8 |
           static_cast<std::uint32_t>(data_[offset + 2]) << 16 |
           static_cast<std::uint32_t>(data_[offset + 3]) << 24;
  }

  // Borrow the next `n` bytes as a span (zero-copy view into the
  // caller's buffer). Fails on truncation; the cursor does not move.
  std::optional<std::span<const std::byte>> bytes(std::size_t n) {
    if (n > remaining()) {
      return std::nullopt;
    }
    const auto s = data_.subspan(pos_, n);
    pos_ += n;
    return s;
  }

  // Checked sub-reader over the next `n` bytes; the parent cursor
  // advances past the sliced region only on success.
  std::optional<BinaryReader> subReader(std::size_t n) {
    const auto s = bytes(n);
    if (!s) {
      return std::nullopt;
    }
    return BinaryReader(*s);
  }

private:
  std::span<const std::byte> data_;
  std::size_t pos_ = 0;
};

} // namespace mdk

#endif // MDK_CORE_BINARY_READER_H
