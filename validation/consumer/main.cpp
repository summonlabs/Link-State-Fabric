// Independent find_package consumer of Link State Fabric.
//
// It uses only the installed public headers and the exported namespaced target:
// it binds a link, registers a publisher, publishes current evidence, queries
// the resulting authoritative state and verifies the deterministic digest.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <linkstate/engine.hpp>
#include <linkstate/version.hpp>

int main() {
  std::printf("link-state-fabric consumer, runtime %s\n",
              linkstate::to_string(linkstate::runtime_version()).c_str());

  linkstate::EngineConfig config;
  config.max_links = 16;
  config.max_publishers = 4;
  linkstate::LinkStateEngine engine(config);

  linkstate::TopologyBinding binding;
  binding.fabric = linkstate::FabricId::from_validated("fabric.consumer");
  binding.site = linkstate::SiteId::from_validated("site.consumer");
  binding.link = linkstate::LinkId::from_validated("link.consumer");
  binding.topology_generation = linkstate::TopologyGeneration::from_value(1);
  binding.endpoints.local = linkstate::EndpointId::from_validated("ep.consumer.a");
  binding.endpoints.local_generation = linkstate::EndpointGeneration::from_value(1);
  binding.endpoints.remote = linkstate::EndpointId::from_validated("ep.consumer.b");
  binding.endpoints.remote_generation = linkstate::EndpointGeneration::from_value(1);
  binding.link_class = linkstate::LinkClass::EthernetLike;
  binding.symmetry = linkstate::LinkSymmetryGuarantee::Symmetric;

  const linkstate::LinkMutationResult bound =
      engine.bind_link(linkstate::BindLinkRequest{binding});
  if (bound.outcome.is_error()) {
    std::printf("bind failed: %s\n", bound.outcome.to_string().c_str());
    return EXIT_FAILURE;
  }

  linkstate::PublisherRegistration registration;
  registration.publisher = linkstate::PublisherId::from_validated("publisher.consumer");
  registration.worker_boot = linkstate::WorkerBootId::from_validated("boot.consumer");
  registration.source = linkstate::SourceId::from_validated("source.consumer");
  registration.scope.links = {binding.link};
  registration.epoch = engine.coordinator_epoch();
  const linkstate::LinkMutationResult registered =
      engine.register_publisher(linkstate::RegisterPublisherRequest{registration});
  if (registered.outcome.is_error()) {
    std::printf("registration failed: %s\n", registered.outcome.to_string().c_str());
    return EXIT_FAILURE;
  }

  linkstate::PublishEvidenceRequest publication;
  publication.publication = linkstate::PublicationId::from_validated("pub.consumer.1");
  linkstate::EvidenceRecord& observation = publication.observation;
  observation.key.link = binding.link;
  observation.key.source = registration.source;
  observation.key.kind = linkstate::EvidenceKind::PlatformDiscovery;
  observation.observation = linkstate::ObservationId::from_validated("obs.consumer.1");
  observation.publisher = registration.publisher;
  observation.worker_boot = registration.worker_boot;
  observation.epoch = engine.coordinator_epoch();
  observation.source_generation = linkstate::SourceGeneration::from_value(1);
  observation.sequence = 1;
  observation.topology_generation = binding.topology_generation;
  observation.evidence_class = linkstate::EvidenceClass::Real;
  observation.claim = linkstate::EvidenceClaim::Up;
  observation.detail = "installed package consumer evidence";

  const linkstate::LinkMutationResult published = engine.publish_evidence(publication);
  if (published.outcome.code != linkstate::OutcomeCode::Committed) {
    std::printf("publication failed: %s\n", published.outcome.to_string().c_str());
    return EXIT_FAILURE;
  }

  const std::optional<linkstate::LinkStateView> view = engine.link_state(binding.link);
  if (!view.has_value()) {
    std::printf("query failed: link is not bound\n");
    return EXIT_FAILURE;
  }
  std::printf("link %s state %s generation %s\n", view->link().value().c_str(),
              linkstate::to_string(view->state()),
              view->record.state_generation.to_string().c_str());
  if (view->state() != linkstate::LinkOperationalState::Up) {
    std::printf("unexpected state\n");
    return EXIT_FAILURE;
  }
  const linkstate::Digest digest = engine.state_digest();
  std::printf("state digest %s\n", digest.to_hex().c_str());
  if (digest.is_zero()) {
    std::printf("digest is empty\n");
    return EXIT_FAILURE;
  }
  const linkstate::Explanation explanation = engine.explain(binding.link);
  std::printf("explanation rules %zu\n", explanation.rules.size());
  if (explanation.rules.empty()) {
    std::printf("explanation is empty\n");
    return EXIT_FAILURE;
  }
  std::printf("consumer validation succeeded\n");
  return EXIT_SUCCESS;
}
