#include "engine_support.hpp"

#include <algorithm>
#include <utility>

#include "text.hpp"

namespace linkstate::detail {

Outcome failure(OutcomeCode code, std::string detail_text) {
  return Outcome::make(code, std::move(detail_text));
}

LinkMutationResult failed(OutcomeCode code, std::string detail_text) {
  LinkMutationResult result;
  result.outcome = failure(code, std::move(detail_text));
  return result;
}

bool advance(GlobalGeneration& generation) {
  const auto next = generation.next();
  if (!next.has_value()) {
    return false;
  }
  generation = *next;
  return true;
}

bool advance(LinkStateGeneration& generation) {
  const auto next = generation.next();
  if (!next.has_value()) {
    return false;
  }
  generation = *next;
  return true;
}

bool advance(PublisherGeneration& generation) {
  const auto next = generation.next();
  if (!next.has_value()) {
    return false;
  }
  generation = *next;
  return true;
}

bool advance(EvidenceGeneration& generation) {
  const auto next = generation.next();
  if (!next.has_value()) {
    return false;
  }
  generation = *next;
  return true;
}

std::string record_id_for(const LinkId& link) {
  std::string value = "lsr.";
  value += link.value();
  return value;
}

std::string transition_id_for(const LinkId& link, LinkStateGeneration generation) {
  std::string value = "st.";
  value += link.value();
  value.push_back('.');
  value += generation.to_string();
  return value;
}

std::string publication_id_for(const char* prefix, std::uint64_t sequence) {
  std::string value = "pub.";
  value += prefix;
  value.push_back('.');
  value += format_u64(sequence);
  return value;
}

bool same_structure(const TopologyBinding& lhs, const TopologyBinding& rhs) {
  return lhs.fabric == rhs.fabric && lhs.site == rhs.site && lhs.link == rhs.link &&
         lhs.link_class == rhs.link_class && lhs.symmetry == rhs.symmetry &&
         lhs.endpoints == rhs.endpoints && lhs.member_links == rhs.member_links &&
         lhs.backing_links == rhs.backing_links;
}

bool backing_stale(const LinkStateEngine::Impl& impl, const TopologyBinding& binding,
                   std::string& detail) {
  for (const BackingReference& backing : binding.backing_links) {
    const auto it = impl.store.links.find(backing.link);
    if (it == impl.store.links.end()) {
      detail = "backing link ";
      detail += backing.link.value();
      detail += " is not bound in this engine";
      return true;
    }
    if (it->second.record.state == LinkOperationalState::Retired) {
      detail = "backing link ";
      detail += backing.link.value();
      detail += " is retired";
      return true;
    }
    const TopologyGeneration current = it->second.record.binding.topology_generation;
    if (current != backing.generation) {
      detail = "backing link ";
      detail += backing.link.value();
      detail += " was bound at topology generation ";
      detail += backing.generation.to_string();
      detail += " but is now at generation ";
      detail += current.to_string();
      return true;
    }
  }
  return false;
}

void sort_links(std::vector<LinkId>& links) {
  std::sort(links.begin(), links.end());
  links.erase(std::unique(links.begin(), links.end()), links.end());
}

bool valid_binding(const TopologyBinding& binding, std::string& error) {
  if (!binding.link.valid()) {
    error = "link identity is missing or malformed";
    return false;
  }
  if (!binding.endpoints.local.valid()) {
    error = "local endpoint identity is missing or malformed";
    return false;
  }
  if (binding.topology_generation.is_zero()) {
    error = "topology generation must be greater than zero";
    return false;
  }
  for (const LinkId& member : binding.member_links) {
    if (!member.valid()) {
      error = "member link identity is malformed";
      return false;
    }
  }
  for (const BackingReference& backing : binding.backing_links) {
    if (!backing.link.valid()) {
      error = "backing link identity is malformed";
      return false;
    }
    if (backing.generation.is_zero()) {
      error = "backing link generation must be greater than zero";
      return false;
    }
  }
  return true;
}

void index_link(EngineStore& store, const LinkId& link, LinkOperationalState state) {
  store.by_state[state].insert(link);
}

void reindex_state(EngineStore& store, const LinkId& link, LinkOperationalState from,
                   LinkOperationalState to) {
  if (from == to) {
    return;
  }
  const auto it = store.by_state.find(from);
  if (it != store.by_state.end()) {
    it->second.erase(link);
    if (it->second.empty()) {
      store.by_state.erase(it);
    }
  }
  store.by_state[to].insert(link);
}

void index_endpoints(EngineStore& store, const TopologyBinding& binding, bool add) {
  const EndpointId endpoints[2] = {binding.endpoints.local, binding.endpoints.remote};
  for (const EndpointId& endpoint : endpoints) {
    if (!endpoint.valid()) {
      continue;
    }
    if (add) {
      store.by_endpoint[endpoint].insert(binding.link);
    } else {
      const auto it = store.by_endpoint.find(endpoint);
      if (it != store.by_endpoint.end()) {
        it->second.erase(binding.link);
        if (it->second.empty()) {
          store.by_endpoint.erase(it);
        }
      }
    }
  }
}

void push_disposition(LinkEntry& entry, const EvidenceRecord& record, EvidenceStatus status,
                      const std::string& reason, LinkStateEngine::Impl& impl) {
  EvidenceDisposition disposition;
  disposition.observation = record.observation;
  disposition.source = record.key.source;
  disposition.publisher = record.publisher;
  disposition.worker_boot = record.worker_boot;
  disposition.kind = record.key.kind;
  disposition.direction = record.key.direction;
  disposition.claim = record.claim;
  disposition.evidence_class = record.evidence_class;
  disposition.status = status;
  disposition.precedence = evidence_precedence(record.key.kind);
  disposition.counted = false;
  disposition.reason = reason;
  entry.dispositions.insert(entry.dispositions.begin(), std::move(disposition));
  const std::size_t cap = impl.config.max_history_per_link;
  if (cap == 0) {
    entry.dispositions.clear();
  } else if (entry.dispositions.size() > cap) {
    entry.dispositions.resize(cap);
  }
}

void erase_observation(EngineStore& store, const EvidenceRecord& record) {
  const auto it = store.observations.find(record.observation);
  if (it != store.observations.end() && it->second == record.key) {
    store.observations.erase(it);
  }
}

void push_history(LinkEntry& entry, const StateHistoryEntry& history, std::size_t cap) {
  if (cap == 0) {
    return;
  }
  entry.record.history.push_back(history);
  if (entry.record.history.size() > cap) {
    const std::size_t excess = entry.record.history.size() - cap;
    entry.record.history.erase(entry.record.history.begin(),
                               entry.record.history.begin() + static_cast<std::ptrdiff_t>(excess));
  }
}

OutcomeCode authorize(EngineStore& store, const PublisherId& publisher,
                      const WorkerBootId& worker_boot, const SourceId& source,
                      CoordinatorEpoch request_epoch, CoordinatorEpoch engine_epoch,
                      const LinkId& link, const TopologyBinding& binding, std::string& error) {
  const auto it = store.publishers.find(publisher);
  if (it == store.publishers.end()) {
    error = "publisher is not registered with this engine";
    return OutcomeCode::UnauthorizedPublisher;
  }
  const PublisherAuthority& authority = it->second;
  if (request_epoch != engine_epoch || authority.epoch != engine_epoch) {
    error = "coordinator epoch ";
    error += request_epoch.to_string();
    error += " is not the current epoch ";
    error += engine_epoch.to_string();
    return OutcomeCode::StaleCoordinatorEpoch;
  }
  if (authority.worker_boot != worker_boot) {
    error = "worker boot ";
    error += worker_boot.value();
    error += " was superseded by ";
    error += authority.worker_boot.value();
    return OutcomeCode::StaleAuthority;
  }
  switch (authority.status) {
    case AuthorityStatus::Active:
      break;
    case AuthorityStatus::Fenced:
      error = "publisher incarnation is fenced";
      return OutcomeCode::StaleAuthority;
    case AuthorityStatus::Superseded:
      error = "publisher incarnation was superseded by a newer boot";
      return OutcomeCode::StaleAuthority;
    case AuthorityStatus::EpochSuperseded:
      error = "publisher incarnation belongs to a superseded coordinator epoch";
      return OutcomeCode::StaleCoordinatorEpoch;
  }
  if (source.valid() && authority.source.valid() && source != authority.source) {
    error = "source ";
    error += source.value();
    error += " is not the registered source of publisher ";
    error += publisher.value();
    return OutcomeCode::UnauthorizedScope;
  }
  if (authority.scope.fabric.valid() && binding.fabric.valid() &&
      authority.scope.fabric != binding.fabric) {
    error = "publisher authority does not cover fabric ";
    error += binding.fabric.value();
    return OutcomeCode::UnauthorizedScope;
  }
  if (!authority.scope.covers(link)) {
    error = "publisher authority does not cover link ";
    error += link.value();
    return OutcomeCode::UnauthorizedScope;
  }
  return OutcomeCode::Committed;
}

std::vector<const EvidenceRecord*> current_pointers(const LinkEntry& entry) {
  std::vector<const EvidenceRecord*> pointers;
  pointers.reserve(entry.current.size());
  for (const EvidenceRecord& record : entry.current) {
    pointers.push_back(&record);
  }
  return pointers;
}

LinkStateView make_view(const LinkEntry& entry) {
  LinkStateView view;
  view.record = entry.record;
  view.current_evidence = entry.current.size();
  view.retained_dispositions = entry.dispositions.size();
  view.conflicted = entry.conflicted;
  view.synthetic_backed = entry.synthetic_backed;
  return view;
}

PreparedCommit prepare_commit(const LinkStateEngine::Impl& impl, const LinkEntry& entry,
                              std::span<const EvidenceRecord* const> prospective,
                              TransitionTrigger trigger) {
  PreparedCommit prepared;
  prepared.resolution = resolve_link_state(prospective, entry.record);

  const bool decisive =
      prepared.resolution.result_class != StateResultClass::Conflicted &&
      prepared.resolution.result_class != StateResultClass::RevalidationRequired &&
      prepared.resolution.result_class != StateResultClass::AuthoritativeUnknown &&
      prepared.resolution.result_class != StateResultClass::Retired;

  TransitionTrigger effective = trigger;
  if (decisive) {
    effective = TransitionTrigger::EvidenceResolution;
  } else if (prepared.resolution.result_class == StateResultClass::AuthoritativeUnknown) {
    effective = TransitionTrigger::EvidenceResolution;
  }
  prepared.effective_trigger = effective;
  prepared.target_state = prepared.resolution.state;

  if (entry.record.state == LinkOperationalState::Retired) {
    prepared.code = OutcomeCode::RetiredLink;
    prepared.detail = "link state record is retired and terminal";
    prepared.ok = false;
    return prepared;
  }

  const TransitionLegality legality =
      evaluate_transition(entry.record.state, prepared.target_state, effective);
  switch (legality) {
    case TransitionLegality::Legal:
    case TransitionLegality::SameState:
      prepared.ok = true;
      prepared.code = OutcomeCode::Committed;
      prepared.state_changed = legality == TransitionLegality::Legal;
      if (prepared.state_changed &&
          (!entry.record.state_generation.next().has_value() ||
           !impl.global_state_generation.next().has_value())) {
        prepared.ok = false;
        prepared.code = OutcomeCode::ResourceLimit;
        prepared.detail = "state generation counter is exhausted";
      }
      break;
    case TransitionLegality::FromRetired:
      prepared.ok = false;
      prepared.code = OutcomeCode::RetiredLink;
      prepared.detail = "retired link state records never return to a live state";
      break;
    case TransitionLegality::RetirementRequired:
      prepared.ok = false;
      prepared.code = OutcomeCode::InvalidTransition;
      prepared.detail = "only an explicit retirement may retire a link state record";
      break;
    case TransitionLegality::TriggerNotPermitted:
    case TransitionLegality::Illegal:
      prepared.ok = false;
      prepared.code = OutcomeCode::InvalidTransition;
      prepared.detail = "transition ";
      prepared.detail += linkstate::to_string(entry.record.state);
      prepared.detail += " -> ";
      prepared.detail += linkstate::to_string(prepared.target_state);
      prepared.detail += " is not permitted for trigger ";
      prepared.detail += linkstate::to_string(effective);
      break;
  }
  return prepared;
}

void commit_prepared(LinkStateEngine::Impl& impl, LinkEntry& entry, PreparedCommit& prepared,
                     const PublicationId& publication, const PublisherId& publisher) {
  entry.conflicted = prepared.resolution.conflicted;
  entry.synthetic_backed = prepared.resolution.synthetic_backed;
  // The retained disposition ring is deliberately not overwritten here: it
  // records why evidence stopped being current (superseded, withdrawn, fenced,
  // stale generation) and must survive the commit that invalidated it.

  entry.record.provenance_digest = digest_evidence_set(entry.current);
  entry.record.last_publication = publication;
  entry.record.last_publisher = publisher;
  entry.record.result_class = prepared.resolution.result_class;
  entry.record.degradation_cause = prepared.resolution.cause;

  const LinkOperationalState previous = entry.record.state;
  if (prepared.state_changed) {
    StateHistoryEntry history;
    history.state_generation = entry.record.state_generation;
    history.from_state = previous;
    history.to_state = prepared.target_state;
    history.trigger = prepared.effective_trigger;
    history.result_class = prepared.resolution.result_class;
    history.cause = prepared.resolution.cause;
    history.publication = publication;
    history.publisher = publisher;
    history.evidence_generation = entry.record.evidence_generation;
    history.topology_generation = entry.record.binding.topology_generation;
    history.outcome = OutcomeCode::Committed;

    if (!advance(entry.record.state_generation) || !advance(impl.global_state_generation)) {
      return;
    }
    entry.record.state = prepared.target_state;
    if (prepared.target_state != LinkOperationalState::Unknown &&
        prepared.target_state != LinkOperationalState::RevalidationRequired) {
      entry.record.ever_established = true;
    }
    reindex_state(impl.store, entry.record.binding.link, previous, prepared.target_state);
    push_history(entry, history, impl.config.max_history_per_link);
    ++impl.counters.transitions;
  } else {
    entry.record.state = prepared.target_state;
  }
  entry.record.last_transition =
      StateTransitionId::from_validated(transition_id_for(entry.record.binding.link,
                                                          entry.record.state_generation));
  entry.record.last_outcome = prepared.code;
}

OutcomeCode recompute_link(LinkStateEngine::Impl& impl, LinkEntry& entry,
                           TransitionTrigger trigger, const PublicationId& publication,
                           const PublisherId& publisher, bool& state_changed) {
  const std::vector<const EvidenceRecord*> pointers = current_pointers(entry);
  PreparedCommit prepared = prepare_commit(impl, entry, pointers, trigger);
  if (!prepared.ok) {
    state_changed = false;
    return prepared.code;
  }
  commit_prepared(impl, entry, prepared, publication, publisher);
  state_changed = prepared.state_changed;
  return OutcomeCode::Committed;
}

}  // namespace linkstate::detail
