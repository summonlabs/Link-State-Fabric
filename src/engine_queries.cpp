#include <algorithm>
#include <string>
#include <vector>

#include "engine_internal.hpp"
#include "engine_support.hpp"
#include "text.hpp"

namespace linkstate {
namespace {

std::vector<LinkId> sorted_links(const std::unordered_set<LinkId>& links) {
  std::vector<LinkId> out(links.begin(), links.end());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::optional<LinkStateView> LinkStateEngine::link_state(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    return std::nullopt;
  }
  return detail::make_view(it->second);
}

std::optional<LinkStateRecord> LinkStateEngine::record(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    return std::nullopt;
  }
  return it->second.record;
}

std::vector<LinkStateView> LinkStateEngine::links_in_state(LinkOperationalState state) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkStateView> views;
  const auto it = impl_->store.by_state.find(state);
  if (it == impl_->store.by_state.end()) {
    return views;
  }
  views.reserve(it->second.size());
  const std::vector<LinkId> links = sorted_links(it->second);
  for (const LinkId& link : links) {
    const auto entry_it = impl_->store.links.find(link);
    if (entry_it != impl_->store.links.end() && entry_it->second.record.state == state) {
      views.push_back(detail::make_view(entry_it->second));
    }
  }
  return views;
}

std::vector<LinkId> LinkStateEngine::links_requiring_revalidation() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkId> links;
  const auto it = impl_->store.by_state.find(LinkOperationalState::RevalidationRequired);
  if (it == impl_->store.by_state.end()) {
    return links;
  }
  links.reserve(it->second.size());
  for (const LinkId& link : it->second) {
    const auto entry_it = impl_->store.links.find(link);
    if (entry_it != impl_->store.links.end() &&
        entry_it->second.record.state == LinkOperationalState::RevalidationRequired) {
      links.push_back(link);
    }
  }
  std::sort(links.begin(), links.end());
  return links;
}

std::vector<LinkId> LinkStateEngine::links_for_endpoint(const EndpointId& endpoint) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkId> links;
  const auto it = impl_->store.by_endpoint.find(endpoint);
  if (it == impl_->store.by_endpoint.end()) {
    return links;
  }
  links.assign(it->second.begin(), it->second.end());
  std::sort(links.begin(), links.end());
  return links;
}

std::optional<LinkStateView> LinkStateEngine::link_by_endpoints(const EndpointId& a,
                                                               const EndpointId& b) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkId> matches;
  const auto first = impl_->store.by_endpoint.find(a);
  if (first == impl_->store.by_endpoint.end()) {
    return std::nullopt;
  }
  for (const LinkId& link : first->second) {
    const auto entry_it = impl_->store.links.find(link);
    if (entry_it == impl_->store.links.end()) {
      continue;
    }
    const EndpointBinding& endpoints = entry_it->second.record.binding.endpoints;
    if ((endpoints.local == a && endpoints.remote == b) ||
        (endpoints.local == b && endpoints.remote == a)) {
      matches.push_back(link);
    }
  }
  if (matches.size() != 1) {
    return std::nullopt;
  }
  return detail::make_view(impl_->store.links.find(matches.front())->second);
}

std::optional<LinkStateView> LinkStateEngine::link_by_remote_endpoint(const EndpointId& local,
                                                                     const EndpointId& remote) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkId> matches;
  const auto first = impl_->store.by_endpoint.find(local);
  if (first == impl_->store.by_endpoint.end()) {
    return std::nullopt;
  }
  for (const LinkId& link : first->second) {
    const auto entry_it = impl_->store.links.find(link);
    if (entry_it == impl_->store.links.end()) {
      continue;
    }
    const EndpointBinding& endpoints = entry_it->second.record.binding.endpoints;
    if (endpoints.local == local && endpoints.remote == remote) {
      matches.push_back(link);
    }
  }
  if (matches.size() != 1) {
    return std::nullopt;
  }
  return detail::make_view(impl_->store.links.find(matches.front())->second);
}

std::vector<EvidenceRecord> LinkStateEngine::evidence_for_link(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<EvidenceRecord> records;
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    return records;
  }
  records = it->second.current;
  std::sort(records.begin(), records.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
              return detail::evidence_less(lhs, rhs);
            });
  return records;
}

