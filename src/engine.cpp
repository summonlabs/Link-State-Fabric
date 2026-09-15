#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine_internal.hpp"
#include "engine_support.hpp"
#include "text.hpp"

namespace linkstate {
namespace {

using detail::LinkEntry;

LinkMutationResult result_for(OutcomeCode code, std::string detail_text, const LinkEntry& entry,
                              bool state_changed) {
  LinkMutationResult result;
  result.outcome = detail::failure(code, std::move(detail_text));
  result.outcome.state_generation = entry.record.state_generation;
  result.outcome.global_generation = entry.record.global_generation;
  result.outcome.evidence_generation = entry.record.evidence_generation;
  result.outcome.state_changed = state_changed;
  result.view = detail::make_view(entry);
  return result;
}

LinkMutationResult plain_result(OutcomeCode code, std::string detail_text) {
  LinkMutationResult result;
  result.outcome = detail::failure(code, std::move(detail_text));
  return result;
}

void index_dependents(detail::EngineStore& store, const TopologyBinding& binding, bool add) {
  for (const BackingReference& backing : binding.backing_links) {
    if (add) {
      store.dependents[backing.link].insert(binding.link);
    } else {
      const auto it = store.dependents.find(backing.link);
      if (it != store.dependents.end()) {
        it->second.erase(binding.link);
        if (it->second.empty()) {
          store.dependents.erase(it);
        }
      }
    }
  }
}

void clear_evidence(LinkEntry& entry, LinkStateEngine::Impl& impl, EvidenceStatus status,
                    const std::string& reason) {
  for (const EvidenceRecord& record : entry.current) {
    detail::push_disposition(entry, record, status, reason, impl);
    detail::erase_observation(impl.store, record);
  }
  entry.current.clear();
}

/// Invalidates the evidence of every link that pins a backing reference to the
/// given link when that reference is no longer current. Logical links are never
/// silently re-derived from a replaced physical relationship.
void invalidate_stale_dependents(LinkStateEngine::Impl& impl, const LinkId& backing_link) {
  const auto it = impl.store.dependents.find(backing_link);
  if (it == impl.store.dependents.end()) {
    return;
  }
  std::vector<LinkId> dependents(it->second.begin(), it->second.end());
  std::sort(dependents.begin(), dependents.end());
  for (const LinkId& dependent : dependents) {
    const auto entry_it = impl.store.links.find(dependent);
    if (entry_it == impl.store.links.end()) {
      continue;
    }
    LinkEntry& entry = entry_it->second;
    if (entry.record.state == LinkOperationalState::Retired || entry.current.empty()) {
      continue;
    }
    std::string reason;
    if (!detail::backing_stale(impl, entry.record.binding, reason)) {
      continue;
    }
    clear_evidence(entry, impl, EvidenceStatus::StaleTopology, reason);
    if (!detail::advance(entry.record.evidence_generation) ||
        !detail::advance(impl.global_generation)) {
      continue;
    }
    entry.record.global_generation = impl.global_generation;
    bool state_changed = false;
    const PublicationId publication = PublicationId::from_validated(
        detail::publication_id_for("backing", impl.global_generation.value()));
    (void)detail::recompute_link(impl, entry, TransitionTrigger::TopologyInvalidation, publication,
                                 PublisherId{}, state_changed);
  }
}

/// True when a worker incarnation was fenced and can never become active again.
bool boot_is_fenced(const LinkStateEngine::Impl& impl, const PublisherId& publisher,
                    const WorkerBootId& boot) {
  const auto it = impl.store.fenced_boots.find(publisher);
  if (it == impl.store.fenced_boots.end()) {
    return false;
  }
  return std::find(it->second.begin(), it->second.end(), boot) != it->second.end();
}

void remember_fenced_boot(LinkStateEngine::Impl& impl, const PublisherId& publisher,
                          const WorkerBootId& boot) {
  std::deque<WorkerBootId>& registry = impl.store.fenced_boots[publisher];
  if (std::find(registry.begin(), registry.end(), boot) != registry.end()) {
    return;
  }
  registry.push_back(boot);
  const std::size_t cap = impl.config.fence_retention_per_publisher;
  while (registry.size() > cap) {
    registry.pop_front();
  }
}

/// Fences a publisher incarnation and invalidates the evidence it owns.
void fence_publisher(LinkStateEngine::Impl& impl, const PublisherId& publisher,
                     const WorkerBootId& boot, bool all_boots, AuthorityStatus status,
                     const std::string& reason) {
  if (boot.valid()) {
    remember_fenced_boot(impl, publisher, boot);
  }
  const auto authority_it = impl.store.publishers.find(publisher);
  if (authority_it != impl.store.publishers.end()) {
    authority_it->second.status = status;
  }
  PublisherFence fence;
  fence.publisher = publisher;
  fence.worker_boot = boot;
  fence.epoch = impl.epoch;
  fence.reason = reason;
  impl.store.fences.push_back(std::move(fence));
  const std::size_t fence_cap = impl.config.max_publishers;
  if (impl.store.fences.size() > fence_cap) {
    impl.store.fences.erase(impl.store.fences.begin(),
                            impl.store.fences.begin() +
                                static_cast<std::ptrdiff_t>(impl.store.fences.size() - fence_cap));
  }

  const auto links_it = impl.store.publisher_links.find(publisher);
  if (links_it == impl.store.publisher_links.end()) {
    return;
  }
  std::vector<LinkId> links(links_it->second.begin(), links_it->second.end());
  std::sort(links.begin(), links.end());
  for (const LinkId& link : links) {
    const auto entry_it = impl.store.links.find(link);
    if (entry_it == impl.store.links.end()) {
      continue;
    }
    LinkEntry& entry = entry_it->second;
    bool removed = false;
    for (auto record_it = entry.current.begin(); record_it != entry.current.end();) {
      if (record_it->publisher == publisher && (all_boots || record_it->worker_boot == boot)) {
        const std::string disposition_reason =
            reason + " (worker boot " + record_it->worker_boot.value() + ")";
        detail::push_disposition(entry, *record_it, EvidenceStatus::StaleAuthority,
                                 disposition_reason, impl);
        detail::erase_observation(impl.store, *record_it);
        record_it = entry.current.erase(record_it);
        removed = true;
      } else {
        ++record_it;
      }
    }
    if (!removed) {
      continue;
    }
    if (!detail::advance(entry.record.evidence_generation) ||
        !detail::advance(impl.global_generation)) {
      continue;
    }
    entry.record.global_generation = impl.global_generation;
    bool state_changed = false;
    const PublicationId publication = PublicationId::from_validated(
        detail::publication_id_for("fence", impl.global_generation.value()));
    (void)detail::recompute_link(impl, entry, TransitionTrigger::PublisherFenced, publication,
                                 PublisherId{}, state_changed);
  }

  for (const LinkId& link : links) {
    const auto entry_it = impl.store.links.find(link);
    bool remains = false;
    if (entry_it != impl.store.links.end()) {
      for (const EvidenceRecord& record : entry_it->second.current) {
        if (record.publisher == publisher) {
          remains = true;
          break;
        }
      }
    }
    if (!remains) {
      links_it->second.erase(link);
    }
  }
  if (links_it->second.empty()) {
    impl.store.publisher_links.erase(links_it);
  }
}

/// Applies a structural rebinding: evidence that no longer describes the
/// current structure is invalidated before the new binding becomes durable.
LinkMutationResult apply_rebinding(LinkStateEngine::Impl& impl, LinkEntry& entry,
                                   TopologyBinding binding, const std::string& reason) {
  const TopologyGeneration previous_generation = entry.record.binding.topology_generation;
  for (auto it = entry.current.begin(); it != entry.current.end();) {
    bool current = it->topology_generation == binding.topology_generation;
    if (current && it->key.endpoint.valid()) {
      if (it->key.endpoint == binding.endpoints.local) {
        current = it->endpoint_generation == binding.endpoints.local_generation;
      } else if (binding.endpoints.remote.valid() &&
                 it->key.endpoint == binding.endpoints.remote) {
        current = it->endpoint_generation == binding.endpoints.remote_generation;
      } else {
        current = false;
      }
    } else if (current) {
      current = it->endpoint_generation.is_zero();
    }
    if (current) {
      ++it;
      continue;
    }
    std::string detail_text = "bound to topology generation ";
    detail_text += it->topology_generation.to_string();
    detail_text += ", current generation is ";
    detail_text += binding.topology_generation.to_string();
    detail::push_disposition(entry, *it, EvidenceStatus::StaleTopology, detail_text, impl);
    detail::erase_observation(impl.store, *it);
    it = entry.current.erase(it);
  }

  detail::index_endpoints(impl.store, entry.record.binding, false);
  index_dependents(impl.store, entry.record.binding, false);
  entry.record.binding = std::move(binding);
  detail::index_endpoints(impl.store, entry.record.binding, true);
  index_dependents(impl.store, entry.record.binding, true);

  if (!detail::advance(entry.record.evidence_generation) ||
      !detail::advance(impl.global_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "generation counter is exhausted");
  }
  entry.record.global_generation = impl.global_generation;
  if (impl.topology_generation < entry.record.binding.topology_generation) {
    impl.topology_generation = entry.record.binding.topology_generation;
  }
  const PublicationId publication = PublicationId::from_validated(
      detail::publication_id_for("topology", impl.global_generation.value()));
  bool state_changed = false;
  const OutcomeCode code = detail::recompute_link(
      impl, entry, TransitionTrigger::TopologyInvalidation, publication, PublisherId{},
      state_changed);
  if (code != OutcomeCode::Committed) {
    return detail::failed(code, "topology supersession could not be committed");
  }
  LinkMutationResult result = result_for(OutcomeCode::Committed, reason, entry, state_changed);
  result.outcome.detail += " (topology generation ";
  result.outcome.detail += previous_generation.to_string();
  result.outcome.detail += " -> ";
  result.outcome.detail += entry.record.binding.topology_generation.to_string();
  result.outcome.detail.push_back(')');
  invalidate_stale_dependents(impl, entry.record.binding.link);
  return result;
}

}  // namespace

LinkStateEngine::Impl::Impl(EngineConfig engine_config)
    : config(std::move(engine_config)),
      epoch(config.initial_epoch),
      topology_generation(config.initial_topology_generation) {}

LinkStateEngine::LinkStateEngine(EngineConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

LinkStateEngine::~LinkStateEngine() = default;
LinkStateEngine::LinkStateEngine(LinkStateEngine&&) noexcept = default;
LinkStateEngine& LinkStateEngine::operator=(LinkStateEngine&&) noexcept = default;

const EngineConfig& LinkStateEngine::config() const noexcept { return impl_->config; }

LinkMutationResult LinkStateEngine::bind_link(const BindLinkRequest& request) {
  std::unique_lock lock(impl_->mutex);
  TopologyBinding binding = request.binding;
  detail::sort_links(binding.member_links);
  std::sort(binding.backing_links.begin(), binding.backing_links.end());
  binding.backing_links.erase(
      std::unique(binding.backing_links.begin(), binding.backing_links.end()),
      binding.backing_links.end());

  std::string error;
  if (!detail::valid_binding(binding, error)) {
    return detail::failed(OutcomeCode::MalformedRequest, error);
  }

  const auto it = impl_->store.links.find(binding.link);
  if (it == impl_->store.links.end()) {
    if (impl_->store.links.size() >= impl_->config.max_links) {
      return detail::failed(OutcomeCode::ResourceLimit, "link capacity of this engine is exhausted");
    }
    LinkEntry entry;
    entry.record.record = LinkStateRecordId::from_validated(detail::record_id_for(binding.link));
    entry.record.binding = std::move(binding);
    entry.record.state = LinkOperationalState::Unknown;
    entry.record.result_class = StateResultClass::AuthoritativeUnknown;
    entry.record.durable = true;
    entry.record.provenance_digest = digest_evidence_set({});
    if (!detail::advance(impl_->global_generation)) {
      return detail::failed(OutcomeCode::ResourceLimit, "global generation counter is exhausted");
    }
    entry.record.global_generation = impl_->global_generation;
    entry.record.last_transition = StateTransitionId::from_validated(
        detail::transition_id_for(entry.record.binding.link, entry.record.state_generation));
    const LinkId link = entry.record.binding.link;
    const TopologyBinding bound = entry.record.binding;
    auto inserted = impl_->store.links.emplace(link, std::move(entry));
    detail::index_link(impl_->store, link, LinkOperationalState::Unknown);
    detail::index_endpoints(impl_->store, bound, true);
    index_dependents(impl_->store, bound, true);
    if (impl_->topology_generation < bound.topology_generation) {
      impl_->topology_generation = bound.topology_generation;
    }
    return result_for(OutcomeCode::Committed, std::string(), inserted.first->second, false);
  }

  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    return detail::failed(OutcomeCode::RetiredLink,
                          "link state record is retired; structural rebinding requires a new link "
                          "identity or an explicit topology supersession");
  }
  const TopologyGeneration current = entry.record.binding.topology_generation;
  if (binding.topology_generation < current) {
    std::string detail_text = "topology generation ";
    detail_text += binding.topology_generation.to_string();
    detail_text += " is older than the bound generation ";
    detail_text += current.to_string();
    return detail::failed(OutcomeCode::StaleTopology, detail_text);
  }
  if (binding.topology_generation == current) {
    if (detail::same_structure(entry.record.binding, binding)) {
      return result_for(OutcomeCode::Idempotent, std::string(), entry, false);
    }
    return detail::failed(OutcomeCode::MalformedRequest,
                          "structural change requires a new topology generation");
  }
  return apply_rebinding(*impl_, entry, std::move(binding), "topology generation advanced");
}

std::vector<LinkMutationResult> LinkStateEngine::bind_links(
    std::span<const BindLinkRequest> requests) {
  std::vector<LinkMutationResult> results;
  if (requests.size() > impl_->config.max_batch_size) {
    results.push_back(detail::failed(OutcomeCode::ResourceLimit, "batch size exceeds the limit"));
    return results;
  }
  results.reserve(requests.size());
  for (const BindLinkRequest& request : requests) {
    results.push_back(bind_link(request));
  }
  return results;
}

LinkMutationResult LinkStateEngine::supersede_topology(const SupersedeTopologyRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.link.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest, "link identity is malformed");
  }
  const auto it = impl_->store.links.find(request.link);
  if (it == impl_->store.links.end()) {
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    return detail::failed(OutcomeCode::RetiredLink, "link state record is retired and terminal");
  }
  if (!(entry.record.binding.topology_generation < request.new_topology_generation)) {
    std::string detail_text = "new topology generation ";
    detail_text += request.new_topology_generation.to_string();
    detail_text += " must be greater than the bound generation ";
    detail_text += entry.record.binding.topology_generation.to_string();
    return detail::failed(OutcomeCode::StaleTopology, detail_text);
  }
  TopologyBinding binding = entry.record.binding;
  if (request.replacement.has_value()) {
    binding = *request.replacement;
    detail::sort_links(binding.member_links);
    std::sort(binding.backing_links.begin(), binding.backing_links.end());
    binding.backing_links.erase(
        std::unique(binding.backing_links.begin(), binding.backing_links.end()),
        binding.backing_links.end());
    std::string error;
    if (!detail::valid_binding(binding, error)) {
      return detail::failed(OutcomeCode::MalformedRequest, error);
    }
    if (binding.link != request.link) {
      return detail::failed(OutcomeCode::MalformedRequest,
                            "replacement binding describes a different link identity");
    }
  }
  binding.topology_generation = request.new_topology_generation;
  std::string reason = "topology supersession";
  if (!request.reason.empty()) {
    reason += ": ";
    reason += request.reason;
  }
  return apply_rebinding(*impl_, entry, std::move(binding), reason);
}

