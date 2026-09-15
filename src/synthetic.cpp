#include "linkstate/synthetic.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "text.hpp"

namespace linkstate {
namespace {

std::string number(std::size_t value) { return detail::format_u64(value); }

}  // namespace

TopologyBinding to_binding(const SyntheticLinkSpec& spec) {
  TopologyBinding binding;
  binding.fabric = spec.fabric;
  binding.site = spec.site;
  binding.link = spec.link;
  binding.topology_generation = spec.topology_generation;
  binding.endpoints.local = spec.local_endpoint;
  binding.endpoints.local_generation = spec.endpoint_generation;
  binding.endpoints.remote = spec.remote_endpoint;
  binding.endpoints.remote_generation = spec.endpoint_generation;
  binding.link_class = spec.link_class;
  binding.symmetry = spec.symmetry == LinkSymmetryGuarantee::Unspecified
                         ? default_symmetry_for(spec.link_class)
                         : spec.symmetry;
  binding.backing_links = spec.backing_links;
  binding.member_links = spec.member_links;
  return binding;
}

std::vector<SyntheticLinkSpec> build_synthetic_fabric(const SyntheticFabricSpec& spec) {
  std::vector<SyntheticLinkSpec> links;
  const std::size_t switches = (std::max)(static_cast<std::size_t>(2), spec.switch_count);
  const std::size_t sites = (std::max)(static_cast<std::size_t>(1), spec.site_count);
  const std::size_t per_pair = (std::max)(static_cast<std::size_t>(1), spec.links_per_switch_pair);
  for (std::size_t left = 0; left < switches; ++left) {
    for (std::size_t right = left + 1; right < switches; ++right) {
      for (std::size_t member = 0; member < per_pair; ++member) {
        SyntheticLinkSpec link;
        link.fabric = spec.fabric;
        link.site = spec.site;
        link.link_class = spec.link_class;
        link.symmetry = spec.symmetry;
        link.topology_generation = spec.topology_generation;
        link.endpoint_generation = spec.endpoint_generation;
        std::string name = spec.fabric.valid() ? spec.fabric.value() : std::string("fabric");
        name += ".sw";
        name += number(left);
        name += "-sw";
        name += number(right);
        name += ".";
        name += number(member);
        link.link = LinkId::from_validated("link." + name);
        link.local_endpoint = EndpointId::from_validated("ep." + name + ".a");
        link.remote_endpoint = EndpointId::from_validated("ep." + name + ".b");
        if (sites > 1) {
          const std::size_t left_site = left % sites;
          const std::size_t right_site = right % sites;
          link.site = SiteId::from_validated((spec.site.valid() ? spec.site.value() : "site") + "." +
                                             number(left_site));
          if (left_site != right_site) {
            link.link_class = spec.link_class;
          }
        }
        links.push_back(std::move(link));
      }
    }
  }
  for (const SyntheticLinkSpec& extra : spec.extra_links) {
    links.push_back(extra);
  }
  std::sort(links.begin(), links.end(),
            [](const SyntheticLinkSpec& lhs, const SyntheticLinkSpec& rhs) {
              return lhs.link < rhs.link;
            });
  return links;
}

std::vector<SyntheticLinkSpec> build_synthetic_inventory(std::size_t link_count,
                                                         const FabricId& fabric, const SiteId& site,
                                                         LinkClass link_class,
                                                         TopologyGeneration topology,
                                                         EndpointGeneration endpoints) {
  std::vector<SyntheticLinkSpec> links;
  links.reserve(link_count);
  for (std::size_t index = 0; index < link_count; ++index) {
    SyntheticLinkSpec link;
    link.fabric = fabric;
    link.site = site;
    link.link_class = link_class;
    link.topology_generation = topology;
    link.endpoint_generation = endpoints;
    const std::string name = number(index);
    link.link = LinkId::from_validated("link.inv." + name);
    link.local_endpoint = EndpointId::from_validated("ep.inv." + name + ".a");
    link.remote_endpoint = EndpointId::from_validated("ep.inv." + name + ".b");
    links.push_back(std::move(link));
  }
  return links;
}

SyntheticFabric::SyntheticFabric(LinkStateEngine& engine, PublisherId publisher,
                                 WorkerBootId worker_boot, SourceId source)
    : engine_(engine), publisher_(std::move(publisher)), worker_boot_(std::move(worker_boot)),
      source_(std::move(source)), epoch_(engine.coordinator_epoch()) {}

Outcome SyntheticFabric::bind(const std::vector<SyntheticLinkSpec>& links) {
  std::vector<LinkId> scope;
  scope.reserve(links.size());
  for (const SyntheticLinkSpec& spec : links) {
    const LinkMutationResult bound = engine_.bind_link(BindLinkRequest{to_binding(spec)});
    if (bound.outcome.is_error() && bound.outcome.code != OutcomeCode::Idempotent) {
      return bound.outcome;
    }
    scope.push_back(spec.link);
  }
  PublisherRegistration registration;
  registration.publisher = publisher_;
  registration.worker_boot = worker_boot_;
  registration.source = source_;
  registration.scope.fabric = links.empty() ? FabricId{} : links.front().fabric;
  registration.scope.links = std::move(scope);
  registration.epoch = engine_.coordinator_epoch();
  epoch_ = registration.epoch;
  const LinkMutationResult registered =
      engine_.register_publisher(RegisterPublisherRequest{registration});
  return registered.outcome;
}

Outcome SyntheticFabric::rebind(CoordinatorEpoch epoch, const std::vector<LinkId>& links) {
  PublisherRegistration registration;
  registration.publisher = publisher_;
  registration.worker_boot = worker_boot_;
  registration.source = source_;
  registration.scope.links = links;
  registration.epoch = epoch;
  const LinkMutationResult registered =
      engine_.register_publisher(RegisterPublisherRequest{registration});
  if (!registered.outcome.is_error()) {
    epoch_ = epoch;
  }
  return registered.outcome;
}

LinkMutationResult SyntheticFabric::publish(const LinkId& link, EvidenceClaim claim,
                                            DegradationCause cause, EvidenceKind kind,
                                            std::string detail_text) {
  return publish_from(source_, link, claim, kind, cause, std::move(detail_text));
}

std::string SyntheticFabric::key_of(const SourceId& source, const LinkId& link, EvidenceKind kind,
                                    EvidenceDirection direction,
                                    const EndpointId& endpoint) const {
  std::string key = link.value();
  key.push_back('|');
  key += source.value();
  key.push_back('|');
  key += endpoint.value();
  key.push_back('|');
  key += std::to_string(static_cast<unsigned>(direction));
  key.push_back('|');
  key += std::to_string(static_cast<unsigned>(kind));
  return key;
}

LinkMutationResult SyntheticFabric::publish_from(const SourceId& source, const LinkId& link,
                                                 EvidenceClaim claim, EvidenceKind kind,
                                                 DegradationCause cause,
                                                 std::string detail_text) {
  return publish_endpoint_impl(source, link, EndpointId{}, EvidenceDirection::Bidirectional, kind,
                               claim, cause, std::move(detail_text));
}

LinkMutationResult SyntheticFabric::publish_endpoint(const LinkId& link, const EndpointId& endpoint,
                                                     EvidenceDirection direction,
                                                     EvidenceClaim claim, EvidenceKind kind,
                                                     DegradationCause cause,
                                                     std::string detail_text) {
  return publish_endpoint_impl(source_, link, endpoint, direction, kind, claim, cause,
                               std::move(detail_text));
}

LinkMutationResult SyntheticFabric::publish_endpoint_impl(const SourceId& source,
                                                            const LinkId& link,
                                                            const EndpointId& endpoint,
                                                            EvidenceDirection direction,
                                                            EvidenceKind kind, EvidenceClaim claim,
                                                            DegradationCause cause,
                                                            std::string detail_text) {
  LinkMutationResult result;
  const std::optional<TopologyBinding> binding = engine_.topology_binding(link);
  if (!binding.has_value()) {
    result.outcome = Outcome::make(OutcomeCode::UnknownLink, "link is not bound");
    return result;
  }
  EndpointGeneration endpoint_generation;
  EvidenceKey key;
  key.link = link;
  key.source = source;
  key.endpoint = endpoint;
  key.direction = direction;
  key.kind = kind;
  if (endpoint.valid()) {
    if (endpoint == binding->endpoints.local) {
      endpoint_generation = binding->endpoints.local_generation;
    } else if (endpoint == binding->endpoints.remote) {
      endpoint_generation = binding->endpoints.remote_generation;
    } else {
      result.outcome = Outcome::make(OutcomeCode::MalformedRequest,
                                     "endpoint is not bound to this link");
      return result;
    }
  }
  const std::string key_text = key_of(source, link, kind, direction, endpoint);
  const std::uint64_t sequence = ++sequences_[key_text];
  ++observation_counter_;

  EvidenceRecord record;
  record.key = key;
  // Observation identity is globally unique per publisher incarnation, so two
  // synthetic agents can never collide on the same observation identity.
  record.observation = ObservationId::from_validated(
      "obs.synth." + publisher_.value() + "." + detail::format_u64(observation_counter_));
  record.publisher = publisher_;
  record.worker_boot = worker_boot_;
  record.epoch = engine_.coordinator_epoch();
  record.source_generation = SourceGeneration::from_value(1);
  record.sequence = sequence;
  record.topology_generation = binding->topology_generation;
  record.endpoint_generation = endpoint_generation;
  record.evidence_class = EvidenceClass::Synthetic;
  record.claim = claim;
  record.degradation_cause = cause;
  record.detail = detail_text.empty() ? std::string("synthetic observation") : std::move(detail_text);

  PublishEvidenceRequest request;
  request.publication = PublicationId::from_validated(
      "pub.synth." + publisher_.value() + "." + detail::format_u64(observation_counter_));
  request.observation = std::move(record);
  epoch_ = request.observation.epoch;
  return engine_.publish_evidence(request);
}

LinkMutationResult SyntheticFabric::withdraw(const LinkId& link, const SourceId& source,
                                             EvidenceKind kind, EvidenceDirection direction,
                                             EndpointId endpoint, std::string reason) {
  LinkMutationResult result;
  const std::vector<EvidenceRecord> records = engine_.evidence_for_link(link);
  const EvidenceRecord* target = nullptr;
  for (const EvidenceRecord& record : records) {
    if (record.key.source == source && record.key.kind == kind &&
        record.key.direction == direction && record.key.endpoint == endpoint) {
      target = &record;
      break;
    }
  }
  if (target == nullptr) {
    result.outcome =
        Outcome::make(OutcomeCode::NotFound, "no current evidence record matches the request");
    return result;
  }
  WithdrawEvidenceRequest request;
  request.key = target->key;
  request.observation = target->observation;
  request.publisher = publisher_;
  request.worker_boot = worker_boot_;
  request.epoch = engine_.coordinator_epoch();
  request.source_generation = target->source_generation;
  request.sequence = target->sequence + 1u;
  request.reason = std::move(reason);
  sequences_[key_of(source, link, kind, direction, endpoint)] = request.sequence;
  return engine_.withdraw_evidence(request);
}

std::uint64_t SyntheticFabric::sequence_of(const LinkId& link, const SourceId& source,
                                           EvidenceKind kind, EvidenceDirection direction,
                                           EndpointId endpoint) const {
  const auto it = sequences_.find(key_of(source, link, kind, direction, endpoint));
  return it == sequences_.end() ? 0u : it->second;
}

}  // namespace linkstate
