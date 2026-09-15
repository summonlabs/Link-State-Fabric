// A restart restores durable structure, never live authority: every recovered
// link requires fresh evidence before it becomes authoritative again.

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

#include <filesystem>

#include "linkstate/persistence.hpp"

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "lsf-example-recovery.lsfjournal";
  PersistenceConfig persistence;
  persistence.path = path.string();
  persistence.max_records = 64;

  LinkStateEngine engine(example_config());
  const SyntheticLinkSpec spec = example_link("recovery");
  (void)engine.bind_link(BindLinkRequest{to_binding(spec)});
  SyntheticFabric fabric(engine, PublisherId::from_validated("publisher.agent"),
                         WorkerBootId::from_validated("boot.1"),
                         SourceId::from_validated("source.port-agent"));
  (void)fabric.rebind(engine.coordinator_epoch(), {spec.link});
  (void)fabric.publish(spec.link, EvidenceClaim::Up);
  std::printf("before restart        %s\n",
              to_string(engine.link_state(spec.link)->state()));
  std::printf("save                  %s\n", engine.save(persistence).to_string().c_str());

  LinkStateEngine recovered(example_config());
  const LoadOutcome loaded = recovered.load(persistence);
  std::printf("load                  %s\n", loaded.outcome.to_string().c_str());
  std::printf("records loaded        %zu (revalidated %zu)\n", loaded.records_loaded,
              loaded.records_revalidated);
  std::printf("after restart         %s\n",
              to_string(recovered.link_state(spec.link)->state()));
  std::printf("evidence restored     %zu\n", recovered.evidence_count());

  SyntheticFabric fresh(recovered, PublisherId::from_validated("publisher.agent"),
                        WorkerBootId::from_validated("boot.2"),
                        SourceId::from_validated("source.port-agent"));
  (void)fresh.rebind(recovered.coordinator_epoch(), {spec.link});
  (void)fresh.publish(spec.link, EvidenceClaim::Up);
  std::printf("after fresh evidence  %s\n",
              to_string(recovered.link_state(spec.link)->state()));
  std::error_code error;
  std::filesystem::remove(path, error);
  return 0;
}
