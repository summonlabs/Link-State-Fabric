// Stale evidence is rejected before mutation, and an exact replay of an already
// committed observation is idempotent rather than stale.

#include <cstdio>
#include <string>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/synthetic.hpp"

using namespace linkstate;

namespace {

EngineConfig example_config() {
  EngineConfig config;
  config.max_links = 64;
  config.max_publishers = 8;
  config.max_evidence_per_link = 8;
  return config;
}

SyntheticLinkSpec example_link(const char* name, LinkClass link_class = LinkClass::EthernetLike) {
  SyntheticLinkSpec spec;
  spec.fabric = FabricId::from_validated("fabric.example");
  spec.site = SiteId::from_validated("site.example");
  spec.link = LinkId::from_validated(std::string("link.") + name);
  spec.local_endpoint = EndpointId::from_validated(std::string("ep.") + name + ".a");
  spec.remote_endpoint = EndpointId::from_validated(std::string("ep.") + name + ".b");
  spec.topology_generation = TopologyGeneration::from_value(1);
  spec.endpoint_generation = EndpointGeneration::from_value(1);
  spec.link_class = link_class;
  return spec;
}

void report(const LinkStateEngine& engine, const LinkId& link) {
  const std::optional<LinkStateView> view = engine.link_state(link);
  if (!view.has_value()) {
    std::printf("state                 <unbound>\n");
    return;
  }
  std::printf("state                 %s\n", to_string(view->state()));
  std::printf("state_generation      %s\n", view->record.state_generation.to_string().c_str());
  std::printf("evidence_generation   %s\n", view->record.evidence_generation.to_string().c_str());
  std::printf("current_evidence      %zu\n", view->current_evidence);
}

}  // namespace

int main() {
  LinkStateEngine engine(example_config());
  const SyntheticLinkSpec spec = example_link("stale");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});
  SyntheticFabric fabric(engine, PublisherId::from_validated("publisher.agent"),
                         WorkerBootId::from_validated("boot.1"),
                         SourceId::from_validated("source.port-agent"));
  (void)fabric.rebind(engine.coordinator_epoch(), {spec.link});

  const LinkMutationResult first = fabric.publish(spec.link, EvidenceClaim::Up);
  std::printf("first                 %s\n", first.outcome.to_string().c_str());
  std::printf("state generation      %s\n",
              engine.link_state(spec.link)->record.state_generation.to_string().c_str());

  // A compare and swap against an outdated state generation is refused.
  const std::uint64_t sequence = fabric.sequence_of(spec.link, fabric.source(),
                                                   EvidenceKind::SyntheticTest);
  PublishEvidenceRequest stale;
  stale.publication = PublicationId::from_validated("pub.stale");
  EvidenceRecord& observation = stale.observation;
  observation.key.link = spec.link;
  observation.key.source = fabric.source();
  observation.key.kind = EvidenceKind::SyntheticTest;
  observation.observation = ObservationId::from_validated("obs.stale");
  observation.publisher = fabric.publisher();
  observation.worker_boot = fabric.worker_boot();
  observation.epoch = engine.coordinator_epoch();
  observation.source_generation = SourceGeneration::from_value(1);
  observation.sequence = sequence;
  observation.topology_generation = TopologyGeneration::from_value(1);
  observation.evidence_class = EvidenceClass::Synthetic;
  observation.claim = EvidenceClaim::Up;
  stale.expected_state_generation = LinkStateGeneration::from_value(0);
  std::printf("stale cas             %s\n",
              engine.publish_evidence(stale).outcome.to_string().c_str());

  // Republishing the identical observation is an exact replay: the result is
  // IDEMPOTENT and no generation advances.
  const std::vector<EvidenceRecord> current = engine.evidence_for_link(spec.link);
  PublishEvidenceRequest replay;
  replay.publication = PublicationId::from_validated("pub.replay");
  replay.observation = current.front();
  std::printf("exact replay          %s\n",
              engine.publish_evidence(replay).outcome.to_string().c_str());
  report(engine, spec.link);
  return 0;
}
