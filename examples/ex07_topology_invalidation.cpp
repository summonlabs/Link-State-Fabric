// Structural replacement invalidates operational state. Old evidence bound to
// the previous topology generation must not become authoritative for the new
// structure.

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
  const SyntheticLinkSpec spec = example_link("topology");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});
  SyntheticFabric fabric(engine, PublisherId::from_validated("publisher.agent"),
                         WorkerBootId::from_validated("boot.1"),
                         SourceId::from_validated("source.port-agent"));
  (void)fabric.rebind(engine.coordinator_epoch(), {spec.link});
  std::printf("up on generation 1    %s\n",
              fabric.publish(spec.link, EvidenceClaim::Up).outcome.to_string().c_str());
  report(engine, spec.link);

  SupersedeTopologyRequest supersession;
  supersession.link = spec.link;
  supersession.new_topology_generation = TopologyGeneration::from_value(2);
  supersession.reason = "endpoint replaced by Fabric Topology";
  TopologyBinding replacement = to_binding(spec);
  replacement.topology_generation = TopologyGeneration::from_value(2);
  replacement.endpoints.local = EndpointId::from_validated("ep.topology.a2");
  supersession.replacement = replacement;
  std::printf("supersede             %s\n",
              engine.supersede_topology(supersession).outcome.to_string().c_str());
  report(engine, spec.link);

  const LinkMutationResult stale = fabric.publish(spec.link, EvidenceClaim::Up);
  std::printf("publish on gen 1      %s\n", stale.outcome.to_string().c_str());
  std::printf("\n%s", engine.explain(spec.link).render().c_str());
  return 0;
}
