#pragma once

// Shared fixtures for the Link State Fabric test suite. Everything here uses
// the public API only.

#include <string>
#include <unordered_map>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/persistence.hpp"
#include "linkstate/platform.hpp"
#include "linkstate/synthetic.hpp"

namespace lsf_test {

inline linkstate::LinkId link_id(const std::string& name) {
  return linkstate::LinkId::from_validated("link." + name);
}
inline linkstate::EndpointId endpoint_id(const std::string& name) {
  return linkstate::EndpointId::from_validated("ep." + name);
}
inline linkstate::FabricId fabric_id(const std::string& name) {
  return linkstate::FabricId::from_validated("fabric." + name);
}
inline linkstate::SiteId site_id(const std::string& name) {
  return linkstate::SiteId::from_validated("site." + name);
}
inline linkstate::PublisherId publisher_id(const std::string& name) {
  return linkstate::PublisherId::from_validated("publisher." + name);
}
inline linkstate::SourceId source_id(const std::string& name) {
  return linkstate::SourceId::from_validated("source." + name);
}
inline linkstate::WorkerBootId boot_id(const std::string& name) {
  return linkstate::WorkerBootId::from_validated("boot." + name);
}

/// Standard binding: link.<name> between ep.<name>.a and ep.<name>.b.
inline linkstate::TopologyBinding binding_for(const std::string& name,
                                              std::uint64_t topology_generation = 1,
                                              std::uint64_t endpoint_generation = 1,
                                              linkstate::LinkClass link_class =
                                                  linkstate::LinkClass::EthernetLike) {
  linkstate::TopologyBinding binding;
  binding.fabric = fabric_id("test");
  binding.site = site_id("test");
  binding.link = link_id(name);
  binding.topology_generation = linkstate::TopologyGeneration::from_value(topology_generation);
  binding.endpoints.local = endpoint_id(name + ".a");
  binding.endpoints.local_generation =
      linkstate::EndpointGeneration::from_value(endpoint_generation);
  binding.endpoints.remote = endpoint_id(name + ".b");
  binding.endpoints.remote_generation =
      linkstate::EndpointGeneration::from_value(endpoint_generation);
  binding.link_class = link_class;
  binding.symmetry = linkstate::default_symmetry_for(link_class);
  return binding;
}

/// A publisher bound to one engine, used by most tests.
class TestPublisher {
 public:
  TestPublisher(linkstate::LinkStateEngine& engine, const std::string& name,
                const std::vector<linkstate::LinkId>& links,
                const std::string& source, const std::string& boot)
      : engine_(engine), publisher_(publisher_id(name)), source_(source_id(source)),
        boot_(boot_id(boot)) {
    linkstate::PublisherRegistration registration;
    registration.publisher = publisher_;
    registration.worker_boot = boot_;
    registration.source = source_;
    registration.scope.links = links;
    registration.scope.fabric = fabric_id("test");
    registration.epoch = engine.coordinator_epoch();
    last_registration_ = engine.register_publisher(linkstate::RegisterPublisherRequest{registration});
  }

  const linkstate::Outcome& registration_outcome() const { return last_registration_.outcome; }
  const linkstate::PublisherId& publisher() const { return publisher_; }
  const linkstate::WorkerBootId& boot() const { return boot_; }
  const linkstate::SourceId& source() const { return source_; }

  linkstate::PublishEvidenceRequest request(const std::string& link, linkstate::EvidenceClaim claim,
                                            linkstate::EvidenceKind kind =
                                                linkstate::EvidenceKind::CarrierState,
                                            linkstate::DegradationCause cause =
                                                linkstate::DegradationCause::None) {
    ++sequence_;
    linkstate::PublishEvidenceRequest publication;
    publication.publication = linkstate::PublicationId::from_validated(
        "pub." + publisher_.value() + "." + std::to_string(sequence_));
    linkstate::EvidenceRecord& observation = publication.observation;
    observation.key.link = link_id(link);
    observation.key.source = source_;
    observation.key.kind = kind;
    observation.observation = linkstate::ObservationId::from_validated(
        "obs." + publisher_.value() + "." + std::to_string(sequence_));
    observation.publisher = publisher_;
    observation.worker_boot = boot_;
    observation.epoch = engine_.coordinator_epoch();
    observation.source_generation = linkstate::SourceGeneration::from_value(1);
    observation.sequence = sequence_;
    observation.evidence_class = linkstate::EvidenceClass::Real;
    observation.claim = claim;
    observation.degradation_cause = cause;
    const std::optional<linkstate::TopologyBinding> binding = engine_.topology_binding(observation.key.link);
    if (binding.has_value()) {
      observation.topology_generation = binding->topology_generation;
    }
    return publication;
  }

  linkstate::LinkMutationResult publish(const std::string& link, linkstate::EvidenceClaim claim,
                                        linkstate::EvidenceKind kind =
                                            linkstate::EvidenceKind::CarrierState,
                                        linkstate::DegradationCause cause =
                                            linkstate::DegradationCause::None) {
    return engine_.publish_evidence(request(link, claim, kind, cause));
  }

  std::uint64_t next_sequence() const { return sequence_ + 1; }

  std::optional<linkstate::TopologyBinding> engine_binding(const std::string& link) const {
    return engine_.topology_binding(link_id(link));
  }

 private:
  linkstate::LinkStateEngine& engine_;
  linkstate::PublisherId publisher_;
  linkstate::SourceId source_;
  linkstate::WorkerBootId boot_;
  std::uint64_t sequence_ = 0;
  linkstate::LinkMutationResult last_registration_;
};

inline std::unordered_map<linkstate::LinkId, linkstate::HostBindingContext> binding_contexts(
    const linkstate::LinkStateEngine& engine, const std::vector<linkstate::LinkId>& links) {
  std::unordered_map<linkstate::LinkId, linkstate::HostBindingContext> contexts;
  for (const linkstate::LinkId& link : links) {
    const std::optional<linkstate::TopologyBinding> binding = engine.topology_binding(link);
    if (!binding.has_value()) {
      continue;
    }
    linkstate::HostBindingContext context;
    context.topology_generation = binding->topology_generation;
    context.endpoint_generation = binding->endpoints.local_generation;
    contexts.emplace(link, context);
  }
  return contexts;
}

}  // namespace lsf_test
