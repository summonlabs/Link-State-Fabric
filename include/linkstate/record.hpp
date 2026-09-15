#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// Endpoint identities a link binds, with the endpoint generations current at
/// binding time.
struct LSF_EXPORT EndpointBinding {
  EndpointId local;
  EndpointGeneration local_generation;
  DeviceId local_device;
  PortId local_port;

  EndpointId remote;
  EndpointGeneration remote_generation;
  DeviceId remote_device;
  PortId remote_port;

  bool operator==(const EndpointBinding&) const = default;
  bool has_remote() const noexcept { return remote.valid(); }
};

/// Reference from a logical or aggregate link to one physical link it depends
/// on, pinned to the exact topology generation that was current when the
/// dependency was declared.
struct LSF_EXPORT BackingReference {
  LinkId link;
  TopologyGeneration generation;

  bool operator==(const BackingReference&) const = default;
  friend bool operator<(const BackingReference& lhs, const BackingReference& rhs) noexcept {
    return lhs.link < rhs.link;
  }
};

/// Structural binding of a link-state record to Fabric Topology.
///
/// The binding is the contract that lets the runtime reject operational state
/// that no longer describes the current structure.
struct LSF_EXPORT TopologyBinding {
  FabricId fabric;
  SiteId site;
  LinkId link;
  TopologyGeneration topology_generation;
  EndpointBinding endpoints;
  LinkClass link_class = LinkClass::Unknown;
  LinkSymmetryGuarantee symmetry = LinkSymmetryGuarantee::Unspecified;

  /// For aggregate links: member links of the aggregate.
  std::vector<LinkId> member_links;
  /// For logical or tunnel-backed links: the physical links it depends on,
  /// each pinned to a topology generation.
  std::vector<BackingReference> backing_links;

  bool operator==(const TopologyBinding&) const = default;
};

/// Durable administrative intent recorded by an authorized publisher through
/// the explicit transition surface.
///
/// Administrative intent is an engine owned constraint, not evidence: while it
/// is present it outranks every operational claim, so a stale operational UP
/// observation can never override a newer administrative restriction. Clearing
/// it requires an explicit transition to UP, which does not itself assert UP.
struct LSF_EXPORT AdministrativeIntent {
  LinkOperationalState state = LinkOperationalState::AdminDisabled;
  PublicationId publication;
  PublisherId publisher;
  GlobalGeneration generation;
  DegradationCause cause = DegradationCause::None;

  bool operator==(const AdministrativeIntent&) const = default;
};

/// One bounded state history entry. History is diagnostic only: it is never a
/// source of authoritative current state.
struct LSF_EXPORT StateHistoryEntry {
  LinkStateGeneration state_generation;
  LinkOperationalState from_state = LinkOperationalState::Unknown;
  LinkOperationalState to_state = LinkOperationalState::Unknown;
  TransitionTrigger trigger = TransitionTrigger::EvidenceResolution;
  StateResultClass result_class = StateResultClass::AuthoritativeUnknown;
  DegradationCause cause = DegradationCause::None;
  PublicationId publication;
  PublisherId publisher;
  EvidenceGeneration evidence_generation;
  TopologyGeneration topology_generation;
  OutcomeCode outcome = OutcomeCode::Committed;

  bool operator==(const StateHistoryEntry&) const = default;
  std::string render() const;
};

/// The authoritative durable record of one link.
struct LSF_EXPORT LinkStateRecord {
  LinkStateRecordId record;
  TopologyBinding binding;

  LinkOperationalState state = LinkOperationalState::Unknown;
  StateResultClass result_class = StateResultClass::AuthoritativeUnknown;
  DegradationCause degradation_cause = DegradationCause::None;

  LinkStateGeneration state_generation;
  GlobalGeneration global_generation;
  EvidenceGeneration evidence_generation;

  PublicationId last_publication;
  StateTransitionId last_transition;
  PublisherId last_publisher;
  OutcomeCode last_outcome = OutcomeCode::NoChange;

  /// True once a decisive (non UNKNOWN) state was committed and not retired.
  bool ever_established = false;
  /// True when the record is present in the durable store.
  bool durable = false;

  /// Present while an administrative restriction is in force.
  std::optional<AdministrativeIntent> admin_intent;

  Digest provenance_digest;
  std::vector<StateHistoryEntry> history;

  bool operator==(const LinkStateRecord&) const = default;
  std::string render() const;
};

/// Read-only projection returned by queries. It owns its data, so it stays
/// valid after internal locks are released.
struct LSF_EXPORT LinkStateView {
  LinkStateRecord record;
  std::size_t current_evidence = 0;
  std::size_t retained_dispositions = 0;
  bool conflicted = false;
  bool synthetic_backed = false;

  const LinkId& link() const noexcept { return record.binding.link; }
  LinkOperationalState state() const noexcept { return record.state; }
  LinkStateGeneration state_generation() const noexcept { return record.state_generation; }
};

/// Canonical digest of an authoritative record set. Records are sorted by link
/// identity before hashing, so insertion order cannot change the digest.
LSF_EXPORT Digest digest_records(const std::vector<LinkStateRecord>& records);

/// Canonical digest of a single record's authoritative commitment.
LSF_EXPORT Digest digest_record(const LinkStateRecord& record);

}  // namespace linkstate
