#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "linkstate/export.hpp"

namespace linkstate {

/// Authoritative operational condition of one fabric link.
///
/// Exactly one state is authoritative for a link at a time. Every state has a
/// single meaning; no state is overloaded with unrelated semantics:
///
///   UNKNOWN                 No decisive current evidence and no durable prior
///                           conclusion exists. UNKNOWN is never a synonym for
///                           DOWN and never a synonym for UP.
///   UP                      Current evidence supports operational availability.
///   DOWN                    Current evidence supports a non-operational link.
///   DEGRADED                The link is operational but materially impaired; the
///                           impairment is named by a degradation cause.
///   ADMIN_DISABLED          The link is intentionally disabled by administrative
///                           intent, not by failure.
///   DRAINING                The link is administratively quiescing: it still
///                           forwards but must not take new work.
///   FAULTED                 A hardware or device fault was reported for the link.
///   REVALIDATION_REQUIRED   A durable prior state exists but no current evidence
///                           is authoritative for it. The runtime refuses to
///                           present the stale conclusion as current.
///   RETIRED                 The link-state record is terminal: the underlying
///                           topology edge or link identity was retired or
///                           superseded. Retired records never return to a live
///                           state.
enum class LinkOperationalState : std::uint8_t {
  Unknown = 0,
  Up = 1,
  Down = 2,
  Degraded = 3,
  AdminDisabled = 4,
  Draining = 5,
  Faulted = 6,
  RevalidationRequired = 7,
  Retired = 8,
};

/// Number of distinct operational states.
inline constexpr std::size_t link_operational_state_count = 9;

/// Canonical uppercase rendering, e.g. "REVALIDATION_REQUIRED".
LSF_EXPORT const char* to_string(LinkOperationalState state) noexcept;

/// Parses the canonical rendering. Unknown spellings return nullopt.
LSF_EXPORT std::optional<LinkOperationalState> parse_state(std::string_view text) noexcept;

/// Prose semantics of a state, used by explanations and diagnostics.
LSF_EXPORT const char* describe(LinkOperationalState state) noexcept;

/// True for every state except RETIRED.
LSF_EXPORT bool is_live_state(LinkOperationalState state) noexcept;

/// True only for RETIRED.
LSF_EXPORT bool is_terminal_state(LinkOperationalState state) noexcept;

/// True when the state asserts that the link is (possibly impaired) operational.
LSF_EXPORT bool is_operational_state(LinkOperationalState state) noexcept;

/// Why a state mutation is being attempted.
///
/// The trigger participates in transition legality: an identical state pair can
/// be legal for one cause and illegal for another. In particular a live state
/// may only degrade to UNKNOWN through evidence or withdrawal resolution, never
/// through publisher loss, source loss or topology invalidation, which must
/// produce REVALIDATION_REQUIRED instead.
enum class TransitionTrigger : std::uint8_t {
  EvidenceResolution = 0,
  EvidenceWithdrawal,
  PublisherFenced,
  SourceLost,
  TopologyInvalidation,
  CoordinatorEpochAdvance,
  Recovery,
  Retirement,
  ExplicitAdministrative,
  RevalidationOutcome,
};

LSF_EXPORT const char* to_string(TransitionTrigger trigger) noexcept;

/// Result of asking whether a transition is permitted.
enum class TransitionLegality : std::uint8_t {
  Legal = 0,
  SameState,
  FromRetired,
  RetirementRequired,
  TriggerNotPermitted,
  Illegal,
};

LSF_EXPORT const char* to_string(TransitionLegality legality) noexcept;

/// Evaluates the explicit transition matrix.
///
/// The matrix is total: every (from, to, trigger) combination is answered, and
/// RETIRED is a sink.
LSF_EXPORT TransitionLegality evaluate_transition(LinkOperationalState from, LinkOperationalState to,
                                                  TransitionTrigger trigger) noexcept;

/// Structural class of a link. The core state machine is class agnostic; the
/// class only selects evidence expectations (for example whether asymmetric
/// direction evidence is meaningful) and the default symmetry guarantee.
enum class LinkClass : std::uint8_t {
  Unknown = 0,
  EthernetLike,
  InfiniBandLike,
  HostNic,
  SwitchToSwitch,
  Logical,
  TunnelBacked,
  Optical,
  FabricInterconnect,
  BondedAggregate,
};

LSF_EXPORT const char* to_string(LinkClass link_class) noexcept;
LSF_EXPORT std::optional<LinkClass> parse_link_class(std::string_view text) noexcept;

/// True when the class describes a physical relationship rather than an
/// overlay built on top of physical relationships.
LSF_EXPORT bool link_class_is_physical(LinkClass link_class) noexcept;

/// Whether both directions of the link are guaranteed to report the same
/// condition.
enum class LinkSymmetryGuarantee : std::uint8_t {
  /// Not declared. The runtime treats directional disagreement as a conflict.
  Unspecified = 0,
  /// The link class guarantees symmetric behaviour: one endpoint claiming UP
  /// while the other claims DOWN is a conflict, not an impairment.
  Symmetric,
  /// The link class can legitimately be asymmetric: transmit UP with receive
  /// DOWN resolves to DEGRADED with an explicit asymmetry cause.
  Directional,
};

LSF_EXPORT const char* to_string(LinkSymmetryGuarantee guarantee) noexcept;

/// Default symmetry guarantee implied by a link class.
LSF_EXPORT LinkSymmetryGuarantee default_symmetry_for(LinkClass link_class) noexcept;

/// Classification of one evidence resolution outcome.
enum class StateResultClass : std::uint8_t {
  AuthoritativeUnknown = 0,
  AuthoritativeUp,
  AuthoritativeDown,
  AuthoritativeDegraded,
  AuthoritativeAdminDisabled,
  AuthoritativeDraining,
  AuthoritativeFaulted,
  Conflicted,
  RevalidationRequired,
  Retired,
};

LSF_EXPORT const char* to_string(StateResultClass result_class) noexcept;

/// Operational state that a result class presents.
LSF_EXPORT LinkOperationalState state_of(StateResultClass result_class) noexcept;

}  // namespace linkstate
