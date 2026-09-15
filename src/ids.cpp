#include "linkstate/ids.hpp"

namespace linkstate {
namespace {

bool is_alphanumeric(char ch) noexcept {
  return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}

bool is_separator(char ch) noexcept {
  return ch == '-' || ch == '_' || ch == '.' || ch == ':' || ch == '/';
}

}  // namespace

std::string_view to_string(IdValidation validation) noexcept {
  switch (validation) {
    case IdValidation::Ok:
      return "OK";
    case IdValidation::Empty:
      return "EMPTY";
    case IdValidation::TooLong:
      return "TOO_LONG";
    case IdValidation::InvalidCharacter:
      return "INVALID_CHARACTER";
    case IdValidation::LeadingSeparator:
      return "LEADING_SEPARATOR";
    case IdValidation::TrailingSeparator:
      return "TRAILING_SEPARATOR";
    case IdValidation::DotDot:
      return "DOT_DOT";
  }
  return "UNKNOWN";
}

IdValidation validate_identifier(std::string_view value) noexcept {
  if (value.empty()) {
    return IdValidation::Empty;
  }
  if (value.size() > limits::max_identifier_length) {
    return IdValidation::TooLong;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (!is_alphanumeric(ch) && !is_separator(ch)) {
      return IdValidation::InvalidCharacter;
    }
    if (ch == '.' && index + 1 < value.size() && value[index + 1] == '.') {
      return IdValidation::DotDot;
    }
  }
  if (!is_alphanumeric(value.front())) {
    return IdValidation::LeadingSeparator;
  }
  if (!is_alphanumeric(value.back())) {
    return IdValidation::TrailingSeparator;
  }
  return IdValidation::Ok;
}

}  // namespace linkstate