std::vector<LinkMutationResult> LinkStateEngine::supersede_topology_batch(
    std::span<const SupersedeTopologyRequest> requests) {
  std::vector<LinkMutationResult> results;
  if (requests.size() > impl_->config.max_batch_size) {
    results.push_back(detail::failed(OutcomeCode::ResourceLimit, "batch size exceeds the limit"));
    return results;
  }
  results.reserve(requests.size());
  for (const SupersedeTopologyRequest& request : requests) {
    results.push_back(supersede_topology(request));
  }
  return results;
}

LinkMutationResult LinkStateEngine::retire_link(const RetireLinkRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.link.valid() || !request.publication.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "link identity and publication identity are required");
  }
  const auto it = impl_->store.links.find(request.link);
  if (it == impl_->store.links.end()) {
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  // Authorization is evaluated before any state dependent outcome so that an
  // unauthorized caller cannot use the retirement surface to probe link state.
  std::string error;
  const OutcomeCode authorized = detail::authorize(
      impl_->store, request.publisher, request.worker_boot, SourceId{}, request.epoch, impl_->epoch,
      request.link, entry.record.binding, error);
  if (authorized != OutcomeCode::Committed) {
    return detail::failed(authorized, error);
  }
  if (entry.record.state == LinkOperationalState::Retired) {
    return result_for(OutcomeCode::Idempotent, "link state record is already retired", entry, false);
  }
  if (request.topology_generation < entry.record.binding.topology_generation) {
    return detail::failed(OutcomeCode::StaleTopology,
                          "retirement refers to an older topology generation");
  }
  const TransitionLegality legality = evaluate_transition(
      entry.record.state, LinkOperationalState::Retired, TransitionTrigger::Retirement);
  if (legality != TransitionLegality::Legal) {
    return detail::failed(OutcomeCode::InvalidTransition,
                          "retirement is not permitted from the current state");
  }

  const LinkOperationalState previous = entry.record.state;
  clear_evidence(entry, *impl_, EvidenceStatus::Superseded,
                 "link state record retired by explicit retirement");
  if (!detail::advance(entry.record.evidence_generation) ||
      !detail::advance(impl_->global_generation) ||
      !detail::advance(entry.record.state_generation) ||
      !detail::advance(impl_->global_state_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "generation counter is exhausted");
  }
  entry.record.global_generation = impl_->global_generation;
  entry.record.provenance_digest = digest_evidence_set(entry.current);

  StateHistoryEntry history;
  history.state_generation = entry.record.state_generation;
  history.from_state = previous;
  history.to_state = LinkOperationalState::Retired;
  history.trigger = TransitionTrigger::Retirement;
  history.result_class = StateResultClass::Retired;
  history.publication = request.publication;
  history.publisher = request.publisher;
  history.evidence_generation = entry.record.evidence_generation;
  history.topology_generation = entry.record.binding.topology_generation;
  history.outcome = OutcomeCode::Committed;
  detail::push_history(entry, history, impl_->config.max_history_per_link);

  detail::reindex_state(impl_->store, entry.record.binding.link, previous,
                        LinkOperationalState::Retired);
  entry.record.state = LinkOperationalState::Retired;
  entry.record.result_class = StateResultClass::Retired;
  entry.record.degradation_cause = DegradationCause::None;
  entry.record.ever_established = false;
  entry.record.last_publication = request.publication;
  entry.record.last_publisher = request.publisher;
  entry.record.last_outcome = OutcomeCode::Committed;
  entry.record.last_transition = StateTransitionId::from_validated(
      detail::transition_id_for(entry.record.binding.link, entry.record.state_generation));
  entry.conflicted = false;
  ++impl_->counters.transitions;

  invalidate_stale_dependents(*impl_, entry.record.binding.link);
  std::string reason = "link state record retired";
  if (!request.reason.empty()) {
    reason += ": ";
    reason += request.reason;
  }
  return result_for(OutcomeCode::Committed, reason, entry, true);
}

