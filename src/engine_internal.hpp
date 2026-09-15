#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "linkstate/engine.hpp"

namespace linkstate::detail {

/// One evidence slot retained for a link. Exactly one current record exists per
/// evidence key.
struct LinkEntry {
  LinkStateRecord record;
  std::vector<EvidenceRecord> current;
  std::vector<EvidenceDisposition> dispositions;
  bool conflicted = false;
  bool synthetic_backed = false;
};

/// Result of resolving the current evidence of one link.
struct Resolution {
  LinkOperationalState state = LinkOperationalState::Unknown;
  StateResultClass result_class = StateResultClass::AuthoritativeUnknown;
  DegradationCause cause = DegradationCause::None;
  bool conflicted = false;
  bool synthetic_backed = false;
  std::vector<std::string> rules;
  std::vector<std::string> conflicts;
  std::vector<EvidenceDisposition> dispositions;
};

/// Rules based, order independent evidence resolution.
///
/// The function is pure: it reads the supplied records and the durable record
/// metadata and returns a complete resolution. Records are canonicalised
/// internally, so the caller may supply them in any order.
Resolution resolve_link_state(std::span<const EvidenceRecord* const> records,
                              const LinkStateRecord& record);

/// Returns the canonical order index used to sort evidence deterministically.
bool evidence_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs);

/// The engine's internal store and counters.
struct EngineStore {
  std::unordered_map<LinkId, LinkEntry> links;
  std::unordered_map<LinkOperationalState, std::unordered_set<LinkId>> by_state;
  std::unordered_map<EndpointId, std::unordered_set<LinkId>> by_endpoint;
  std::unordered_map<PublisherId, PublisherAuthority> publishers;
  std::unordered_map<PublisherId, std::unordered_set<LinkId>> publisher_links;
  std::unordered_map<ObservationId, EvidenceKey> observations;
  std::unordered_map<LinkId, std::unordered_set<LinkId>> dependents;
  /// Permanently fenced worker incarnations, oldest first so that the retention
  /// window can evict in a bounded way.
  std::unordered_map<PublisherId, std::deque<WorkerBootId>> fenced_boots;
  std::vector<PublisherFence> fences;
  std::unordered_map<SnapshotId, Snapshot> snapshots;
  std::deque<SnapshotId> snapshot_order;
};

}  // namespace linkstate::detail

namespace linkstate {

class LinkStateEngine::Impl {
 public:
  explicit Impl(EngineConfig engine_config);

  EngineConfig config;
  mutable std::shared_mutex mutex;
  detail::EngineStore store;

  GlobalGeneration global_generation;
  LinkStateGeneration global_state_generation;
  CoordinatorEpoch epoch;
  TopologyGeneration topology_generation;
  std::uint64_t transition_counter = 0;
  std::uint64_t snapshot_counter = 0;

  EngineStats counters;
};

}  // namespace linkstate
