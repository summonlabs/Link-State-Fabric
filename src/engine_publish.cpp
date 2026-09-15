#include <algorithm>
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

bool validate_observation(const EvidenceRecord& observation, const PublicationId& publication,
                          std::string& error) {
  if (!publication.valid()) {
    error = "publication identity is required";
    return false;
  }
  if (!observation.key.link.valid() || !observation.key.source.valid()) {
    error = "evidence key link and source identities are required";
    return false;
  }
  if (!observation.observation.valid()) {
    error = "observation identity is required";
    return false;
  }
  if (!observation.publisher.valid() || !observation.worker_boot.valid()) {
    error = "publisher and worker boot identities are required";
    return false;
  }
  if (observation.evidence_class == EvidenceClass::Unsupported) {
    error = "unsupported capability evidence cannot be published";
    return false;
  }
  if (observation.claim == EvidenceClaim::Degraded &&
      observation.degradation_cause == DegradationCause::None) {
    error = "a DEGRADED claim requires a named degradation cause";
    return false;
  }
  if (observation.claim != EvidenceClaim::Degraded &&
      observation.degradation_cause != DegradationCause::None) {
    error = "a degradation cause is only valid with a DEGRADED claim";
    return false;
  }
  if (!detail::validate_text(observation.detail, limits::max_detail_length, error)) {
    return false;
  }
  if (observation.metadata.size() > limits::max_evidence_metadata_bytes) {
    error = "evidence metadata exceeds the maximum size";
    return false;
  }
  return true;
}

const EvidenceRecord* find_slot(const LinkEntry& entry, const EvidenceKey& key) {
  for (const EvidenceRecord& record : entry.current) {
    if (record.key == key) {
      return &record;
    }
  }
  return nullptr;
}

void note_rejection(LinkStateEngine::Impl& impl, const PublisherId& publisher) {
  const auto it = impl.store.publishers.find(publisher);
  if (it != impl.store.publishers.end()) {
    ++it->second.rejected_publications;
  }
  ++impl.counters.rejected_publications;
}

}  // namespace

