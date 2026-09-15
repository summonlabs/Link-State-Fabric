#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine_internal.hpp"

namespace linkstate::detail {

Outcome failure(OutcomeCode code, std::string detail_text);
LinkMutationResult failed(OutcomeCode code, std::string detail_text);

bool advance(GlobalGeneration& generation);
bool advance(LinkStateGeneration& generation);
bool advance(EvidenceGeneration& generation);
bool advance(PublisherGeneration& generation);

std::string record_id_for(const LinkId& link);
std::string transition_id_for(const LinkId& link, LinkStateGeneration generation);
std::string publication_id_for(const char* prefix, std::uint64_t sequence);

bool same_structure(const TopologyBinding& lhs, const TopologyBinding& rhs);

/// True when one of the backing references of a binding is no longer at the
/// pinned generation, or the backing link is unknown. A link in that condition
/// may not accept current evidence.
bool backing_stale(const LinkStateEngine::Impl& impl, const TopologyBinding& binding,
                   std::string& detail);
void sort_links(std::vector<LinkId>& links);
bool valid_binding(const TopologyBinding& binding, std::string& error);

void index_link(EngineStore& store, const LinkId& link, LinkOperationalState state);
void reindex_state(EngineStore& store, const LinkId& link, LinkOperationalState from,
                   LinkOperationalState to);
void index_endpoints(EngineStore& store, const TopologyBinding& binding, bool add);
void push_disposition(LinkEntry& entry, const EvidenceRecord& record, EvidenceStatus status,
                      const std::string& reason, LinkStateEngine::Impl& impl);
void erase_observation(EngineStore& store, const EvidenceRecord& record);
void push_history(LinkEntry& entry, const StateHistoryEntry& history, std::size_t cap);

OutcomeCode authorize(EngineStore& store, const PublisherId& publisher,
                      const WorkerBootId& worker_boot, const SourceId& source,
                      CoordinatorEpoch request_epoch, CoordinatorEpoch engine_epoch,
                      const LinkId& link, const TopologyBinding& binding, std::string& error);

/// A resolution that has been validated but not yet applied.
///
/// prepare_commit performs every check that can fail; commit_prepared applies
/// the result and cannot fail. Mutating operations therefore never leave
/// partial evidence, mismatched indexes or half advanced generations.
struct PreparedCommit {
  bool ok = false;
  OutcomeCode code = OutcomeCode::Committed;
  std::string detail;
  Resolution resolution;
  TransitionTrigger effective_trigger = TransitionTrigger::EvidenceResolution;
  LinkOperationalState target_state = LinkOperationalState::Unknown;
  bool state_changed = false;
};

PreparedCommit prepare_commit(const LinkStateEngine::Impl& impl, const LinkEntry& entry,
                              std::span<const EvidenceRecord* const> prospective,
                              TransitionTrigger trigger);

/// Read-only projection of one link entry.
LinkStateView make_view(const LinkEntry& entry);

void commit_prepared(LinkStateEngine::Impl& impl, LinkEntry& entry, PreparedCommit& prepared,
                     const PublicationId& publication, const PublisherId& publisher);

/// Resolves the prospective evidence of a link and applies the result.
/// Used by invalidation paths after they have already removed evidence.
OutcomeCode recompute_link(LinkStateEngine::Impl& impl, LinkEntry& entry,
                           TransitionTrigger trigger, const PublicationId& publication,
                           const PublisherId& publisher, bool& state_changed);

std::vector<const EvidenceRecord*> current_pointers(const LinkEntry& entry);

}  // namespace linkstate::detail