LinkMutationResult LinkStateEngine::register_publisher(const RegisterPublisherRequest& request) {
  std::unique_lock lock(impl_->mutex);
  PublisherRegistration registration = request.registration;
  if (!registration.publisher.valid() || !registration.worker_boot.valid() ||
      !registration.source.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "publisher, worker boot and source identities are required");
  }
  if (!registration.scope.normalize()) {
    return detail::failed(OutcomeCode::ResourceLimit,
                          "authority scope is empty or exceeds the maximum number of links");
  }
  if (registration.epoch != impl_->epoch) {
    std::string detail_text = "registration epoch ";
    detail_text += registration.epoch.to_string();
    detail_text += " is not the current coordinator epoch ";
    detail_text += impl_->epoch.to_string();
    return detail::failed(OutcomeCode::StaleCoordinatorEpoch, detail_text);
  }

  if (boot_is_fenced(*impl_, registration.publisher, registration.worker_boot)) {
    std::string detail_text = "worker boot ";
    detail_text += registration.worker_boot.value();
    detail_text +=
        " was fenced and can never become active again; a restarted worker must present a fresh "
        "boot identity";
    return detail::failed(OutcomeCode::StaleAuthority, detail_text);
  }

  const auto it = impl_->store.publishers.find(registration.publisher);
  if (it != impl_->store.publishers.end()) {
    PublisherAuthority& authority = it->second;
    if (authority.worker_boot == registration.worker_boot) {
      if (!detail::advance(authority.generation)) {
        return detail::failed(OutcomeCode::ResourceLimit, "publisher generation is exhausted");
      }
      authority.scope = std::move(registration.scope);
      authority.source = registration.source;
      return plain_result(OutcomeCode::Committed, "publisher incarnation refreshed");
    }
    const WorkerBootId previous_boot = authority.worker_boot;
    std::string reason = "superseded by worker boot ";
    reason += registration.worker_boot.value();
    fence_publisher(*impl_, registration.publisher, previous_boot, true,
                    AuthorityStatus::Superseded, reason);
    PublisherAuthority fresh;
    fresh.publisher = registration.publisher;
    fresh.worker_boot = registration.worker_boot;
    fresh.source = registration.source;
    fresh.scope = std::move(registration.scope);
    fresh.epoch = impl_->epoch;
    fresh.generation = authority.generation;
    if (!detail::advance(fresh.generation)) {
      return detail::failed(OutcomeCode::ResourceLimit, "publisher generation is exhausted");
    }
    it->second = std::move(fresh);
    return plain_result(OutcomeCode::Committed,
                        "worker incarnation superseded; previous evidence is no longer current");
  }

  if (impl_->store.publishers.size() >= impl_->config.max_publishers) {
    return detail::failed(OutcomeCode::ResourceLimit, "publisher capacity is exhausted");
  }
  PublisherAuthority authority;
  authority.publisher = registration.publisher;
  authority.worker_boot = registration.worker_boot;
  authority.source = registration.source;
  authority.scope = std::move(registration.scope);
  authority.epoch = impl_->epoch;
  authority.generation = PublisherGeneration::from_value(1);
  authority.status = AuthorityStatus::Active;
  impl_->store.publishers.emplace(registration.publisher, std::move(authority));
  return plain_result(OutcomeCode::Committed, "publisher incarnation registered");
}

