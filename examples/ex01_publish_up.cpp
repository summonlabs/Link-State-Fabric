// Publishes current evidence for a link and observes the authoritative state.
// The link is UNKNOWN until evidence establishes it: structural existence alone
// never implies UP.

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
  const SyntheticLinkSpec spec = example_link("up");
  std::printf("bind                  %s\n",
              engine.bind_link(BindLinkRequest{to_binding(spec)}).outcome.to_string().c_str());
  std::printf("state before evidence %s\n",
              to_string(engine.link_state(spec.link)->state()));

  SyntheticFabric fabric(engine, PublisherId::from_validated("publisher.agent"),
                         WorkerBootId::from_validated("boot.1"),
                         SourceId::from_validated("source.port-agent"));
  std::printf("register              %s\n",
              fabric.rebind(engine.coordinator_epoch(), {spec.link}).to_string().c_str());
  const LinkMutationResult published = fabric.publish(spec.link, EvidenceClaim::Up);
  std::printf("publish               %s\n", published.outcome.to_string().c_str());
  report(engine, spec.link);
  return 0;
}