LinkMutationResult LinkStateEngine::publish_evidence(const PublishEvidenceRequest& request) {
  std::unique_lock lock(impl_->mutex);
  const EvidenceRecord& observation = request.observation;
  std::string error;
  if (!validate_observation(observation, request.publication, error)) {
    return detail::failed(OutcomeCode::MalformedRequest, error);
  }
  const auto it = impl_->store.links.find(observation.key.link);
  if (it == impl_->store.links.end()) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::RetiredLink,
                          "link state record is retired and accepts no further evidence");
  }

  const OutcomeCode authorized = detail::authorize(
      impl_->store, observation.publisher, observation.worker_boot, observation.key.source,
      observation.epoch, impl_->epoch, observation.key.link, entry.record.binding, error);
  if (authorized != OutcomeCode::Committed) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(authorized, error);
  }

  std::string backing_detail;
  if (detail::backing_stale(*impl_, entry.record.binding, backing_detail)) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::StaleTopology, backing_detail);
  }

  if (observation.topology_generation != entry.record.binding.topology_generation) {
    note_rejection(*impl_, observation.publisher);
    std::string detail_text = "observation is bound to topology generation ";
    detail_text += observation.topology_generation.to_string();
    detail_text += ", the current generation of link ";
    detail_text += observation.key.link.value();
    detail_text += " is ";
    detail_text += entry.record.binding.topology_generation.to_string();
    return detail::failed(OutcomeCode::StaleTopology, detail_text);
  }

  if (observation.key.endpoint.valid()) {
    EndpointGeneration expected;
    bool known = false;
    if (observation.key.endpoint == entry.record.binding.endpoints.local) {
      expected = entry.record.binding.endpoints.local_generation;
      known = true;
    } else if (entry.record.binding.endpoints.remote.valid() &&
               observation.key.endpoint == entry.record.binding.endpoints.remote) {
      expected = entry.record.binding.endpoints.remote_generation;
      known = true;
    }
    if (!known) {
      note_rejection(*impl_, observation.publisher);
      return detail::failed(OutcomeCode::MalformedRequest,
                            "endpoint is not bound to this link in the current topology");
    }
    if (observation.endpoint_generation != expected) {
      note_rejection(*impl_, observation.publisher);
      std::string detail_text = "observation is bound to endpoint generation ";
      detail_text += observation.endpoint_generation.to_string();
      detail_text += ", the current generation is ";
      detail_text += expected.to_string();
      return detail::failed(OutcomeCode::StaleTopology, detail_text);
    }
  } else if (!observation.endpoint_generation.is_zero()) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::MalformedRequest,
                          "an endpoint generation requires an endpoint scoped observation");
  }

  if (request.expected_evidence_generation.has_value() &&
      request.expected_evidence_generation.value() != entry.record.evidence_generation) {
    note_rejection(*impl_, observation.publisher);
    std::string detail_text = "expected evidence generation ";
    detail_text += request.expected_evidence_generation->to_string();
    detail_text += " is not the current evidence generation ";
    detail_text += entry.record.evidence_generation.to_string();
    return detail::failed(OutcomeCode::StaleGeneration, detail_text);
  }
  if (request.expected_state_generation.has_value() &&
      request.expected_state_generation.value() != entry.record.state_generation) {
    note_rejection(*impl_, observation.publisher);
    std::string detail_text = "expected state generation ";
    detail_text += request.expected_state_generation->to_string();
    detail_text += " is not the current state generation ";
    detail_text += entry.record.state_generation.to_string();
    return detail::failed(OutcomeCode::StaleGeneration, detail_text);
  }

  const auto observation_it = impl_->store.observations.find(observation.observation);
  if (observation_it != impl_->store.observations.end() &&
      !(observation_it->second == observation.key)) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::MalformedRequest,
                          "observation identity is already used by a different evidence key");
  }

  const EvidenceRecord* slot = find_slot(entry, observation.key);
  if (slot != nullptr) {
    if (observation.source_generation < slot->source_generation) {
      note_rejection(*impl_, observation.publisher);
      std::string detail_text = "source generation ";
      detail_text += observation.source_generation.to_string();
      detail_text += " is older than the recorded generation ";
      detail_text += slot->source_generation.to_string();
      return detail::failed(OutcomeCode::StaleEvidence, detail_text);
    }
    if (observation.source_generation == slot->source_generation) {
      if (observation.sequence < slot->sequence) {
        note_rejection(*impl_, observation.publisher);
        std::string detail_text = "observation sequence ";
        detail_text += detail::format_u64(observation.sequence);
        detail_text += " is older than the recorded sequence ";
        detail_text += detail::format_u64(slot->sequence);
        return detail::failed(OutcomeCode::StaleEvidence, detail_text);
      }
      if (observation.sequence == slot->sequence) {
        if (!(observation == *slot)) {
          note_rejection(*impl_, observation.publisher);
          return detail::failed(
              OutcomeCode::StaleEvidence,
              "observation sequence is already consumed by different observation content");
        }
        return result_for(OutcomeCode::Idempotent,
                          "exact replay of an already committed observation", entry, false);
      }
    }
  }

  std::size_t purged = 0;
  for (const EvidenceRecord& record : entry.current) {
    if (record.key != observation.key && record.key.source == observation.key.source &&
        record.source_generation < observation.source_generation) {
      ++purged;
    }
  }
  if (slot == nullptr && (entry.current.size() - purged) >= impl_->config.max_evidence_per_link) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(OutcomeCode::ResourceLimit,
                          "evidence capacity of this link is exhausted");
  }

  std::vector<const EvidenceRecord*> prospective;
  prospective.reserve(entry.current.size() + 1);
  for (const EvidenceRecord& record : entry.current) {
    if (record.key == observation.key) {
      continue;
    }
    if (record.key.source == observation.key.source &&
        record.source_generation < observation.source_generation) {
      continue;
    }
    prospective.push_back(&record);
  }
  prospective.push_back(&observation);

  detail::PreparedCommit prepared = detail::prepare_commit(
      *impl_, entry, prospective, TransitionTrigger::EvidenceResolution);
  if (!prepared.ok) {
    note_rejection(*impl_, observation.publisher);
    return detail::failed(prepared.code, prepared.detail);
  }

  // From here on nothing can fail: the transaction is applied atomically.
  for (auto record_it = entry.current.begin(); record_it != entry.current.end();) {
    const bool replaced = record_it->key == observation.key;
    const bool source_superseded = !replaced &&
                                   record_it->key.source == observation.key.source &&
                                   record_it->source_generation < observation.source_generation;
    if (replaced || source_superseded) {
      detail::push_disposition(
          entry, *record_it,
          replaced ? EvidenceStatus::Superseded : EvidenceStatus::StaleSourceGeneration,
          replaced ? "superseded by a newer observation of the same evidence key"
                   : "source generation superseded by a newer incarnation of the same source",
          *impl_);
      detail::erase_observation(impl_->store, *record_it);
      record_it = entry.current.erase(record_it);
    } else {
      ++record_it;
    }
  }

  if (!detail::advance(impl_->global_generation) ||
      !detail::advance(entry.record.evidence_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "generation counter is exhausted");
  }
  EvidenceRecord stored = observation;
  stored.status = EvidenceStatus::Current;
  stored.commit_sequence = impl_->global_generation;
  stored.evidence_generation = entry.record.evidence_generation;
  stored.record_digest = stored.compute_digest();
  entry.record.global_generation = impl_->global_generation;
  entry.current.push_back(std::move(stored));
  std::sort(entry.current.begin(), entry.current.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
              return detail::evidence_less(lhs, rhs);
            });
  impl_->store.observations[observation.observation] = observation.key;
  impl_->store.publisher_links[observation.publisher].insert(observation.key.link);

  const auto authority_it = impl_->store.publishers.find(observation.publisher);
  if (authority_it != impl_->store.publishers.end()) {
    ++authority_it->second.accepted_publications;
  }
  ++impl_->counters.committed_publications;

  detail::commit_prepared(*impl_, entry, prepared, request.publication, observation.publisher);
  const OutcomeCode result_code =
      prepared.resolution.conflicted ? OutcomeCode::EvidenceConflict : OutcomeCode::Committed;
  std::string detail_text =
      result_code == OutcomeCode::EvidenceConflict
          ? "evidence committed; current evidence conflicts so the link requires revalidation"
          : std::string();
  return result_for(result_code, std::move(detail_text), entry, prepared.state_changed);
}

