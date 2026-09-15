#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

#include "linkstate/export.hpp"

namespace linkstate {

/// Monotonic generation counter.
///
/// Generations are the primary staleness mechanism of Link State Fabric: a
/// mutation is only committed when every generation it binds is current. A
/// generation never decreases and never wraps: incrementing the maximum value
/// fails instead of overflowing.
template <class Tag>
class StrongGeneration {
 public:
  using value_type = std::uint64_t;
  using tag_type = Tag;

  constexpr StrongGeneration() noexcept = default;
  explicit constexpr StrongGeneration(std::uint64_t value) noexcept : value_(value) {}

  static constexpr StrongGeneration from_value(std::uint64_t value) noexcept {
    return StrongGeneration(value);
  }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  /// Returns the successor generation, or nullopt when the counter is saturated.
  std::optional<StrongGeneration> next() const noexcept {
    if (value_ == (std::numeric_limits<std::uint64_t>::max)()) {
      return std::nullopt;
    }
    return StrongGeneration(value_ + 1u);
  }

  std::string to_string() const { return std::to_string(value_); }

  friend constexpr bool operator==(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ != rhs.value_;
  }
  friend constexpr bool operator<(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ > rhs.value_;
  }
  friend constexpr bool operator<=(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ <= rhs.value_;
  }
  friend constexpr bool operator>=(StrongGeneration lhs, StrongGeneration rhs) noexcept {
    return lhs.value_ >= rhs.value_;
  }

 private:
  std::uint64_t value_ = 0;
};

namespace tags {
struct LinkStateGenerationTag {};
struct EvidenceGenerationTag {};
struct TopologyGenerationTag {};
struct EndpointGenerationTag {};
struct PublisherGenerationTag {};
struct AdminStateGenerationTag {};
struct SourceGenerationTag {};
struct GlobalGenerationTag {};
struct CoordinatorEpochTag {};
struct LayoutGenerationTag {};
}  // namespace tags

using LinkStateGeneration = StrongGeneration<tags::LinkStateGenerationTag>;
using EvidenceGeneration = StrongGeneration<tags::EvidenceGenerationTag>;
using TopologyGeneration = StrongGeneration<tags::TopologyGenerationTag>;
using EndpointGeneration = StrongGeneration<tags::EndpointGenerationTag>;
using PublisherGeneration = StrongGeneration<tags::PublisherGenerationTag>;
using AdminStateGeneration = StrongGeneration<tags::AdminStateGenerationTag>;
using SourceGeneration = StrongGeneration<tags::SourceGenerationTag>;
using GlobalGeneration = StrongGeneration<tags::GlobalGenerationTag>;
using CoordinatorEpoch = StrongGeneration<tags::CoordinatorEpochTag>;
using LayoutGeneration = StrongGeneration<tags::LayoutGenerationTag>;

}  // namespace linkstate

namespace std {

template <class Tag>
struct hash<linkstate::StrongGeneration<Tag>> {
  std::size_t operator()(const linkstate::StrongGeneration<Tag>& generation) const noexcept {
    return static_cast<std::size_t>(generation.value());
  }
};

}  // namespace std
