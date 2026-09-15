// lsf-coordinator: hosts the authoritative link-state engine and serves the
// distributed publication protocol.
//
// The coordinator prints a deterministic banner on stdout after it starts
// listening, so a supervising process can discover the bound endpoint:
//
//   endpoint <address>:<port>
//   epoch <n>
//   links <n>
//   ready

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/persistence.hpp"
#include "linkstate/server.hpp"
#include "linkstate/synthetic.hpp"
#include "linkstate/version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT || signal == CTRL_BREAK_EVENT) {
    g_stop = true;
    return TRUE;
  }
  return FALSE;
}
#endif

void print_usage() {
  std::printf(
      "lsf-coordinator %s\n"
      "usage: lsf-coordinator [--journal PATH] [--address ADDR] [--port N]\n"
      "                       [--name NAME] [--bind-file PATH] [--fence-on-shutdown]\n"
      "                       [--no-durable-writes]\n"
      "  --journal PATH        journal file loaded at start and written after every\n"
      "                        accepted mutation and at shutdown\n"
      "  --address ADDR        listening address (default 127.0.0.1)\n"
      "  --port N              listening port, 0 selects an ephemeral port (default 0)\n"
      "  --bind-file PATH      bind links from a text file; one link per line:\n"
      "                        <link> <local-endpoint> <remote-endpoint>\n"
      "                        [LINK_CLASS] [topology-generation] [endpoint-generation]\n",
      linkstate::to_string(linkstate::runtime_version()).c_str());
}