std::vector<LinkMutationResult> LinkStateEngine::publish_evidence_batch(
    std::span<const PublishEvidenceRequest> requests) {
  std::vector<LinkMutationResult> results;
  if (requests.size() > impl_->config.max_batch_size) {
    results.push_back(detail::failed(OutcomeCode::ResourceLimit, "batch size exceeds the limit"));
    return results;
  }
  results.reserve(requests.size());
  for (const PublishEvidenceRequest& request : requests) {
    results.push_back(publish_evidence(request));
  }
  return results;
}

LinkMutationResult LinkStateEngine::withdraw_evidence(const WithdrawEvidenceRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.key.link.valid() || !request.key.source.valid() || !request.observation.valid() ||
      !request.publisher.valid() || !request.worker_boot.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "evidence key, observation, publisher and worker boot are required");
  }
  const auto it = impl_->store.links.find(request.key.link);
  if (it == impl_->store.links.end()) {
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    return detail::failed(OutcomeCode::RetiredLink,
                          "link state record is retired and accepts no withdrawal");
  }
  std::string error;
  const OutcomeCode authorized = detail::authorize(
      impl_->store, request.publisher, request.worker_boot, request.key.source, request.epoch,
      impl_->epoch, request.key.link, entry.record.binding, error);
  if (authorized != OutcomeCode::Committed) {
    return detail::failed(authorized, error);
  }
  const EvidenceRecord* slot = find_slot(entry, request.key);
  if (slot == nullptr) {
    return detail::failed(OutcomeCode::NotFound, "evidence key has no current record on this link");
  }
  if (slot->publisher != request.publisher || slot->worker_boot != request.worker_boot) {
    return detail::failed(OutcomeCode::StaleAuthority,
                          "the current record of this evidence key is owned by another "
                          "publisher incarnation");
  }
  if (slot->observation != request.observation) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "withdrawal must reference the current observation of the evidence key");
  }
  if (request.source_generation < slot->source_generation) {
    return detail::failed(OutcomeCode::StaleEvidence,
                          "withdrawal source generation is older than the recorded generation");
  }
  if (request.sequence <= slot->sequence) {
    return detail::failed(
        OutcomeCode::StaleEvidence,
        "withdrawal sequence must be newer than the sequence of the withdrawn observation");
  }

  std::vector<const EvidenceRecord*> prospective;
  prospective.reserve(entry.current.size());
  for (const EvidenceRecord& record : entry.current) {
    if (record.key != request.key) {
      prospective.push_back(&record);
    }
  }
  detail::PreparedCommit prepared =
      detail::prepare_commit(*impl_, entry, prospective, TransitionTrigger::EvidenceWithdrawal);
  if (!prepared.ok) {
    return detail::failed(prepared.code, prepared.detail);
  }

  for (auto record_it = entry.current.begin(); record_it != entry.current.end();) {
    if (record_it->key == request.key) {
      detail::push_disposition(entry, *record_it, EvidenceStatus::Withdrawn,
                               request.reason.empty()
                                   ? std::string("withdrawn by its publisher")
                                   : "withdrawn by its publisher: " + request.reason,
                               *impl_);
      detail::erase_observation(impl_->store, *record_it);
      record_it = entry.current.erase(record_it);
    } else {
      ++record_it;
    }
  }
  if (!detail::advance(impl_->global_generation) ||
      !detail::advance(entry.record.evidence_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "generation counter is exhausted");
  }
  entry.record.global_generation = impl_->global_generation;
  const auto links_it = impl_->store.publisher_links.find(request.publisher);
  if (links_it != impl_->store.publisher_links.end()) {
    bool remains = false;
    for (const EvidenceRecord& record : entry.current) {
      if (record.publisher == request.publisher) {
        remains = true;
        break;
      }
    }
    if (!remains) {
      links_it->second.erase(request.key.link);
      if (links_it->second.empty()) {
        impl_->store.publisher_links.erase(links_it);
      }
    }
  }
  detail::commit_prepared(*impl_, entry, prepared, PublicationId{}, request.publisher);
  ++impl_->counters.committed_publications;
  return result_for(OutcomeCode::Committed, std::string(), entry, prepared.state_changed);
}

