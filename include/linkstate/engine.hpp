#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "linkstate/authority.hpp"
#include "linkstate/config.hpp"
#include "linkstate/digest.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/explanation.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/persistence.hpp"
#include "linkstate/record.hpp"
#include "linkstate/snapshot.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// Structural binding request: registers a link, or advances the topology
/// generation of an already known link.
struct LSF_EXPORT BindLinkRequest {
  TopologyBinding binding;
};

/// Topology supersession: Fabric Topology advanced a structural edge.
struct LSF_EXPORT SupersedeTopologyRequest {
  LinkId link;
  TopologyGeneration new_topology_generation;
  /// Replacement structure. When unset, the previous binding is retained but
  /// its topology generation advances.
  std::optional<TopologyBinding> replacement;
  std::string reason;
};

/// Retirement of a link-state record because the structure no longer exists.
struct LSF_EXPORT RetireLinkRequest {
  LinkId link;
  TopologyGeneration topology_generation;
  PublicationId publication;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::string reason;
};

/// Publisher incarnation registration.
struct LSF_EXPORT RegisterPublisherRequest {
  PublisherRegistration registration;
};

/// Fencing of a publisher incarnation.
struct LSF_EXPORT InvalidatePublisherRequest {
  PublisherId publisher;
  /// When unset, every incarnation of the publisher is fenced.
  std::optional<WorkerBootId> worker_boot;
  CoordinatorEpoch epoch;
  std::string reason;
};

/// Coordinator epoch advance after a coordinator restart.
struct LSF_EXPORT AdvanceEpochRequest {
  CoordinatorEpoch new_epoch;
  std::string reason;
};

/// Evidence publication.
struct LSF_EXPORT PublishEvidenceRequest {
  EvidenceRecord observation;
  /// Optional compare-and-swap on the link evidence generation.
  std::optional<EvidenceGeneration> expected_evidence_generation;
  /// Optional compare-and-swap on the link state generation.
  std::optional<LinkStateGeneration> expected_state_generation;
  /// Publication identity used for idempotent replay detection.
  PublicationId publication;
};

/// Explicit withdrawal of previously published evidence.
struct LSF_EXPORT WithdrawEvidenceRequest {
  EvidenceKey key;
  ObservationId observation;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  SourceGeneration source_generation;
  /// Withdrawal sequence. Must be strictly newer than the withdrawn record.
  std::uint64_t sequence = 0;
  std::string reason;
};

/// Explicit state transition requested by an authorized publisher, for example
/// administrative disablement or draining.
struct LSF_EXPORT TransitionStateRequest {
  LinkId link;
  LinkOperationalState target = LinkOperationalState::Unknown;
  PublicationId publication;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::optional<LinkStateGeneration> expected_state_generation;
  DegradationCause degradation_cause = DegradationCause::None;
  std::string reason;
};

/// Mark a link as requiring revalidation without asserting a state.
struct LSF_EXPORT MarkRevalidationRequest {
  LinkId link;
  PublicationId publication;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::optional<LinkStateGeneration> expected_state_generation;
  std::string reason;
};

/// Result of a mutating operation on one link.
struct LSF_EXPORT LinkMutationResult {
  Outcome outcome;
  std::optional<LinkStateView> view;

  bool committed() const noexcept { return outcome.committed(); }
};

/// Engine wide counters.
struct LSF_EXPORT EngineStats {
  std::size_t links = 0;
  std::size_t evidence_records = 0;
  std::size_t current_evidence = 0;
  std::size_t publishers = 0;
  std::size_t active_publishers = 0;
  std::size_t fenced_publishers = 0;
  std::size_t conflicts = 0;
  std::size_t revalidation_required = 0;
  std::size_t retired = 0;
  std::size_t retained_snapshots = 0;
  std::uint64_t committed_publications = 0;
  std::uint64_t idempotent_publications = 0;
  std::uint64_t rejected_publications = 0;
  std::uint64_t transitions = 0;
};

/// Result of loading a journal.
struct LSF_EXPORT LoadOutcome {
  Outcome outcome;
  std::size_t records_loaded = 0;
  std::size_t records_revalidated = 0;
  std::size_t records_left_unknown = 0;
  LayoutGeneration layout;
  Digest journal_digest;
};

/// The authoritative dynamic link-condition runtime.
///
/// Thread safety: every public method is safe to call concurrently from any
/// thread. Queries return owned values that remain valid after internal locks
/// are released; no public API hands out a reference into internal storage.
/// Internal locks are never held across user code because the engine invokes no
/// callbacks.
///
/// Generations: each link carries a state generation and an evidence
/// generation. A mutation that leaves both the authoritative state and the
/// current evidence view unchanged does not advance the state generation; an
/// exact replay of an already committed observation returns IDEMPOTENT.
///
/// Ownership: the engine owns all records, evidence, authority state and
/// snapshots. Callers hand in values and receive values.
class LSF_EXPORT LinkStateEngine {
 public:
  /// Opaque engine state.
  ///
  /// The type is public only so that the runtime's own translation units can
  /// name it; it has no public members and its definition is not installed.
  class Impl;