bool bind_from_file(linkstate::LinkStateEngine& engine, const std::string& path,
                    std::string& error) {
  std::ifstream input(path);
  if (!input) {
    error = "the bind file could not be opened";
    return false;
  }
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::istringstream stream(line);
    std::string link;
    std::string local;
    std::string remote;
    std::string link_class;
    std::uint64_t topology = 1;
    std::uint64_t endpoints = 1;
    if (!(stream >> link >> local >> remote)) {
      error = "bind file line " + std::to_string(line_number) + " is malformed";
      return false;
    }
    if (stream >> link_class) {
      (void)link_class;
    }
    if (stream >> topology) {
      (void)topology;
    }
    if (stream >> endpoints) {
      (void)endpoints;
    }
    linkstate::SyntheticLinkSpec spec;
    spec.fabric = linkstate::FabricId::from_validated("fabric.coordinator");
    spec.site = linkstate::SiteId::from_validated("site.coordinator");
    const std::optional<linkstate::LinkId> parsed_link = linkstate::LinkId::parse(link);
    const std::optional<linkstate::EndpointId> parsed_local =
        linkstate::EndpointId::parse(local);
    const std::optional<linkstate::EndpointId> parsed_remote =
        linkstate::EndpointId::parse(remote);
    if (!parsed_link.has_value() || !parsed_local.has_value() || !parsed_remote.has_value()) {
      error = "bind file line " + std::to_string(line_number) + " holds a malformed identity";
      return false;
    }
    spec.link = parsed_link.value();
    spec.local_endpoint = parsed_local.value();
    spec.remote_endpoint = parsed_remote.value();
    spec.topology_generation = linkstate::TopologyGeneration::from_value(topology);
    spec.endpoint_generation = linkstate::EndpointGeneration::from_value(endpoints);
    spec.link_class = linkstate::LinkClass::EthernetLike;
    if (!link_class.empty()) {
      const std::optional<linkstate::LinkClass> parsed_class =
          linkstate::parse_link_class(link_class);
      if (!parsed_class.has_value()) {
        error = "bind file line " + std::to_string(line_number) + " holds an unknown link class";
        return false;
      }
      spec.link_class = parsed_class.value();
    }
    const linkstate::LinkMutationResult bound =
        engine.bind_link(linkstate::BindLinkRequest{linkstate::to_binding(spec)});
    if (bound.outcome.is_error()) {
      error = "bind file line " + std::to_string(line_number) + ": " +
              bound.outcome.to_string();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  linkstate::CoordinatorConfig config;
  linkstate::PersistenceConfig persistence;
  std::string bind_file;
  bool use_journal = false;
  bool durable_writes = true;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if (argument == "--fence-on-shutdown") {
      config.fence_on_shutdown = true;
      continue;
    }
    if (argument == "--no-durable-writes") {
      durable_writes = false;
      continue;
    }
    if (index + 1 >= argc) {
      std::printf("error: %s requires a value\n", argument.c_str());
      return 2;
    }
    const std::string value = argv[++index];
    if (argument == "--journal") {
      persistence.path = value;
      use_journal = true;
    } else if (argument == "--bind-file") {
      bind_file = value;
    } else if (argument == "--address") {
      config.bind_address = value;
    } else if (argument == "--port") {
      config.port = static_cast<std::uint16_t>(std::stoi(value));
    } else if (argument == "--name") {
      config.coordinator_name = value;
    } else {
      std::printf("error: unknown option %s\n", argument.c_str());
      return 2;
    }
  }

  linkstate::EngineConfig engine_config;
  engine_config.max_links = 100000;
  engine_config.max_publishers = 4096;
  linkstate::LinkStateEngine engine(engine_config);

  if (use_journal) {
    const linkstate::LoadOutcome loaded = engine.load(persistence);
    if (loaded.outcome.is_error() &&
        loaded.outcome.code != linkstate::OutcomeCode::PersistenceIoFailure) {
      std::printf("error: journal could not be loaded: %s\n", loaded.outcome.to_string().c_str());
      return 3;
    }
    if (!loaded.outcome.is_error()) {
      std::printf("recovered %zu durable records, %zu require revalidation\n", loaded.records_loaded,
                  loaded.records_revalidated);
      // A restart never inherits live authority: the epoch advances so that any
      // surviving or replayed traffic from the previous epoch is rejected.
      linkstate::AdvanceEpochRequest advance;
      advance.new_epoch =
          linkstate::CoordinatorEpoch::from_value(engine.coordinator_epoch().value() + 1);
      advance.reason = "coordinator restart";
      const linkstate::LinkMutationResult advanced = engine.advance_coordinator_epoch(advance);
      if (advanced.outcome.is_error()) {
        std::printf("error: coordinator epoch could not be advanced\n");
        return 3;
      }
      std::printf("epoch advanced to %s\n", engine.coordinator_epoch().to_string().c_str());
    }
  }

  if (!bind_file.empty()) {
    std::string error;
    if (!bind_from_file(engine, bind_file, error)) {
      std::printf("error: %s\n", error.c_str());
      return 3;
    }
    std::printf("bound %zu links from %s\n", engine.link_count(), bind_file.c_str());
  }

  if (use_journal && durable_writes) {
    config.journal_path = persistence.path;
    config.journal_history = persistence.include_history;
  }

  // Session lifecycle observability: one deterministic line per closed session,
  // emitted after the socket is closed and after fencing completed.
  config.session_observer = [](const linkstate::SessionEvent& event) {
    if (event.kind != linkstate::SessionEvent::Kind::Closed) {
      return;
    }
    std::printf("session closed agent=%s registrations=%zu fenced=%zu\n",
                event.agent.empty() ? "<unnamed>" : event.agent.c_str(), event.registrations,
                event.fenced);
    if (event.fenced > 0) {
      std::printf("fenced publisher=%s boot=%s\n", event.publisher.value().c_str(),
                  event.worker_boot.value().c_str());
    }
    std::fflush(stdout);
  };

  linkstate::CoordinatorServer server(engine, config);
  const linkstate::Outcome started = server.start();
  if (started.is_error()) {
    std::printf("error: %s\n", started.to_string().c_str());
    return 4;
  }

  std::printf("endpoint %s\n", server.endpoint().c_str());
  std::printf("epoch %s\n", engine.coordinator_epoch().to_string().c_str());
  std::printf("links %zu\n", engine.link_count());
  std::printf("ready\n");
  std::fflush(stdout);

#ifdef _WIN32
  SetConsoleCtrlHandler(console_handler, TRUE);
#endif

  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const linkstate::Outcome stopped = server.stop();
  if (use_journal) {
    const linkstate::Outcome saved = engine.save(persistence);
    if (saved.is_error()) {
      std::printf("error: journal could not be written: %s\n", saved.to_string().c_str());
      return 5;
    }
  }
  std::printf("stopped %s frames=%llu rejected=%llu sessions=%llu\n",
              stopped.to_string().c_str(),
              static_cast<unsigned long long>(server.frames_received()),
              static_cast<unsigned long long>(server.frames_rejected()),
              static_cast<unsigned long long>(server.sessions_accepted()));
  std::fflush(stdout);
  return 0;
}