LinkMutationResult LinkStateEngine::transition_state(const TransitionStateRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.link.valid() || !request.publication.valid() || !request.publisher.valid() ||
      !request.worker_boot.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "link, publication, publisher and worker boot are required");
  }
  const auto it = impl_->store.links.find(request.link);
  if (it == impl_->store.links.end()) {
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    return detail::failed(OutcomeCode::RetiredLink, "link state record is retired and terminal");
  }
  if (request.target == LinkOperationalState::Unknown) {
    return detail::failed(
        OutcomeCode::InvalidTransition,
        "UNKNOWN is never asserted; it is reached only when no decisive evidence exists");
  }
  if (request.target == LinkOperationalState::RevalidationRequired) {
    return detail::failed(OutcomeCode::InvalidTransition,
                          "use mark_revalidation_required to require revalidation");
  }
  std::string error;
  const OutcomeCode authorized = detail::authorize(
      impl_->store, request.publisher, request.worker_boot, SourceId{}, request.epoch, impl_->epoch,
      request.link, entry.record.binding, error);
  if (authorized != OutcomeCode::Committed) {
    return detail::failed(authorized, error);
  }
  if (request.expected_state_generation.has_value() &&
      request.expected_state_generation.value() != entry.record.state_generation) {
    std::string detail_text = "expected state generation ";
    detail_text += request.expected_state_generation->to_string();
    detail_text += " is not the current state generation ";
    detail_text += entry.record.state_generation.to_string();
    return detail::failed(OutcomeCode::StaleGeneration, detail_text);
  }

  if (request.target == LinkOperationalState::Up) {
    if (!entry.record.admin_intent.has_value()) {
      return result_for(OutcomeCode::NoChange,
                        "no administrative intent is in force; UP is established only by evidence",
                        entry, false);
    }
    LinkEntry shadow = entry;
    shadow.record.admin_intent.reset();
    const std::vector<const EvidenceRecord*> pointers = detail::current_pointers(entry);
    detail::PreparedCommit prepared = detail::prepare_commit(
        *impl_, shadow, pointers, TransitionTrigger::RevalidationOutcome);
    if (!prepared.ok) {
      return detail::failed(prepared.code, prepared.detail);
    }
    entry.record.admin_intent.reset();
    if (!detail::advance(impl_->global_generation)) {
      return detail::failed(OutcomeCode::ResourceLimit, "global generation counter is exhausted");
    }
    entry.record.global_generation = impl_->global_generation;
    detail::commit_prepared(*impl_, entry, prepared, request.publication, request.publisher);
    std::string detail_text = "administrative intent cleared; the state now follows current evidence";
    return result_for(OutcomeCode::Committed, std::move(detail_text), entry, prepared.state_changed);
  }

  if (request.target != LinkOperationalState::AdminDisabled &&
      request.target != LinkOperationalState::Draining &&
      request.target != LinkOperationalState::Down && request.target != LinkOperationalState::Degraded &&
      request.target != LinkOperationalState::Faulted) {
    return detail::failed(OutcomeCode::InvalidTransition,
                          "explicit transitions may only restrict a link");
  }
  if (request.target == LinkOperationalState::Degraded &&
      request.degradation_cause == DegradationCause::None) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "an explicit DEGRADED transition requires a named degradation cause");
  }

  AdministrativeIntent intent;
  intent.state = request.target;
  intent.publication = request.publication;
  intent.publisher = request.publisher;
  intent.generation = impl_->global_generation;
  intent.cause = request.degradation_cause;

  LinkEntry shadow = entry;
  shadow.record.admin_intent = intent;
  const std::vector<const EvidenceRecord*> pointers = detail::current_pointers(entry);
  detail::PreparedCommit prepared = detail::prepare_commit(
      *impl_, shadow, pointers, TransitionTrigger::ExplicitAdministrative);
  if (!prepared.ok) {
    return detail::failed(prepared.code, prepared.detail);
  }
  if (!detail::advance(impl_->global_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "global generation counter is exhausted");
  }
  intent.generation = impl_->global_generation;
  entry.record.global_generation = impl_->global_generation;
  entry.record.admin_intent = intent;
  detail::commit_prepared(*impl_, entry, prepared, request.publication, request.publisher);
  std::string detail_text = "administrative intent recorded (";
  detail_text += linkstate::to_string(request.target);
  detail_text.push_back(')');
  return result_for(OutcomeCode::Committed, std::move(detail_text), entry, prepared.state_changed);
}

