// Losing one publisher does not invalidate unrelated evidence: the runtime
// recomputes state from the evidence that remains.

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
  const SyntheticLinkSpec spec = example_link("loss");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});
  SyntheticFabric local(engine, PublisherId::from_validated("publisher.local"),
                        WorkerBootId::from_validated("boot.local"),
                        SourceId::from_validated("source.local"));
  SyntheticFabric peer(engine, PublisherId::from_validated("publisher.peer"),
                       WorkerBootId::from_validated("boot.peer"),
                       SourceId::from_validated("source.peer"));
  (void)local.rebind(engine.coordinator_epoch(), {spec.link});
  (void)peer.rebind(engine.coordinator_epoch(), {spec.link});
  (void)local.publish(spec.link, EvidenceClaim::Up);
  (void)peer.publish(spec.link, EvidenceClaim::Up);

  InvalidatePublisherRequest invalidation;
  invalidation.publisher = local.publisher();
  invalidation.worker_boot = local.worker_boot();
  invalidation.epoch = engine.coordinator_epoch();
  invalidation.reason = "local agent lost";
  std::printf("fence local           %s\n",
              engine.invalidate_publisher(invalidation).outcome.to_string().c_str());
  report(engine, spec.link);
  std::printf("peer evidence kept    %zu record(s)\n",
              engine.evidence_for_publisher(peer.publisher()).size());

  InvalidatePublisherRequest second;
  second.publisher = peer.publisher();
  second.worker_boot = peer.worker_boot();
  second.epoch = engine.coordinator_epoch();
  second.reason = "peer agent lost";
  std::printf("fence peer            %s\n",
              engine.invalidate_publisher(second).outcome.to_string().c_str());
  report(engine, spec.link);
  return 0;
}