std::vector<EvidenceRecord> LinkStateEngine::evidence_for_publisher(
    const PublisherId& publisher) const {
  std::shared_lock lock(impl_->mutex);
  std::vector<EvidenceRecord> records;
  const auto links_it = impl_->store.publisher_links.find(publisher);
  if (links_it == impl_->store.publisher_links.end()) {
    return records;
  }
  for (const LinkId& link : links_it->second) {
    const auto entry_it = impl_->store.links.find(link);
    if (entry_it == impl_->store.links.end()) {
      continue;
    }
    for (const EvidenceRecord& record : entry_it->second.current) {
      if (record.publisher == publisher) {
        records.push_back(record);
      }
    }
  }
  std::sort(records.begin(), records.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
              return detail::evidence_less(lhs, rhs);
            });
  return records;
}

std::optional<PublisherAuthority> LinkStateEngine::publisher_authority(
    const PublisherId& publisher) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.publishers.find(publisher);
  if (it == impl_->store.publishers.end()) {
    return std::nullopt;
  }
  PublisherAuthority authority = it->second;
  authority.live_evidence = 0;
  const auto links_it = impl_->store.publisher_links.find(publisher);
  if (links_it != impl_->store.publisher_links.end()) {
    for (const LinkId& link : links_it->second) {
      const auto entry_it = impl_->store.links.find(link);
      if (entry_it == impl_->store.links.end()) {
        continue;
      }
      for (const EvidenceRecord& record : entry_it->second.current) {
        if (record.publisher == publisher) {
          ++authority.live_evidence;
        }
      }
    }
  }
  return authority;
}

std::vector<PublisherAuthority> LinkStateEngine::publishers() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<PublisherAuthority> authorities;
  authorities.reserve(impl_->store.publishers.size());
  for (const auto& entry : impl_->store.publishers) {
    PublisherAuthority authority = entry.second;
    authority.live_evidence = 0;
    const auto links_it = impl_->store.publisher_links.find(entry.first);
    if (links_it != impl_->store.publisher_links.end()) {
      for (const LinkId& link : links_it->second) {
        const auto entry_it = impl_->store.links.find(link);
        if (entry_it == impl_->store.links.end()) {
          continue;
        }
        for (const EvidenceRecord& record : entry_it->second.current) {
          if (record.publisher == entry.first) {
            ++authority.live_evidence;
          }
        }
      }
    }
    authorities.push_back(std::move(authority));
  }
  std::sort(authorities.begin(), authorities.end(),
            [](const PublisherAuthority& lhs, const PublisherAuthority& rhs) {
              return lhs.publisher < rhs.publisher;
            });
  return authorities;
}

std::vector<PublisherFence> LinkStateEngine::fences() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->store.fences;
}

std::optional<TopologyBinding> LinkStateEngine::topology_binding(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    return std::nullopt;
  }
  return it->second.record.binding;
}

GlobalGeneration LinkStateEngine::global_generation() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->global_generation;
}

LinkStateGeneration LinkStateEngine::global_state_generation() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->global_state_generation;
}

EvidenceGeneration LinkStateEngine::link_evidence_generation(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    return EvidenceGeneration{};
  }
  return it->second.record.evidence_generation;
}

CoordinatorEpoch LinkStateEngine::coordinator_epoch() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->epoch;
}

TopologyGeneration LinkStateEngine::topology_generation() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->topology_generation;
}

std::size_t LinkStateEngine::link_count() const {
  std::shared_lock lock(impl_->mutex);
  return impl_->store.links.size();
}

std::size_t LinkStateEngine::evidence_count() const {
  std::shared_lock lock(impl_->mutex);
  std::size_t count = 0;
  for (const auto& entry : impl_->store.links) {
    count += entry.second.current.size();
  }
  return count;
}

EngineStats LinkStateEngine::stats() const {
  std::shared_lock lock(impl_->mutex);
  EngineStats stats = impl_->counters;
  stats.links = impl_->store.links.size();
  stats.evidence_records = 0;
  stats.current_evidence = 0;
  stats.conflicts = 0;
  stats.retained_snapshots = impl_->store.snapshots.size();
  for (const auto& entry : impl_->store.links) {
    stats.evidence_records += entry.second.current.size();
    stats.current_evidence += entry.second.current.size();
    if (entry.second.conflicted) {
      ++stats.conflicts;
    }
  }
  stats.publishers = impl_->store.publishers.size();
  stats.active_publishers = 0;
  stats.fenced_publishers = 0;
  for (const auto& entry : impl_->store.publishers) {
    if (entry.second.status == AuthorityStatus::Active) {
      ++stats.active_publishers;
    } else {
      ++stats.fenced_publishers;
    }
  }
  return stats;
}

