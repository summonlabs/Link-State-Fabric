#include <algorithm>
#include <deque>
#include <span>
#include <string>
#include <vector>

#include "engine_internal.hpp"
#include "engine_support.hpp"
#include "text.hpp"

namespace linkstate {
namespace {

constexpr LayoutGeneration kCurrentLayout = LayoutGeneration::from_value(2);

JournalPayload build_payload(const LinkStateEngine::Impl& impl, bool include_history) {
  JournalPayload payload;
  payload.layout = kCurrentLayout;
  payload.epoch = impl.epoch;
  payload.global_generation = impl.global_generation;
  payload.global_state_generation = impl.global_state_generation;
  payload.topology_generation = impl.topology_generation;
  payload.snapshot_counter = impl.snapshot_counter;
  payload.transition_counter = impl.transition_counter;
  payload.fences = impl.store.fences;

  std::vector<LinkId> links;
  links.reserve(impl.store.links.size());
  for (const auto& entry : impl.store.links) {
    links.push_back(entry.first);
  }
  std::sort(links.begin(), links.end());
  payload.links.reserve(links.size());
  for (const LinkId& link : links) {
    const auto it = impl.store.links.find(link);
    if (it == impl.store.links.end()) {
      continue;
    }
    PersistedLink persisted;
    persisted.record = it->second.record;
    if (!include_history) {
      persisted.record.history.clear();
    }
    persisted.evidence = it->second.current;
    payload.links.push_back(std::move(persisted));
  }
  return payload;
}

}  // namespace

Outcome LinkStateEngine::save(const PersistenceConfig& config) const {
  std::vector<std::byte> encoded;
  JournalSummary summary;
  {
    std::shared_lock lock(impl_->mutex);
    const JournalPayload payload = build_payload(*impl_, config.include_history);
    encoded = encode_journal(payload, config, summary);
  }
  const Outcome written = write_journal_file(config, std::span<const std::byte>(encoded));
  if (written.is_error()) {
    return written;
  }
  std::string detail_text = "journal written with ";
  detail_text += detail::format_u64(static_cast<std::uint64_t>(summary.record_count));
  detail_text += " records and ";
  detail_text += detail::format_u64(static_cast<std::uint64_t>(summary.evidence_count));
  detail_text += " evidence records; file digest ";
  detail_text += summary.file_digest.to_hex();
  return Outcome::make(OutcomeCode::Committed, detail_text);
}

LoadOutcome LinkStateEngine::load(const PersistenceConfig& config) {
  LoadOutcome result;
  std::vector<std::byte> bytes;
  JournalSummary summary;
  const Outcome read = read_journal_file(config, bytes, summary);
  if (read.is_error()) {
    result.outcome = read;
    return result;
  }
  JournalPayload payload;
  const Outcome decoded =
      decode_journal(std::span<const std::byte>(bytes), config, payload, summary);
  if (decoded.is_error()) {
    result.outcome = decoded;
    return result;
  }

  std::unique_lock lock(impl_->mutex);
  impl_->store = detail::EngineStore{};
  impl_->global_generation = payload.global_generation;
  impl_->global_state_generation = payload.global_state_generation;
  impl_->topology_generation = payload.topology_generation;
  impl_->epoch = payload.epoch;
  impl_->snapshot_counter = payload.snapshot_counter;
  impl_->transition_counter = payload.transition_counter;
  impl_->store.fences = payload.fences;
  // Fencing is permanent, so the fence registry is rebuilt from the durable
  // fence records: a worker incarnation that was fenced before the restart can
  // never become active again, even though its live authority is gone.
  for (const PublisherFence& fence : impl_->store.fences) {
    if (!fence.publisher.valid() || !fence.worker_boot.valid()) {
      continue;
    }
    std::deque<WorkerBootId>& registry = impl_->store.fenced_boots[fence.publisher];
    if (std::find(registry.begin(), registry.end(), fence.worker_boot) == registry.end()) {
      registry.push_back(fence.worker_boot);
    }
    while (registry.size() > impl_->config.fence_retention_per_publisher) {
      registry.pop_front();
    }
  }

  for (PersistedLink& persisted : payload.links) {
    detail::LinkEntry entry;
    entry.record = persisted.record;
    entry.record.history.clear();
    entry.record.durable = true;
    entry.conflicted = false;
    entry.synthetic_backed = false;

    // Conservative recovery: the durable conclusion survives, live authority
    // does not. Persisted dynamic evidence is retained as an explicit
    // disposition and never restored as current.
    for (const EvidenceRecord& record : persisted.evidence) {
      EvidenceDisposition disposition;
      disposition.observation = record.observation;
      disposition.source = record.key.source;
      disposition.publisher = record.publisher;
      disposition.worker_boot = record.worker_boot;
      disposition.kind = record.key.kind;
      disposition.direction = record.key.direction;
      disposition.claim = record.claim;
      disposition.evidence_class = record.evidence_class;
      disposition.status = EvidenceStatus::StaleAuthority;
      disposition.precedence = evidence_precedence(record.key.kind);
      disposition.counted = false;
      disposition.reason =
          "not current: live publisher authority does not survive a process restart";
      entry.dispositions.push_back(std::move(disposition));
    }
    const std::size_t cap = impl_->config.max_history_per_link;
    if (entry.dispositions.size() > cap) {
      entry.dispositions.resize(cap);
    }
    entry.current.clear();

    if (entry.record.state == LinkOperationalState::Retired) {
      detail::index_link(impl_->store, entry.record.binding.link, LinkOperationalState::Retired);
      detail::index_endpoints(impl_->store, entry.record.binding, true);
      impl_->store.links.emplace(entry.record.binding.link, std::move(entry));
      continue;
    }

    const LinkOperationalState previous = entry.record.state;
    const LinkOperationalState recovered =
        entry.record.ever_established ? LinkOperationalState::RevalidationRequired
                                      : LinkOperationalState::Unknown;
    const TransitionLegality legality =
        evaluate_transition(previous, recovered, TransitionTrigger::Recovery);
    if (legality == TransitionLegality::FromRetired) {
      detail::index_link(impl_->store, entry.record.binding.link, previous);
      detail::index_endpoints(impl_->store, entry.record.binding, true);
      impl_->store.links.emplace(entry.record.binding.link, std::move(entry));
      continue;
    }
    if (legality == TransitionLegality::Legal) {
      StateHistoryEntry history;
      history.state_generation = entry.record.state_generation;
      history.from_state = previous;
      history.to_state = recovered;
      history.trigger = TransitionTrigger::Recovery;
      history.result_class = recovered == LinkOperationalState::RevalidationRequired
                                 ? StateResultClass::RevalidationRequired
                                 : StateResultClass::AuthoritativeUnknown;
      history.publication = PublicationId{};
      history.publisher = PublisherId{};
      history.evidence_generation = entry.record.evidence_generation;
      history.topology_generation = entry.record.binding.topology_generation;
      history.outcome = OutcomeCode::Committed;
      entry.record.state = recovered;
      entry.record.result_class = history.result_class;
      entry.record.degradation_cause = DegradationCause::None;
      entry.record.state_generation = LinkStateGeneration::from_value(
          entry.record.state_generation.value() + 1u);
      if (impl_->global_state_generation < entry.record.state_generation) {
        impl_->global_state_generation = entry.record.state_generation;
      }
      detail::push_history(entry, history, cap);
      if (recovered == LinkOperationalState::RevalidationRequired) {
        ++result.records_revalidated;
      } else {
        ++result.records_left_unknown;
      }
    } else {
      entry.record.result_class = StateResultClass::AuthoritativeUnknown;
      ++result.records_left_unknown;
    }
    entry.record.provenance_digest = digest_evidence_set(entry.current);
    detail::index_link(impl_->store, entry.record.binding.link, entry.record.state);
    detail::index_endpoints(impl_->store, entry.record.binding, true);
    impl_->store.links.emplace(entry.record.binding.link, std::move(entry));
  }

  for (auto& entry : impl_->store.links) {
    for (const BackingReference& backing : entry.second.record.binding.backing_links) {
      impl_->store.dependents[backing.link].insert(entry.first);
    }
  }

  result.records_loaded = impl_->store.links.size();
  result.layout = payload.layout;
  result.journal_digest = summary.file_digest;
  result.outcome = Outcome::make(
      OutcomeCode::Committed,
      "durable state recovered; live authority and dynamic evidence were not restored as current");
  return result;
}

Outcome inspect_journal(const PersistenceConfig& config, JournalSummary& summary) {
  std::vector<std::byte> bytes;
  const Outcome read = read_journal_file(config, bytes, summary);
  if (read.is_error()) {
    return read;
  }
  JournalPayload payload;
  return decode_journal(std::span<const std::byte>(bytes), config, payload, summary);
}

}  // namespace linkstate
