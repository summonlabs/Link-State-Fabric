#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "linkstate/export.hpp"
#include "linkstate/limits.hpp"

namespace linkstate {

/// Result of validating an encoded identifier.
enum class IdValidation : std::uint8_t {
  Ok = 0,
  Empty,
  TooLong,
  InvalidCharacter,
  LeadingSeparator,
  TrailingSeparator,
  DotDot,
};

/// Stable diagnostic rendering of an identifier validation result.
LSF_EXPORT std::string_view to_string(IdValidation validation) noexcept;

/// Validates an encoded identifier.
///
/// Accepted form: 1..max_identifier_length bytes over [A-Za-z0-9], where the
/// first and last byte are alphanumeric and the interior may additionally use
/// '-', '_', '.', ':' and '/'. The two byte sequence ".." is always rejected so
/// that identifiers can never be interpreted as relative path traversal.
LSF_EXPORT IdValidation validate_identifier(std::string_view value) noexcept;

/// Strongly typed identifier.
///
/// A StrongId carries a domain tag, so identifiers from different domains never
/// convert into each other implicitly. A default constructed StrongId is the
/// null identifier: it is not valid, compares equal only to other null
/// identifiers, and is rejected by every mutating request.
template <class Tag>
class StrongId {
 public:
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;

  /// Builds an identifier from an already validated encoding.
  /// The caller must have validated; invalid input yields the null identifier.
  static StrongId from_validated(std::string value) {
    if (validate_identifier(value) != IdValidation::Ok) {
      return StrongId{};
    }
    StrongId result;
    result.hash_ = hash_of(value);
    result.value_ = std::move(value);
    return result;
  }

  /// Parses an encoded identifier, returning nullopt when malformed.
  static std::optional<StrongId> parse(std::string_view value) {
    if (validate_identifier(value) != IdValidation::Ok) {
      return std::nullopt;
    }
    std::string owned(value);
    StrongId result;
    result.hash_ = hash_of(owned);
    result.value_ = std::move(owned);
    return result;
  }

  /// Parses an encoded identifier, reporting why malformed input was rejected.
  static std::optional<StrongId> parse(std::string_view value, IdValidation& reason) {
    reason = validate_identifier(value);
    if (reason != IdValidation::Ok) {
      return std::nullopt;
    }
    return parse(value);
  }

  bool valid() const noexcept { return !value_.empty(); }
  explicit operator bool() const noexcept { return valid(); }

  const std::string& value() const noexcept { return value_; }
  const char* c_str() const noexcept { return value_.c_str(); }

  std::string to_string() const { return value_; }

  std::uint64_t hash() const noexcept { return hash_; }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const StrongId& lhs, const StrongId& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const StrongId& lhs, const StrongId& rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend bool operator>(const StrongId& lhs, const StrongId& rhs) noexcept { return rhs < lhs; }
  friend bool operator<=(const StrongId& lhs, const StrongId& rhs) noexcept { return !(rhs < lhs); }
  friend bool operator>=(const StrongId& lhs, const StrongId& rhs) noexcept { return !(lhs < rhs); }

 private:
  static std::uint64_t hash_of(const std::string& value) noexcept {
    // FNV-1a over the validated encoding. Deterministic and stable across
    // processes, unlike std::hash implementations that may be salted.
    std::uint64_t hash = 1469598103934665603ull;
    for (const char ch : value) {
      hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
      hash *= 1099511628211ull;
    }
    return hash == 0 ? 1ull : hash;
  }

  std::string value_;
  std::uint64_t hash_ = 0;
};

namespace tags {
struct FabricTag {};
struct SiteTag {};
struct DeviceTag {};
struct SwitchTag {};
struct RouterTag {};
struct NicTag {};
struct PortTag {};
struct LinkTag {};
struct EndpointTag {};
struct LinkStateRecordTag {};
struct ObservationTag {};
struct PublisherTag {};
struct WorkerBootTag {};
struct PublicationTag {};
struct StateTransitionTag {};
struct SnapshotTag {};
struct SourceTag {};
}  // namespace tags

using FabricId = StrongId<tags::FabricTag>;
using SiteId = StrongId<tags::SiteTag>;
using DeviceId = StrongId<tags::DeviceTag>;
using SwitchId = StrongId<tags::SwitchTag>;
using RouterId = StrongId<tags::RouterTag>;
using NicId = StrongId<tags::NicTag>;
using PortId = StrongId<tags::PortTag>;
using LinkId = StrongId<tags::LinkTag>;
using EndpointId = StrongId<tags::EndpointTag>;
using LinkStateRecordId = StrongId<tags::LinkStateRecordTag>;
using ObservationId = StrongId<tags::ObservationTag>;
using PublisherId = StrongId<tags::PublisherTag>;
using WorkerBootId = StrongId<tags::WorkerBootTag>;
using PublicationId = StrongId<tags::PublicationTag>;
using StateTransitionId = StrongId<tags::StateTransitionTag>;
using SnapshotId = StrongId<tags::SnapshotTag>;
using SourceId = StrongId<tags::SourceTag>;

}  // namespace linkstate

namespace std {

template <class Tag>
struct hash<linkstate::StrongId<Tag>> {
  std::size_t operator()(const linkstate::StrongId<Tag>& id) const noexcept {
    return static_cast<std::size_t>(id.hash());
  }
};

}  // namespace std