Digest LinkStateEngine::state_digest() const {
  std::shared_lock lock(impl_->mutex);
  std::vector<LinkStateRecord> records;
  records.reserve(impl_->store.links.size());
  for (const auto& entry : impl_->store.links) {
    records.push_back(entry.second.record);
  }
  return digest_records(records);
}

Snapshot LinkStateEngine::snapshot(const SnapshotRequest& request) {
  std::unique_lock lock(impl_->mutex);
  Snapshot snap;
  ++impl_->snapshot_counter;
  std::string id_value = "snap.";
  id_value += impl_->global_state_generation.to_string();
  id_value.push_back('.');
  id_value += detail::format_u64(impl_->snapshot_counter);
  snap.id = SnapshotId::from_validated(id_value);
  snap.label = request.label;
  snap.global_generation = impl_->global_generation;
  snap.global_state_generation = impl_->global_state_generation;
  snap.epoch = impl_->epoch;
  snap.topology_generation = impl_->topology_generation;
  snap.sequence = impl_->snapshot_counter;

  std::vector<LinkId> selected;
  if (!request.links.empty()) {
    if (request.links.size() > limits::max_snapshot_records) {
      return snap;
    }
    selected = request.links;
    std::sort(selected.begin(), selected.end());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  } else {
    selected.reserve(impl_->store.links.size());
    for (const auto& entry : impl_->store.links) {
      selected.push_back(entry.first);
    }
    std::sort(selected.begin(), selected.end());
  }

  std::size_t limit = request.max_records;
  if (limit == 0 || limit > impl_->config.max_links) {
    limit = impl_->config.max_links;
  }
  for (const LinkId& link : selected) {
    if (snap.records.size() >= limit) {
      break;
    }
    const auto it = impl_->store.links.find(link);
    if (it == impl_->store.links.end()) {
      continue;
    }
    const detail::LinkEntry& entry = it->second;
    if (request.fabric.valid() && entry.record.binding.fabric.valid() &&
        entry.record.binding.fabric != request.fabric) {
      continue;
    }
    if (request.state_filter.has_value() && entry.record.state != request.state_filter.value()) {
      continue;
    }
    SnapshotRecord record;
    record.link = link;
    record.state = entry.record.state;
    record.result_class = entry.record.result_class;
    record.state_generation = entry.record.state_generation;
    record.evidence_generation = entry.record.evidence_generation;
    record.topology_generation = entry.record.binding.topology_generation;
    record.current_evidence = entry.current.size();
    record.conflicted = entry.conflicted;
    record.synthetic_backed = entry.synthetic_backed;
    record.record_digest = digest_record(entry.record);
    snap.records.push_back(std::move(record));
  }
  snap.digest = digest_snapshot(snap.records, snap.global_generation, snap.global_state_generation,
                                snap.epoch);
  impl_->store.snapshots.emplace(snap.id, snap);
  impl_->store.snapshot_order.push_back(snap.id);
  while (impl_->store.snapshot_order.size() > impl_->config.max_retained_snapshots) {
    const SnapshotId oldest = impl_->store.snapshot_order.front();
    impl_->store.snapshot_order.pop_front();
    impl_->store.snapshots.erase(oldest);
  }
  return snap;
}

std::optional<Snapshot> LinkStateEngine::retained_snapshot(const SnapshotId& id) const {
  std::shared_lock lock(impl_->mutex);
  const auto it = impl_->store.snapshots.find(id);
  if (it == impl_->store.snapshots.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<SnapshotId> LinkStateEngine::retained_snapshot_ids() const {
  std::shared_lock lock(impl_->mutex);
  return std::vector<SnapshotId>(impl_->store.snapshot_order.begin(),
                                 impl_->store.snapshot_order.end());
}

bool LinkStateEngine::snapshot_is_current(const Snapshot& snapshot) const {
  std::shared_lock lock(impl_->mutex);
  return snapshot.global_state_generation == impl_->global_state_generation &&
         snapshot.epoch == impl_->epoch;
}

}  // namespace linkstate
