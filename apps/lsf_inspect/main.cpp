// lsf-inspect: deterministic inspection of Link State Fabric state.
//
// Subcommands:
//   journal <path>   validate and describe a journal file
//   records <path>   list the durable link records held by a journal
//   digest <path>    print the journal file digest
//   host             enumerate real host visible link evidence
//   scenario <name>  run a labelled synthetic scenario and explain the result
//   help             usage

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/persistence.hpp"
#include "linkstate/platform.hpp"
#include "linkstate/synthetic.hpp"
#include "linkstate/version.hpp"

namespace {

int usage() {
  std::printf(
      "lsf-inspect %s\n"
      "usage:\n"
      "  lsf-inspect journal <path>    validate a journal and print its summary\n"
      "  lsf-inspect records <path>    print the durable link records of a journal\n"
      "  lsf-inspect digest <path>     print the journal file digest\n"
      "  lsf-inspect host              enumerate real host visible link evidence\n"
      "  lsf-inspect scenario <name>   run a synthetic scenario (up, down, degraded,\n"
      "                                conflict, admin-disable, topology-change)\n"
      "  lsf-inspect version           print the runtime version\n",
      linkstate::to_string(linkstate::runtime_version()).c_str());
  return 0;
}

int inspect_journal(const std::string& path, bool records_only) {
  linkstate::PersistenceConfig config;
  config.path = path;
  const std::string validation = config.validate();
  if (!validation.empty()) {
    std::printf("error: %s\n", validation.c_str());
    return 2;
  }
  std::vector<std::byte> bytes;
  linkstate::JournalSummary summary;
  const linkstate::Outcome read = linkstate::read_journal_file(config, bytes, summary);
  if (read.is_error()) {
    std::printf("error: %s\n", read.to_string().c_str());
    return 3;
  }
  linkstate::JournalPayload payload;
  const linkstate::Outcome decoded =
      linkstate::decode_journal(std::span<const std::byte>(bytes), config, payload, summary);
  if (decoded.is_error()) {
    std::printf("error: %s\n", decoded.to_string().c_str());
    return 3;
  }
  if (!records_only) {
    std::printf("%s", summary.render().c_str());
  }
  for (const linkstate::PersistedLink& link : payload.links) {
    std::printf("%s", link.record.render().c_str());
    std::printf("persisted_evidence    %zu\n", link.evidence.size());
    std::printf("\n");
  }
  std::printf("recovery_note         loading this journal restores durable records only;\n");
  std::printf("                      dynamic evidence and live authority are never restored\n");
  return 0;
}

int inspect_digest(const std::string& path) {
  linkstate::PersistenceConfig config;
  config.path = path;
  std::vector<std::byte> bytes;
  linkstate::JournalSummary summary;
  const linkstate::Outcome read = linkstate::read_journal_file(config, bytes, summary);
  if (read.is_error()) {
    std::printf("error: %s\n", read.to_string().c_str());
    return 3;
  }
  const linkstate::Digest digest =
      linkstate::sha256(std::span<const std::byte>(bytes.data(), bytes.size()));
  std::printf("file                  %s\n", path.c_str());
  std::printf("size                  %zu\n", bytes.size());
  std::printf("digest                %s\n", digest.to_hex().c_str());
  return 0;
}

int inspect_host() {
  const linkstate::HostDiscoveryReport report = linkstate::discover_host_links();
  std::printf("availability          %s\n",
              linkstate::to_string(report.availability));
  std::printf("interfaces            %zu\n", report.interfaces_enumerated);
  std::printf("observations          %zu\n", report.observations.size());
  std::printf("unsupported           %zu\n", report.unsupported_interfaces);
  for (const std::string& note : report.notes) {
    std::printf("note                  %s\n", note.c_str());
  }
  for (const linkstate::HostLinkObservation& observation : report.observations) {
    std::printf("%s", linkstate::render(observation).c_str());
    std::printf("\n");
  }
  return 0;
}

int run_scenario(const std::string& name) {
  linkstate::EngineConfig config;
  config.max_links = 64;
  config.max_publishers = 8;
  linkstate::LinkStateEngine engine(config);
  const linkstate::LinkId link = linkstate::LinkId::from_validated("link.scenario");

  linkstate::SyntheticLinkSpec spec;
  spec.fabric = linkstate::FabricId::from_validated("fabric.scenario");
  spec.site = linkstate::SiteId::from_validated("site.scenario");
  spec.link = link;
  spec.local_endpoint = linkstate::EndpointId::from_validated("ep.scenario.a");
  spec.remote_endpoint = linkstate::EndpointId::from_validated("ep.scenario.b");
  spec.topology_generation = linkstate::TopologyGeneration::from_value(1);
  spec.endpoint_generation = linkstate::EndpointGeneration::from_value(1);
  spec.link_class = linkstate::LinkClass::EthernetLike;
  if (engine.bind_link(linkstate::BindLinkRequest{linkstate::to_binding(spec)}).outcome.is_error()) {
    std::printf("error: the scenario link could not be bound\n");
    return 4;
  }

  linkstate::SyntheticFabric local(engine, linkstate::PublisherId::from_validated("publisher.local"),
                                   linkstate::WorkerBootId::from_validated("boot.local"),
                                   linkstate::SourceId::from_validated("source.local"));
  linkstate::SyntheticFabric peer(engine, linkstate::PublisherId::from_validated("publisher.peer"),
                                  linkstate::WorkerBootId::from_validated("boot.peer"),
                                  linkstate::SourceId::from_validated("source.peer"));
  std::vector<linkstate::LinkId> scope{link};
  if (local.rebind(engine.coordinator_epoch(), scope).is_error() ||
      peer.rebind(engine.coordinator_epoch(), scope).is_error()) {
    std::printf("error: the scenario publishers could not be registered\n");
    return 4;
  }

  if (name == "up") {
    std::printf("outcome               %s\n",
                local.publish(link, linkstate::EvidenceClaim::Up).outcome.to_string().c_str());
  } else if (name == "down") {
    (void)local.publish(link, linkstate::EvidenceClaim::Up);
    std::printf("outcome               %s\n",
                local.publish(link, linkstate::EvidenceClaim::Down).outcome.to_string().c_str());
  } else if (name == "degraded") {
    std::printf(
        "outcome               %s\n",
        local
            .publish(link, linkstate::EvidenceClaim::Degraded,
                     linkstate::DegradationCause::ErrorRateThresholdExceeded)
            .outcome.to_string()
            .c_str());
  } else if (name == "conflict") {
    (void)local.publish(link, linkstate::EvidenceClaim::Up);
    std::printf("outcome               %s\n",
                peer.publish(link, linkstate::EvidenceClaim::Down).outcome.to_string().c_str());
  } else if (name == "admin-disable") {
    (void)local.publish(link, linkstate::EvidenceClaim::Up);
    std::printf(
        "outcome               %s\n",
        local
            .publish(link, linkstate::EvidenceClaim::AdminDisabled, linkstate::DegradationCause::None,
                     linkstate::EvidenceKind::AdministrativeState)
            .outcome.to_string()
            .c_str());
  } else if (name == "topology-change") {
    (void)local.publish(link, linkstate::EvidenceClaim::Up);
    linkstate::SupersedeTopologyRequest supersession;
    supersession.link = link;
    supersession.new_topology_generation = linkstate::TopologyGeneration::from_value(2);
    supersession.reason = "endpoint replaced";
    linkstate::TopologyBinding replacement = linkstate::to_binding(spec);
    replacement.topology_generation = linkstate::TopologyGeneration::from_value(2);
    replacement.endpoints.local = linkstate::EndpointId::from_validated("ep.scenario.a2");
    supersession.replacement = replacement;
    std::printf("outcome               %s\n",
                engine.supersede_topology(supersession).outcome.to_string().c_str());
  } else {
    std::printf("error: unknown scenario '%s'\n", name.c_str());
    return 2;
  }

  const std::optional<linkstate::LinkStateView> view = engine.link_state(link);
  if (view.has_value()) {
    std::printf("state                 %s\n", linkstate::to_string(view->state()));
    std::printf("current_evidence      %zu\n", view->current_evidence);
  }
  std::printf("\n%s", engine.explain(link).render().c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string command = argv[1];
  if (command == "help" || command == "--help" || command == "-h") {
    return usage();
  }
  if (command == "version") {
    std::printf("link-state-fabric %s\n",
                linkstate::to_string(linkstate::runtime_version()).c_str());
    return 0;
  }
  if (command == "host") {
    return inspect_host();
  }
  if (command == "journal" || command == "records") {
    if (argc < 3) {
      std::printf("error: %s requires a journal path\n", command.c_str());
      return 2;
    }
    return inspect_journal(argv[2], command == "records");
  }
  if (command == "digest") {
    if (argc < 3) {
      std::printf("error: digest requires a journal path\n");
      return 2;
    }
    return inspect_digest(argv[2]);
  }
  if (command == "scenario") {
    if (argc < 3) {
      std::printf("error: scenario requires a name\n");
      return 2;
    }
    return run_scenario(argv[2]);
  }
  std::printf("error: unknown command '%s'\n", command.c_str());
  return 2;
}