LinkMutationResult LinkStateEngine::invalidate_publisher(
    const InvalidatePublisherRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.publisher.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest, "publisher identity is malformed");
  }
  const auto it = impl_->store.publishers.find(request.publisher);
  if (it == impl_->store.publishers.end()) {
    return detail::failed(OutcomeCode::UnauthorizedPublisher,
                          "publisher is not registered with this engine");
  }
  const PublisherAuthority& authority = it->second;
  if (request.worker_boot.has_value() && *request.worker_boot != authority.worker_boot) {
    return detail::failed(OutcomeCode::StaleAuthority,
                          "the requested worker boot was already superseded");
  }
  if (authority.status != AuthorityStatus::Active) {
    return plain_result(OutcomeCode::Idempotent, "publisher incarnation is already fenced");
  }
  if (request.epoch != impl_->epoch) {
    return detail::failed(OutcomeCode::StaleCoordinatorEpoch,
                          "invalidation epoch is not the current coordinator epoch");
  }
  std::string reason = "publisher invalidated";
  if (!request.reason.empty()) {
    reason += ": ";
    reason += request.reason;
  }
  const WorkerBootId boot = authority.worker_boot;
  fence_publisher(*impl_, request.publisher, boot, true, AuthorityStatus::Fenced, reason);
  return plain_result(OutcomeCode::Committed,
                      "publisher incarnation fenced; its evidence is no longer current");
}

