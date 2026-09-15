#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace linkstate::detail {

/// Appends the decimal rendering of an unsigned value.
void append_u64(std::string& out, std::uint64_t value);

/// Decimal rendering of an unsigned value.
std::string format_u64(std::uint64_t value);

/// Appends a stable, human readable boolean.
void append_bool(std::string& out, bool value);

/// Validates a bounded, well formed UTF-8 text field.
///
/// Rejects: input longer than max_length, invalid UTF-8 (overlong encodings,
/// surrogate code points, values above U+10FFFF, truncated sequences) and
/// control characters other than none.
bool validate_text(std::string_view text, std::size_t max_length, std::string& error);

/// True when the byte range is well formed UTF-8.
bool is_valid_utf8(std::string_view text) noexcept;

/// Replaces every newline and carriage return so that a value can be rendered
/// on a single deterministic line without changing its meaning.
std::string single_line(std::string_view text);

/// Renders a byte range as lowercase hexadecimal.
std::string to_hex(const std::vector<std::byte>& bytes);

/// FNV-1a over raw bytes, used for deterministic unordered container ordering
/// in diagnostics only.
std::uint64_t fnv1a(std::string_view bytes) noexcept;

}  // namespace linkstate::detail
