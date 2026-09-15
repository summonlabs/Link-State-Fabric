#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/limits.hpp"

namespace linkstate {

/// The set of links a publisher is authorized to observe and publish for.
///
/// An empty link set grants nothing: there is no implicit wildcard, because a
/// wildcard would let any connected worker mutate arbitrary links.
struct LSF_EXPORT AuthorityScope {
  /// Optional fabric restriction. A null FabricId means "any fabric".
  FabricId fabric;
  /// Authorized links, sorted and deduplicated.
  std::vector<LinkId> links;

  bool covers(const LinkId& link) const noexcept;
  bool empty() const noexcept { return links.empty(); }
  std::size_t size() const noexcept { return links.size(); }

  /// Sorts and removes duplicates. Returns false when the scope is empty or
  /// exceeds limits::max_authority_scope_links.
  bool normalize() noexcept;
};

/// Lifecycle of a publisher incarnation.
enum class AuthorityStatus : std::uint8_t {
  /// The incarnation may publish.
  Active = 0,
  /// The incarnation was explicitly invalidated by an operator or coordinator.
  Fenced,
  /// The incarnation was superseded by a newer boot of the same publisher.
  Superseded,
  /// The incarnation belonged to a superseded coordinator epoch.
  EpochSuperseded,
};

LSF_EXPORT const char* to_string(AuthorityStatus status) noexcept;

/// Registration request for one publisher incarnation.
struct LSF_EXPORT PublisherRegistration {
  PublisherId publisher;
  WorkerBootId worker_boot;
  SourceId source;
  AuthorityScope scope;
  CoordinatorEpoch epoch;
};

/// The engine's view of one publisher incarnation.
struct LSF_EXPORT PublisherAuthority {
  PublisherId publisher;
  WorkerBootId worker_boot;
  SourceId source;
  AuthorityScope scope;
  CoordinatorEpoch epoch;
  PublisherGeneration generation;
  AuthorityStatus status = AuthorityStatus::Active;
  std::size_t live_evidence = 0;
  std::uint64_t accepted_publications = 0;
  std::uint64_t rejected_publications = 0;
  bool operator==(const PublisherAuthority&) const = default;
};

/// Why a publisher incarnation was fenced.
struct LSF_EXPORT PublisherFence {
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::string reason;
};

}  // namespace linkstate
