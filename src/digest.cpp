#include "linkstate/digest.hpp"

#include <cstring>

namespace linkstate {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t kInitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                            0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32u - count));
}

int hex_value(char ch) noexcept {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  return -1;
}

}  // namespace

Digest Digest::from_bytes(const std::array<std::byte, byte_count>& bytes) noexcept {
  Digest digest;
  digest.bytes_ = bytes;
  return digest;
}

std::optional<Digest> Digest::from_hex(std::string_view hex) noexcept {
  if (hex.size() != byte_count * 2u) {
    return std::nullopt;
  }
  Digest digest;
  for (std::size_t index = 0; index < byte_count; ++index) {
    const int high = hex_value(hex[index * 2u]);
    const int low = hex_value(hex[index * 2u + 1u]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    digest.bytes_[index] = static_cast<std::byte>((high << 4) | low);
  }
  return digest;
}

std::string Digest::to_hex() const {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(byte_count * 2u);
  for (const std::byte value : bytes_) {
    const auto byte = std::to_integer<unsigned>(value);
    out.push_back(kDigits[(byte >> 4u) & 0x0fu]);
    out.push_back(kDigits[byte & 0x0fu]);
  }
  return out;
}

bool Digest::is_zero() const noexcept {
  for (const std::byte value : bytes_) {
    if (value != std::byte{0}) {
      return false;
    }
  }
  return true;
}

std::string to_string(const Digest& digest) { return digest.to_hex(); }

Hasher::Hasher() noexcept {
  std::memcpy(state_.data(), kInitialState, sizeof(kInitialState));
}

void Hasher::compress(const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (std::size_t index = 0; index < 16; ++index) {
    schedule[index] = (static_cast<std::uint32_t>(block[index * 4u]) << 24u) |
                      (static_cast<std::uint32_t>(block[index * 4u + 1u]) << 16u) |
                      (static_cast<std::uint32_t>(block[index * 4u + 2u]) << 8u) |
                      static_cast<std::uint32_t>(block[index * 4u + 3u]);
  }
  for (std::size_t index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotate_right(schedule[index - 15], 7) ^
                             rotate_right(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3u);
    const std::uint32_t s1 = rotate_right(schedule[index - 2], 17) ^
                             rotate_right(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10u);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t index = 0; index < 64; ++index) {
    const std::uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + s1 + ch + kRoundConstants[index] + schedule[index];
    const std::uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Hasher::update(std::span<const std::byte> bytes) noexcept {
  const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
  std::size_t remaining = bytes.size();
  total_bytes_ += static_cast<std::uint64_t>(remaining);
  while (remaining > 0) {
    const std::size_t take = (remaining < (64u - buffered_)) ? remaining : (64u - buffered_);
    std::memcpy(buffer_.data() + buffered_, data, take);
    buffered_ += take;
    data += take;
    remaining -= take;
    if (buffered_ == 64u) {
      compress(buffer_.data());
      buffered_ = 0;
    }
  }
}

void Hasher::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

void Hasher::update(bool value) noexcept { update_u8(value ? 1u : 0u); }

void Hasher::update_bool(bool value) noexcept { update_u8(value ? 1u : 0u); }

void Hasher::update_u8(std::uint8_t value) noexcept {
  const std::byte buffer[1] = {static_cast<std::byte>(value)};
  update(std::span<const std::byte>(buffer, 1));
}

void Hasher::update_u16(std::uint16_t value) noexcept {
  const std::byte buffer[2] = {static_cast<std::byte>(value & 0xffu),
                               static_cast<std::byte>((value >> 8u) & 0xffu)};
  update(std::span<const std::byte>(buffer, 2));
}

void Hasher::update_u32(std::uint32_t value) noexcept {
  const std::byte buffer[4] = {static_cast<std::byte>(value & 0xffu),
                               static_cast<std::byte>((value >> 8u) & 0xffu),
                               static_cast<std::byte>((value >> 16u) & 0xffu),
                               static_cast<std::byte>((value >> 24u) & 0xffu)};
  update(std::span<const std::byte>(buffer, 4));
}

void Hasher::update_u64(std::uint64_t value) noexcept {
  update_u32(static_cast<std::uint32_t>(value & 0xffffffffull));
  update_u32(static_cast<std::uint32_t>((value >> 32u) & 0xffffffffull));
}

void Hasher::update_text(std::string_view text) noexcept {
  update_u32(static_cast<std::uint32_t>(text.size()));
  update(text);
}

Digest Hasher::finalize() noexcept {
  if (!finalized_) {
    const std::uint64_t bit_count = total_bytes_ * 8u;
    const std::byte padding = static_cast<std::byte>(0x80u);
    update(std::span<const std::byte>(&padding, 1));
    const std::byte zero{0};
    while (buffered_ != 56u) {
      update(std::span<const std::byte>(&zero, 1));
    }
    std::byte length_bytes[8];
    for (std::size_t index = 0; index < 8; ++index) {
      length_bytes[index] = static_cast<std::byte>((bit_count >> (56u - 8u * index)) & 0xffull);
    }
    update(std::span<const std::byte>(length_bytes, 8));
    finalized_ = true;
  }
  std::array<std::byte, Digest::byte_count> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    const std::uint32_t word = state_[index];
    bytes[index * 4u] = static_cast<std::byte>((word >> 24u) & 0xffu);
    bytes[index * 4u + 1u] = static_cast<std::byte>((word >> 16u) & 0xffu);
    bytes[index * 4u + 2u] = static_cast<std::byte>((word >> 8u) & 0xffu);
    bytes[index * 4u + 3u] = static_cast<std::byte>(word & 0xffu);
  }
  return Digest::from_bytes(bytes);
}

Digest sha256(std::span<const std::byte> bytes) {
  Hasher hasher;
  hasher.update(bytes);
  return hasher.finalize();
}

Digest sha256(std::string_view text) {
  Hasher hasher;
  hasher.update(text);
  return hasher.finalize();
}

}  // namespace linkstate