LinkMutationResult LinkStateEngine::mark_revalidation_required(
    const MarkRevalidationRequest& request) {
  std::unique_lock lock(impl_->mutex);
  if (!request.link.valid() || !request.publication.valid() || !request.publisher.valid() ||
      !request.worker_boot.valid()) {
    return detail::failed(OutcomeCode::MalformedRequest,
                          "link, publication, publisher and worker boot are required");
  }
  const auto it = impl_->store.links.find(request.link);
  if (it == impl_->store.links.end()) {
    return detail::failed(OutcomeCode::UnknownLink, "link is not bound in this engine");
  }
  LinkEntry& entry = it->second;
  if (entry.record.state == LinkOperationalState::Retired) {
    return detail::failed(OutcomeCode::RetiredLink, "link state record is retired and terminal");
  }
  std::string error;
  const OutcomeCode authorized = detail::authorize(
      impl_->store, request.publisher, request.worker_boot, SourceId{}, request.epoch, impl_->epoch,
      request.link, entry.record.binding, error);
  if (authorized != OutcomeCode::Committed) {
    return detail::failed(authorized, error);
  }
  if (request.expected_state_generation.has_value() &&
      request.expected_state_generation.value() != entry.record.state_generation) {
    return detail::failed(OutcomeCode::StaleGeneration,
                          "expected state generation is not the current state generation");
  }

  bool owns_evidence = false;
  std::vector<const EvidenceRecord*> prospective;
  prospective.reserve(entry.current.size());
  for (const EvidenceRecord& record : entry.current) {
    if (record.publisher == request.publisher) {
      owns_evidence = true;
      continue;
    }
    prospective.push_back(&record);
  }
  if (!owns_evidence) {
    return result_for(OutcomeCode::NoChange,
                      "publisher holds no current evidence for this link; nothing to invalidate",
                      entry, false);
  }
  detail::PreparedCommit prepared =
      detail::prepare_commit(*impl_, entry, prospective, TransitionTrigger::EvidenceWithdrawal);
  if (!prepared.ok) {
    return detail::failed(prepared.code, prepared.detail);
  }
  std::string reason = "publisher requires revalidation";
  if (!request.reason.empty()) {
    reason += ": ";
    reason += request.reason;
  }
  for (auto record_it = entry.current.begin(); record_it != entry.current.end();) {
    if (record_it->publisher == request.publisher) {
      detail::push_disposition(entry, *record_it, EvidenceStatus::Withdrawn, reason, *impl_);
      detail::erase_observation(impl_->store, *record_it);
      record_it = entry.current.erase(record_it);
    } else {
      ++record_it;
    }
  }
  const auto links_it = impl_->store.publisher_links.find(request.publisher);
  if (links_it != impl_->store.publisher_links.end()) {
    bool remains = false;
    for (const EvidenceRecord& record : entry.current) {
      if (record.publisher == request.publisher) {
        remains = true;
        break;
      }
    }
    if (!remains) {
      links_it->second.erase(request.link);
      if (links_it->second.empty()) {
        impl_->store.publisher_links.erase(links_it);
      }
    }
  }
  if (!detail::advance(impl_->global_generation) ||
      !detail::advance(entry.record.evidence_generation)) {
    return detail::failed(OutcomeCode::ResourceLimit, "generation counter is exhausted");
  }
  entry.record.global_generation = impl_->global_generation;
  detail::commit_prepared(*impl_, entry, prepared, request.publication, request.publisher);
  return result_for(OutcomeCode::Committed, reason, entry, prepared.state_changed);
}

}  // namespace linkstate
