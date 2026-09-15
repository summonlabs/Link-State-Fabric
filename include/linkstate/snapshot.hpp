#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/record.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// Selection criteria for an immutable snapshot.
struct LSF_EXPORT SnapshotRequest {
  /// Optional fabric restriction.
  FabricId fabric;
  /// Optional explicit link selection. Empty means "every link in scope".
  std::vector<LinkId> links;
  /// Optional state filter.
  std::optional<LinkOperationalState> state_filter;
  /// Maximum records to include; 0 means the engine configured maximum.
  std::size_t max_records = 0;
  /// Human readable label, excluded from the snapshot digest.
  std::string label;
};

/// One record inside a snapshot.
struct LSF_EXPORT SnapshotRecord {
  LinkId link;
  LinkOperationalState state = LinkOperationalState::Unknown;
  StateResultClass result_class = StateResultClass::AuthoritativeUnknown;
  LinkStateGeneration state_generation;
  EvidenceGeneration evidence_generation;
  TopologyGeneration topology_generation;
  std::size_t current_evidence = 0;
  bool conflicted = false;
  bool synthetic_backed = false;
  Digest record_digest;

  bool operator==(const SnapshotRecord&) const = default;
  friend bool operator<(const SnapshotRecord& lhs, const SnapshotRecord& rhs) noexcept {
    return lhs.link < rhs.link;
  }
};

/// Immutable authoritative state snapshot.
///
/// A snapshot binds the global state generation and coordinator epoch it was
/// taken at, so a consumer can always decide whether it is current. Snapshots
/// are retained in a bounded registry and stay inspectable after they stop
/// being current, but they never masquerade as current.
struct LSF_EXPORT Snapshot {
  SnapshotId id;
  std::string label;
  GlobalGeneration global_generation;
  LinkStateGeneration global_state_generation;
  CoordinatorEpoch epoch;
  TopologyGeneration topology_generation;
  std::uint64_t sequence = 0;
  std::vector<SnapshotRecord> records;
  Digest digest;

  std::size_t size() const noexcept { return records.size(); }
  bool empty() const noexcept { return records.empty(); }
  std::string render() const;
};

/// Deterministic digest of a snapshot body: records are ordered by link
/// identity and the label is excluded, so identical authoritative state always
/// produces an identical digest.
LSF_EXPORT Digest digest_snapshot(std::vector<SnapshotRecord> records,
                                  GlobalGeneration global_generation,
                                  LinkStateGeneration global_state_generation,
                                  CoordinatorEpoch epoch);

}  // namespace linkstate
