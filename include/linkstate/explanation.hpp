#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/record.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// What happened to one retained evidence record during resolution.
struct LSF_EXPORT EvidenceDisposition {
  ObservationId observation;
  SourceId source;
  PublisherId publisher;
  WorkerBootId worker_boot;
  EvidenceKind kind = EvidenceKind::CarrierState;
  EvidenceDirection direction = EvidenceDirection::Bidirectional;
  EvidenceClaim claim = EvidenceClaim::Unknown;
  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceStatus status = EvidenceStatus::Current;
  std::uint8_t precedence = 0;
  bool counted = false;
  std::string reason;
};

/// Deterministic explanation of the authoritative state of one link.
struct LSF_EXPORT Explanation {
  LinkId link;
  LinkOperationalState state = LinkOperationalState::Unknown;
  StateResultClass result_class = StateResultClass::AuthoritativeUnknown;
  DegradationCause degradation_cause = DegradationCause::None;
  LinkStateGeneration state_generation;
  GlobalGeneration global_generation;
  EvidenceGeneration evidence_generation;
  TopologyGeneration topology_generation;

  /// Ordered rule applications that produced the state.
  std::vector<std::string> rules;
  /// Ordered, canonicalised evidence dispositions.
  std::vector<EvidenceDisposition> evidence;
  /// Conflict notes, in canonical order.
  std::vector<std::string> conflicts;
  /// Authority and fencing notes.
  std::vector<std::string> authority;
  /// Bounded state history, oldest first.
  std::vector<StateHistoryEntry> history;

  /// Deterministic multi line rendering. Stable for identical inputs.
  std::string render() const;
};

}  // namespace linkstate
