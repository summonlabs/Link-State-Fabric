// lsf-worker: a real publisher process.
//
// The worker connects to a coordinator, registers its publisher incarnation and
// publishes evidence for the links it is authorized to observe. With --hold it
// then waits for a line on stdin so that a supervising test controls its
// lifetime; without --hold it exits immediately after publishing.

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "linkstate/client.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/version.hpp"

namespace {

void print_usage() {
  std::printf(
      "lsf-worker %s\n"
      "usage: lsf-worker --endpoint ADDR:PORT --publisher ID --boot ID --source ID\n"
      "                  --link ID [--link ID ...] [--claim UP|DOWN|DEGRADED|FAULTED|\n"
      "                  ADMIN_DISABLED|DRAINING|UNKNOWN] [--kind KIND] [--count N] [--hold]\n",
      linkstate::to_string(linkstate::runtime_version()).c_str());
}

std::optional<linkstate::EvidenceClaim> parse_claim(const std::string& text) {
  for (std::size_t index = 0; index < linkstate::evidence_claim_count; ++index) {
    const auto claim = static_cast<linkstate::EvidenceClaim>(index);
    if (text == linkstate::to_string(claim)) {
      return claim;
    }
  }
  return std::nullopt;
}

}  // namespace

int main(int argc, char** argv) {
  linkstate::PublisherClientConfig config;
  std::vector<std::string> links;
  linkstate::EvidenceClaim claim = linkstate::EvidenceClaim::Up;
  linkstate::EvidenceKind kind = linkstate::EvidenceKind::PlatformDiscovery;
  std::uint64_t count = 1;
  bool hold = false;
  std::string endpoint;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if (argument == "--hold") {
      hold = true;
      continue;
    }
    if (index + 1 >= argc) {
      std::printf("error: %s requires a value\n", argument.c_str());
      return 2;
    }
    const std::string value = argv[++index];
    if (argument == "--endpoint") {
      endpoint = value;
    } else if (argument == "--publisher") {
      config.publisher = linkstate::PublisherId::from_validated(value);
    } else if (argument == "--boot") {
      config.worker_boot = linkstate::WorkerBootId::from_validated(value);
    } else if (argument == "--source") {
      config.source = linkstate::SourceId::from_validated(value);
    } else if (argument == "--link") {
      links.push_back(value);
    } else if (argument == "--claim") {
      const std::optional<linkstate::EvidenceClaim> parsed = parse_claim(value);
      if (!parsed.has_value()) {
        std::printf("error: unknown claim %s\n", value.c_str());
        return 2;
      }
      claim = parsed.value();
    } else if (argument == "--kind") {
      const std::optional<linkstate::EvidenceKind> parsed =
          linkstate::parse_evidence_kind(value);
      if (!parsed.has_value()) {
        std::printf("error: unknown evidence kind %s\n", value.c_str());
        return 2;
      }
      kind = parsed.value();
    } else if (argument == "--count") {
      count = std::strtoull(value.c_str(), nullptr, 10);
    } else {
      std::printf("error: unknown option %s\n", argument.c_str());
      return 2;
    }
  }

  const std::size_t separator = endpoint.rfind(':');
  if (separator == std::string::npos) {
    std::printf("error: --endpoint must be ADDR:PORT\n");
    return 2;
  }
  config.address = endpoint.substr(0, separator);
  config.port = static_cast<std::uint16_t>(std::stoi(endpoint.substr(separator + 1)));
  for (const std::string& link : links) {
    const std::optional<linkstate::LinkId> parsed = linkstate::LinkId::parse(link);
    if (!parsed.has_value()) {
      std::printf("error: malformed link identity %s\n", link.c_str());
      return 2;
    }
    config.links.push_back(parsed.value());
  }
  config.agent_name = "lsf-worker";

  const std::string validation = config.validate();
  if (!validation.empty()) {
    std::printf("error: %s\n", validation.c_str());
    return 2;
  }

  linkstate::PublisherClient client(config);
  const linkstate::Outcome connected = client.connect();
  if (connected.is_error()) {
    std::printf("connect_failed %s\n", connected.to_string().c_str());
    std::fflush(stdout);
    return 3;
  }
  const linkstate::Outcome registered = client.register_publisher();
  if (registered.is_error()) {
    std::printf("register_failed %s\n", registered.to_string().c_str());
    std::fflush(stdout);
    return 3;
  }
  std::printf("connected epoch=%s\n", client.epoch().to_string().c_str());

  for (std::uint64_t sequence = 1; sequence <= count; ++sequence) {
    for (const linkstate::LinkId& link : config.links) {
      // The worker binds its observation to the exact structural generation the
      // coordinator currently holds for the link; evidence that names a stale
      // generation is rejected rather than silently applied.
      linkstate::LinkStateView binding;
      if (!client.query_state(link, binding)) {
        std::printf("publish %s SKIPPED unknown link\n", link.value().c_str());
        std::fflush(stdout);
        continue;
      }
      linkstate::PublishEvidenceRequest publication;
      publication.publication = linkstate::PublicationId::from_validated(
          "pub." + config.publisher.value() + "." + link.value() + "." +
          std::to_string(sequence));
      linkstate::EvidenceRecord& observation = publication.observation;
      observation.key.link = link;
      observation.key.source = config.source;
      observation.key.kind = kind;
      observation.observation = linkstate::ObservationId::from_validated(
          "obs." + config.publisher.value() + "." + link.value() + "." +
          std::to_string(sequence));
      observation.publisher = config.publisher;
      observation.worker_boot = config.worker_boot;
      observation.epoch = client.epoch();
      observation.source_generation = linkstate::SourceGeneration::from_value(1);
      observation.sequence = sequence;
      // Link scoped observation: the worker speaks for the link as a whole, so
      // no endpoint generation is asserted. An endpoint scoped publisher would
      // bind binding.record.binding.endpoints.local_generation instead.
      observation.topology_generation = binding.record.binding.topology_generation;
      observation.evidence_class = linkstate::EvidenceClass::Synthetic;
      observation.claim = claim;
      observation.degradation_cause =
          claim == linkstate::EvidenceClaim::Degraded
              ? linkstate::DegradationCause::HardwareReportedImpairment
              : linkstate::DegradationCause::None;
      observation.detail = "worker publication from " + config.publisher.value();
      linkstate::LinkStateView view;
      const linkstate::Outcome outcome = client.publish(publication, &view);
      std::printf("publish %s %s state=%s\n", link.value().c_str(),
                  outcome.to_string().c_str(), linkstate::to_string(view.state()));
    }
  }
  std::fflush(stdout);

  if (hold) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "stop") {
        break;
      }
    }
  }

  const linkstate::Outcome closed = client.disconnect();
  std::printf("closed %s\n", closed.to_string().c_str());
  std::fflush(stdout);
  return 0;
}
