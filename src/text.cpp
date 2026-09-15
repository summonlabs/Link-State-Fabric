#include "text.hpp"

#include <cstdio>

namespace linkstate::detail {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_hex_byte(std::string& out, unsigned value) {
  out.push_back(kHexDigits[(value >> 4u) & 0x0fu]);
  out.push_back(kHexDigits[value & 0x0fu]);
}

}  // namespace

void append_u64(std::string& out, std::uint64_t value) {
  char buffer[24];
  const int written = std::snprintf(buffer, sizeof(buffer), "%llu",
                                    static_cast<unsigned long long>(value));
  if (written > 0) {
    out.append(buffer, static_cast<std::size_t>(written));
  }
}

std::string format_u64(std::uint64_t value) {
  std::string out;
  append_u64(out, value);
  return out;
}

void append_bool(std::string& out, bool value) { out.append(value ? "true" : "false"); }

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto byte = static_cast<unsigned char>(text[index]);
    std::size_t extra = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if (byte < 0x80u) {
      ++index;
      continue;
    } else if ((byte & 0xe0u) == 0xc0u) {
      extra = 1;
      code_point = byte & 0x1fu;
      minimum = 0x80u;
    } else if ((byte & 0xf0u) == 0xe0u) {
      extra = 2;
      code_point = byte & 0x0fu;
      minimum = 0x800u;
    } else if ((byte & 0xf8u) == 0xf0u) {
      extra = 3;
      code_point = byte & 0x07u;
      minimum = 0x10000u;
    } else {
      return false;
    }
    if (index + extra >= size) {
      return false;
    }
    for (std::size_t offset = 1; offset <= extra; ++offset) {
      const auto continuation = static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xc0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6u) | (continuation & 0x3fu);
    }
    if (code_point < minimum) {
      return false;
    }
    if (code_point > 0x10ffffu) {
      return false;
    }
    if (code_point >= 0xd800u && code_point <= 0xdfffu) {
      return false;
    }
    index += extra + 1;
  }
  return true;
}

bool validate_text(std::string_view text, std::size_t max_length, std::string& error) {
  if (text.size() > max_length) {
    error = "text exceeds maximum length ";
    append_u64(error, static_cast<std::uint64_t>(max_length));
    return false;
  }
  if (!is_valid_utf8(text)) {
    error = "text is not well formed UTF-8";
    return false;
  }
  for (const char ch : text) {
    const auto value = static_cast<unsigned char>(ch);
    if (value < 0x20u || value == 0x7fu) {
      error = "text contains a control character";
      return false;
    }
  }
  return true;
}

std::string single_line(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '\n' || ch == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string to_hex(const std::vector<std::byte>& bytes) {
  std::string out;
  out.reserve(bytes.size() * 2u);
  for (const std::byte value : bytes) {
    append_hex_byte(out, std::to_integer<unsigned char>(value));
  }
  return out;
}

std::uint64_t fnv1a(std::string_view bytes) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char ch : bytes) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace linkstate::detail
