// Two independent sources contradict each other on a link class that cannot be
// asymmetric. The runtime refuses to force a binary answer: the link becomes
// CONFLICTED and requires revalidation.

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
  const SyntheticLinkSpec spec = example_link("conflict");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});
  SyntheticFabric local(engine, PublisherId::from_validated("publisher.local"),
                        WorkerBootId::from_validated("boot.local"),
                        SourceId::from_validated("source.local"));
  SyntheticFabric peer(engine, PublisherId::from_validated("publisher.peer"),
                       WorkerBootId::from_validated("boot.peer"),
                       SourceId::from_validated("source.peer"));
  (void)local.rebind(engine.coordinator_epoch(), {spec.link});
  (void)peer.rebind(engine.coordinator_epoch(), {spec.link});

  std::printf("local up              %s\n",
              local.publish(spec.link, EvidenceClaim::Up).outcome.to_string().c_str());
  const LinkMutationResult conflicted = peer.publish(spec.link, EvidenceClaim::Down);
  std::printf("peer down             %s\n", conflicted.outcome.to_string().c_str());
  report(engine, spec.link);
  std::printf("\n%s", engine.explain(spec.link).render().c_str());
  return 0;
}
