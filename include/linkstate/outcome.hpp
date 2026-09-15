#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "linkstate/digest.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"

namespace linkstate {

/// Structured, deterministic result code of every mutating operation.
///
/// The runtime never collapses distinct failures into one generic error: a
/// caller can always tell a stale generation from a fenced publisher or an
/// unauthorized scope.
enum class OutcomeCode : std::uint8_t {
  Committed = 0,
  Idempotent,
  NoChange,
  StaleGeneration,
  StaleEvidence,
  StaleTopology,
  StaleAuthority,
  StaleCoordinatorEpoch,
  UnauthorizedScope,
  UnauthorizedPublisher,
  UnknownLink,
  RetiredLink,
  EvidenceConflict,
  RevalidationRequired,
  InvalidTransition,
  MalformedRequest,
  ResourceLimit,
  PersistenceCorrupt,
  PersistenceIoFailure,
  TransportFailure,
  UnsupportedCapability,
  UnsupportedState,
  NotFound,
  Invalidated,
};

/// Stable uppercase rendering of an outcome code.
LSF_EXPORT std::string_view to_string(OutcomeCode code) noexcept;

/// Structured outcome of a mutating operation.
struct LSF_EXPORT Outcome {
  OutcomeCode code = OutcomeCode::MalformedRequest;
  std::string detail;
  LinkStateGeneration state_generation{};
  GlobalGeneration global_generation{};
  EvidenceGeneration evidence_generation{};
  bool state_changed = false;

  Outcome() = default;

  static Outcome make(OutcomeCode code, std::string detail = {});

  bool is_error() const noexcept;
  bool committed() const noexcept { return code == OutcomeCode::Committed || code == OutcomeCode::NoChange; }

  /// Deterministic one line rendering used by tools and tests.
  std::string to_string() const;
};

}  // namespace linkstate