LinkMutationResult LinkStateEngine::advance_coordinator_epoch(const AdvanceEpochRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (request.new_epoch < impl_->epoch) {
    std::string detail_text = "coordinator epoch ";
    detail_text += request.new_epoch.to_string();
    detail_text += " is older than the current epoch ";
    detail_text += impl_->epoch.to_string();
    return detail::failed(OutcomeCode::StaleGeneration, detail_text);
  }
  if (request.new_epoch == impl_->epoch) {
    return plain_result(OutcomeCode::Idempotent, "coordinator epoch is already current");
  }
  impl_->epoch = request.new_epoch;

  std::vector<PublisherId> stale;
  stale.reserve(impl_->store.publishers.size());
  for (const auto& entry : impl_->store.publishers) {
    if (entry.second.epoch < impl_->epoch) {
      stale.push_back(entry.first);
    }
  }
  std::sort(stale.begin(), stale.end());
  for (const PublisherId& publisher : stale) {
    const auto authority_it = impl_->store.publishers.find(publisher);
    if (authority_it == impl_->store.publishers.end()) {
      continue;
    }
    std::string reason = "coordinator epoch advanced to ";
    reason += impl_->epoch.to_string();
    fence_publisher(*impl_, publisher, authority_it->second.worker_boot, true,
                    AuthorityStatus::EpochSuperseded, reason);
  }
  std::string detail_text = "coordinator epoch advanced to ";
  detail_text += impl_->epoch.to_string();
  detail_text += "; ";
  detail_text += detail::format_u64(static_cast<std::uint64_t>(stale.size()));
  detail_text += " publisher incarnations were superseded";
  return plain_result(OutcomeCode::Committed, detail_text);
}