  explicit LinkStateEngine(EngineConfig config = {});
  ~LinkStateEngine();

  LinkStateEngine(LinkStateEngine&&) noexcept;
  LinkStateEngine& operator=(LinkStateEngine&&) noexcept;
  LinkStateEngine(const LinkStateEngine&) = delete;
  LinkStateEngine& operator=(const LinkStateEngine&) = delete;

  // -- configuration ------------------------------------------------------
  const EngineConfig& config() const noexcept;

  // -- topology binding ---------------------------------------------------
  LinkMutationResult bind_link(const BindLinkRequest& request);
  std::vector<LinkMutationResult> bind_links(std::span<const BindLinkRequest> requests);
  LinkMutationResult supersede_topology(const SupersedeTopologyRequest& request);
  std::vector<LinkMutationResult> supersede_topology_batch(
      std::span<const SupersedeTopologyRequest> requests);
  LinkMutationResult retire_link(const RetireLinkRequest& request);

  // -- authority ----------------------------------------------------------
  LinkMutationResult register_publisher(const RegisterPublisherRequest& request);
  LinkMutationResult invalidate_publisher(const InvalidatePublisherRequest& request);
  LinkMutationResult advance_coordinator_epoch(const AdvanceEpochRequest& request);

  // -- evidence -----------------------------------------------------------
  LinkMutationResult publish_evidence(const PublishEvidenceRequest& request);
  std::vector<LinkMutationResult> publish_evidence_batch(
      std::span<const PublishEvidenceRequest> requests);
  LinkMutationResult withdraw_evidence(const WithdrawEvidenceRequest& request);

  // -- explicit transitions ----------------------------------------------
  LinkMutationResult transition_state(const TransitionStateRequest& request);
  LinkMutationResult mark_revalidation_required(const MarkRevalidationRequest& request);

  // -- queries ------------------------------------------------------------
  std::optional<LinkStateView> link_state(const LinkId& link) const;
  std::optional<LinkStateRecord> record(const LinkId& link) const;
  std::vector<LinkStateView> links_in_state(LinkOperationalState state) const;
  std::vector<LinkId> links_requiring_revalidation() const;
  std::vector<LinkId> links_for_endpoint(const EndpointId& endpoint) const;
  std::optional<LinkStateView> link_by_endpoints(const EndpointId& a, const EndpointId& b) const;
  std::optional<LinkStateView> link_by_remote_endpoint(const EndpointId& local,
                                                       const EndpointId& remote) const;
  std::vector<EvidenceRecord> evidence_for_link(const LinkId& link) const;
  std::vector<EvidenceRecord> evidence_for_publisher(const PublisherId& publisher) const;
  std::optional<PublisherAuthority> publisher_authority(const PublisherId& publisher) const;
  std::vector<PublisherAuthority> publishers() const;
  std::vector<PublisherFence> fences() const;
  Explanation explain(const LinkId& link) const;
  std::optional<TopologyBinding> topology_binding(const LinkId& link) const;

  // -- aggregate state ----------------------------------------------------
  GlobalGeneration global_generation() const;
  LinkStateGeneration global_state_generation() const;
  EvidenceGeneration link_evidence_generation(const LinkId& link) const;
  CoordinatorEpoch coordinator_epoch() const;
  TopologyGeneration topology_generation() const;
  std::size_t link_count() const;
  std::size_t evidence_count() const;
  EngineStats stats() const;
  Digest state_digest() const;

  // -- snapshots ----------------------------------------------------------
  Snapshot snapshot(const SnapshotRequest& request);
  std::optional<Snapshot> retained_snapshot(const SnapshotId& id) const;
  std::vector<SnapshotId> retained_snapshot_ids() const;
  bool snapshot_is_current(const Snapshot& snapshot) const;

  /// Re-resolves every live link from its current evidence. Deterministic and
  /// idempotent: links whose resolution is unchanged keep their state
  /// generation.
  Outcome reconcile();

  // -- persistence --------------------------------------------------------
  Outcome save(const PersistenceConfig& config) const;
  LoadOutcome load(const PersistenceConfig& config);

 private:
  std::unique_ptr<Impl> impl_;
};

/// Inspects a journal without applying it. Returns the validated summary.
LSF_EXPORT Outcome inspect_journal(const PersistenceConfig& config, JournalSummary& summary);

}  // namespace linkstate
