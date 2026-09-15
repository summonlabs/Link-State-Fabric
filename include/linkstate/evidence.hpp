#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// What a piece of evidence observes.
enum class EvidenceKind : std::uint8_t {
  CarrierState = 0,
  InterfaceOperationalState,
  DeviceReportedState,
  PortReportedState,
  RemoteEndpointState,
  ControlPlaneHeartbeat,
  HardwareErrorIndication,
  LossErrorThreshold,
  AdministrativeState,
  SyntheticTest,
  PlatformDiscovery,
};

LSF_EXPORT const char* to_string(EvidenceKind kind) noexcept;
LSF_EXPORT std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept;

/// Precedence tier used by the rules based resolver. Lower is stronger.
///
/// The tier is a documented policy input, not an accident of evaluation order:
///
///   tier 1  AdministrativeState     explicit administrative intent
///   tier 2  HardwareErrorIndication  device proven hardware fault
///   tier 3  CarrierState, InterfaceOperationalState, DeviceReportedState,
///           PortReportedState, ControlPlaneHeartbeat, PlatformDiscovery,
///           LossErrorThreshold     local operational observation
///   tier 4  RemoteEndpointState      the peer's view of the same link
///   tier 5  SyntheticTest            injected test evidence
LSF_EXPORT std::uint8_t evidence_precedence(EvidenceKind kind) noexcept;

/// Truthfulness classification required by the validation policy.
enum class EvidenceClass : std::uint8_t {
  /// Produced by actual host or device observation.
  Real = 0,
  /// Produced by the synthetic backend for environments unavailable on the host.
  Synthetic,
  /// A capability that is not present or not accessible on this host.
  Unsupported,
};

LSF_EXPORT const char* to_string(EvidenceClass evidence_class) noexcept;

/// Direction a piece of evidence describes.
enum class EvidenceDirection : std::uint8_t {
  Bidirectional = 0,
  Transmit,
  Receive,
};

LSF_EXPORT const char* to_string(EvidenceDirection direction) noexcept;

/// The state a source claims for the link.
enum class EvidenceClaim : std::uint8_t {
  Unknown = 0,
  Up,
  Down,
  Degraded,
  AdminDisabled,
  Draining,
  Faulted,
};

inline constexpr std::size_t evidence_claim_count = 7;

LSF_EXPORT const char* to_string(EvidenceClaim claim) noexcept;

/// Converts a claim to the operational state it asserts, if it asserts one.
LSF_EXPORT std::optional<LinkOperationalState> state_from_claim(EvidenceClaim claim) noexcept;

/// Converts an operational state into a publishable claim.
/// RETIRED and REVALIDATION_REQUIRED are lifecycle states, not claims, and have
/// no claim representation.
LSF_EXPORT std::optional<EvidenceClaim> claim_from_state(LinkOperationalState state) noexcept;

/// Named impairment behind a DEGRADED classification.
enum class DegradationCause : std::uint8_t {
  None = 0,
  ErrorRateThresholdExceeded,
  ReducedNegotiatedSpeed,
  PartialLaneFailure,
  UnstableCarrier,
  Asymmetry,
  DegradedOpticalSignal,
  RestrictedForwarding,
  HardwareReportedImpairment,
};

LSF_EXPORT const char* to_string(DegradationCause cause) noexcept;

/// Lifecycle of a retained evidence record inside the engine.
enum class EvidenceStatus : std::uint8_t {
  /// Current and eligible for resolution.
  Current = 0,
  /// Replaced by a newer observation of the same key.
  Superseded,
  /// Explicitly withdrawn by its publisher.
  Withdrawn,
  /// The publishing worker incarnation was fenced.
  StaleAuthority,
  /// Bound to a topology generation that is no longer current.
  StaleTopology,
  /// Replaced by a newer source generation of the same source.
  StaleSourceGeneration,
};

LSF_EXPORT const char* to_string(EvidenceStatus status) noexcept;

/// Identity of an evidence slot: at most one current record exists per key.
struct LSF_EXPORT EvidenceKey {
  LinkId link;
  SourceId source;
  EndpointId endpoint;
  EvidenceDirection direction = EvidenceDirection::Bidirectional;
  EvidenceKind kind = EvidenceKind::CarrierState;

  friend bool operator==(const EvidenceKey& lhs, const EvidenceKey& rhs) noexcept {
    return lhs.link == rhs.link && lhs.source == rhs.source && lhs.endpoint == rhs.endpoint &&
           lhs.direction == rhs.direction && lhs.kind == rhs.kind;
  }
  friend bool operator!=(const EvidenceKey& lhs, const EvidenceKey& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const EvidenceKey& lhs, const EvidenceKey& rhs) noexcept;

  std::string to_string() const;
};

/// One observation published by an authorized source.
struct LSF_EXPORT EvidenceRecord {
  EvidenceKey key;

  ObservationId observation;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;

  /// Incarnation generation of the observing source. A lower value than the one
  /// already recorded for the key is stale evidence.
  SourceGeneration source_generation;

  /// Monotonic per source observation order for this key. Must strictly
  /// increase for new observations; an equal value is either an exact replay
  /// (IDEMPOTENT) or a stale conflict (STALE_EVIDENCE).
  std::uint64_t sequence = 0;

  /// Topology generation the observation is bound to.
  TopologyGeneration topology_generation;

  /// Endpoint generation the observation is bound to.
  EndpointGeneration endpoint_generation;

  EvidenceClass evidence_class = EvidenceClass::Real;
  EvidenceClaim claim = EvidenceClaim::Unknown;
  DegradationCause degradation_cause = DegradationCause::None;

  /// Advisory source provided observation time. It is evidence metadata only:
  /// it never drives freshness, expiry or conflict resolution, and it is
  /// excluded from every authoritative digest.
  std::uint64_t observed_at_unix_nanos = 0;

  std::string detail;

  /// Opaque publisher supplied metadata, bounded by
  /// limits::max_evidence_metadata_bytes.
  std::vector<std::byte> metadata;

  /// Engine bookkeeping, assigned on commit.
  EvidenceStatus status = EvidenceStatus::Current;
  EvidenceGeneration evidence_generation;
  GlobalGeneration commit_sequence;
  Digest record_digest;

  /// Value equality over every semantic field.
  friend bool operator==(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept;
  friend bool operator!=(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
    return !(lhs == rhs);
  }

  /// Digest over all semantic fields except engine bookkeeping and the advisory
  /// observation timestamp.
  Digest compute_digest() const;
};

/// Deterministic digest of an evidence set: records are sorted by their
/// canonical key before hashing, so publication order cannot change the result.
LSF_EXPORT Digest digest_evidence_set(const std::vector<EvidenceRecord>& records);

/// Deterministic rendering of an evidence record, one field per line.
LSF_EXPORT std::string render(const EvidenceRecord& record);

}  // namespace linkstate
