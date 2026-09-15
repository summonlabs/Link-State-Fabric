// A restarted worker gets a fresh boot identity. The previous incarnation is
// fenced permanently: its traffic can neither publish nor be replayed.

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
  const SyntheticLinkSpec spec = example_link("reincarnation");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});

  SyntheticFabric first(engine, PublisherId::from_validated("publisher.worker"),
                        WorkerBootId::from_validated("boot.1"),
                        SourceId::from_validated("source.worker"));
  (void)first.rebind(engine.coordinator_epoch(), {spec.link});
  std::printf("boot 1 up             %s\n",
              first.publish(spec.link, EvidenceClaim::Up).outcome.to_string().c_str());

  InvalidatePublisherRequest invalidation;
  invalidation.publisher = first.publisher();
  invalidation.worker_boot = first.worker_boot();
  invalidation.epoch = engine.coordinator_epoch();
  invalidation.reason = "worker process died";
  std::printf("fence boot 1          %s\n",
              engine.invalidate_publisher(invalidation).outcome.to_string().c_str());
  report(engine, spec.link);

  SyntheticFabric restarted(engine, PublisherId::from_validated("publisher.worker"),
                            WorkerBootId::from_validated("boot.2"),
                            SourceId::from_validated("source.worker"));
  std::printf("register boot 2       %s\n",
              restarted.rebind(engine.coordinator_epoch(), {spec.link}).to_string().c_str());
  const LinkMutationResult fresh = restarted.publish(spec.link, EvidenceClaim::Up);
  std::printf("boot 2 up             %s\n", fresh.outcome.to_string().c_str());
  report(engine, spec.link);

  SyntheticFabric old_boot(engine, PublisherId::from_validated("publisher.worker"),
                           WorkerBootId::from_validated("boot.1"),
                           SourceId::from_validated("source.worker"));
  const Outcome rejected = old_boot.rebind(engine.coordinator_epoch(), {spec.link});
  std::printf("replay boot 1         %s\n", rejected.to_string().c_str());
  return 0;
}