Outcome LinkStateEngine::reconcile() {
  std::unique_lock lock(impl_->mutex);
  std::vector<LinkId> links;
  links.reserve(impl_->store.links.size());
  for (const auto& entry : impl_->store.links) {
    links.push_back(entry.first);
  }
  std::sort(links.begin(), links.end());
  std::size_t changed = 0;
  for (const LinkId& link : links) {
    const auto it = impl_->store.links.find(link);
    if (it == impl_->store.links.end()) {
      continue;
    }
    LinkEntry& entry = it->second;
    if (entry.record.state == LinkOperationalState::Retired) {
      continue;
    }
    bool state_changed = false;
    const PublicationId publication = PublicationId::from_validated(
        detail::publication_id_for("reconcile", impl_->global_generation.value()));
    const OutcomeCode code = detail::recompute_link(
        *impl_, entry, TransitionTrigger::EvidenceResolution, publication, PublisherId{},
        state_changed);
    if (code != OutcomeCode::Committed) {
      continue;
    }
    if (state_changed) {
      ++changed;
    }
  }
  std::string detail_text = "reconciled ";
  detail_text += detail::format_u64(static_cast<std::uint64_t>(links.size()));
  detail_text += " links, ";
  detail_text += detail::format_u64(static_cast<std::uint64_t>(changed));
  detail_text += " state changes";
  return Outcome::make(OutcomeCode::Committed, detail_text);
}

}  // namespace linkstate
