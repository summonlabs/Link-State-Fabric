#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "linkstate/export.hpp"

namespace linkstate {

/// 256 bit content digest (SHA-256) used for authoritative state digests,
/// snapshot digests and persistence integrity checks.
class Digest {
 public:
  static constexpr std::size_t byte_count = 32;

  constexpr Digest() noexcept = default;

  static Digest from_bytes(const std::array<std::byte, byte_count>& bytes) noexcept;

  /// Parses a 64 character lowercase or uppercase hexadecimal rendering.
  static std::optional<Digest> from_hex(std::string_view hex) noexcept;

  /// Lowercase hexadecimal rendering, 64 characters.
  std::string to_hex() const;

  const std::array<std::byte, byte_count>& bytes() const noexcept { return bytes_; }

  bool is_zero() const noexcept;

  friend bool operator==(const Digest& lhs, const Digest& rhs) noexcept {
    return lhs.bytes_ == rhs.bytes_;
  }
  friend bool operator!=(const Digest& lhs, const Digest& rhs) noexcept { return !(lhs == rhs); }
  friend bool operator<(const Digest& lhs, const Digest& rhs) noexcept {
    return lhs.bytes_ < rhs.bytes_;
  }

 private:
  std::array<std::byte, byte_count> bytes_{};
};

LSF_EXPORT std::string to_string(const Digest& digest);

/// Streaming SHA-256 hasher with fixed width, endian explicit encoding helpers.
///
/// All multi byte integers are folded in little endian so that digests are
/// identical on every platform and independent of in-memory layout.
class LSF_EXPORT Hasher {
 public:
  Hasher() noexcept;

  void update(std::span<const std::byte> bytes) noexcept;
  void update(std::string_view text) noexcept;
  void update(bool value) noexcept;
  void update_bool(bool value) noexcept;
  void update_u8(std::uint8_t value) noexcept;
  void update_u16(std::uint16_t value) noexcept;
  void update_u32(std::uint32_t value) noexcept;
  void update_u64(std::uint64_t value) noexcept;

  /// Folds a length prefix (u32) followed by the raw bytes of the text.
  void update_text(std::string_view text) noexcept;

  /// Finalizes the hash. The hasher must not be used afterwards.
  Digest finalize() noexcept;

 private:
  void compress(const std::uint8_t* block) noexcept;

  std::array<std::uint32_t, 8> state_{};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
  bool finalized_ = false;
};

/// One-shot digest of a byte range.
LSF_EXPORT Digest sha256(std::span<const std::byte> bytes);

/// One-shot digest of a text range.
LSF_EXPORT Digest sha256(std::string_view text);

}  // namespace linkstate

namespace std {

template <>
struct hash<linkstate::Digest> {
  std::size_t operator()(const linkstate::Digest& digest) const noexcept {
    std::size_t value = 1469598103934665603ull;
    for (const std::byte b : digest.bytes()) {
      value ^= static_cast<std::size_t>(std::to_integer<unsigned char>(b));
      value *= 1099511628211ull;
    }
    return value;
  }
};

}  // namespace std
