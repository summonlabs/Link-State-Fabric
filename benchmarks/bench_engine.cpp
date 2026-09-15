// Guarded benchmarks for Link State Fabric.
//
// Every benchmark measures completed work: each operation is asserted to have
// the expected outcome before its elapsed time is used, so a benchmark cannot
// report throughput for work that was rejected. Measured values are printed as
// observed on the running host; no target is invented.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/persistence.hpp"
#include "linkstate/synthetic.hpp"

using namespace linkstate;

namespace {

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void report(const char* label, std::size_t operations, double seconds) {
  const double per_operation = operations == 0 ? 0.0 : seconds / static_cast<double>(operations);
  std::printf("%-38s %9zu ops %8.3f s %10.3f us/op %12.0f ops/s\n", label, operations, seconds,
              per_operation * 1e6, per_operation > 0.0 ? 1.0 / per_operation : 0.0);
  std::fflush(stdout);
}

std::vector<SyntheticLinkSpec> inventory(std::size_t count) {
  return build_synthetic_inventory(count, FabricId::from_validated("fabric.bench"),
                                   SiteId::from_validated("site.bench"),
                                   LinkClass::SwitchToSwitch, TopologyGeneration::from_value(1),
                                   EndpointGeneration::from_value(1));
}

void bench_scale(std::size_t count) {
  EngineConfig config;
  config.max_links = count + 16;
  config.max_publishers = 64;
  config.max_evidence_per_link = 4;
  LinkStateEngine engine(config);
  const std::vector<SyntheticLinkSpec> links = inventory(count);

  const std::size_t scope_size = 8192;
  const std::size_t agent_count = (count + scope_size - 1) / scope_size;
  std::vector<SyntheticFabric> agents;
  agents.reserve(agent_count);

  {
    const Stopwatch watch;
    for (const SyntheticLinkSpec& spec : links) {
      if (engine.bind_link(BindLinkRequest{to_binding(spec)}).outcome.is_error()) {
        std::printf("bind failed\n");
        return;
      }
    }
    report("bind links", count, watch.seconds());
  }
  for (std::size_t index = 0; index < agent_count; ++index) {
    agents.emplace_back(engine,
                        PublisherId::from_validated("publisher.agent-" + std::to_string(index)),
                        WorkerBootId::from_validated("boot.agent-" + std::to_string(index)),
                        SourceId::from_validated("source.agent-" + std::to_string(index)));
    std::vector<LinkId> scope;
    const std::size_t first = index * scope_size;
    const std::size_t last = (std::min)(first + scope_size, count);
    for (std::size_t position = first; position < last; ++position) {
      scope.push_back(links[position].link);
    }
    if (agents.back().rebind(engine.coordinator_epoch(), scope).is_error()) {
      std::printf("registration failed\n");
      return;
    }
  }

  {
    const Stopwatch watch;
    std::size_t completed = 0;
    for (std::size_t index = 0; index < count; ++index) {
      SyntheticFabric& agent = agents[index / scope_size];
      if (agent.publish(links[index].link, EvidenceClaim::Up).outcome.code ==
          OutcomeCode::Committed) {
        ++completed;
      }
    }
    report("publish evidence (completed)", completed, watch.seconds());
  }
  {
    const Stopwatch watch;
    std::size_t found = 0;
    for (const SyntheticLinkSpec& spec : links) {
      if (engine.link_state(spec.link).has_value()) {
        ++found;
      }
    }
    report("query state", found, watch.seconds());
  }
  {
    const Stopwatch watch;
    Snapshot snapshot = engine.snapshot(SnapshotRequest{});
    report("snapshot", snapshot.records.size(), watch.seconds());
  }
  {
    const Stopwatch watch;
    const Digest digest = engine.state_digest();
    (void)digest;
    report("deterministic state digest", count, watch.seconds());
  }
  {
    const Stopwatch watch;
    std::size_t invalidated = 0;
    for (std::size_t index = 0; index < agent_count; ++index) {
      InvalidatePublisherRequest invalidation;
      invalidation.publisher = PublisherId::from_validated("publisher.agent-" +
                                                           std::to_string(index));
      invalidation.worker_boot = WorkerBootId::from_validated("boot.agent-" +
                                                              std::to_string(index));
      invalidation.epoch = engine.coordinator_epoch();
      invalidation.reason = "benchmark invalidation";
      if (engine.invalidate_publisher(invalidation).outcome.code == OutcomeCode::Committed) {
        ++invalidated;
      }
    }
    report("mass publisher invalidation", invalidated, watch.seconds());
  }
  {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "lsf-benchmark.lsfjournal";
    PersistenceConfig persistence;
    persistence.path = path.string();
    persistence.max_records = count + 16;
    const Stopwatch save_watch;
    const Outcome saved = engine.save(persistence);
    const double save_seconds = save_watch.seconds();
    if (!saved.is_error()) {
      report("persistence save", count, save_seconds);
    }
    LinkStateEngine restored(config);
    const Stopwatch load_watch;
    const LoadOutcome loaded = restored.load(persistence);
    const double load_seconds = load_watch.seconds();
    if (!loaded.outcome.is_error()) {
      report("persistence load", loaded.records_loaded, load_seconds);
    }
    std::error_code error;
    std::filesystem::remove(path, error);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t small = 10000;
  std::size_t large = 100000;
  bool run_large = true;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--small-only") {
      run_large = false;
    }
  }
  std::printf("Link State Fabric benchmarks (completed operations only)\n");
  std::printf("\n== %zu links ==\n", small);
  bench_scale(small);
  if (run_large) {
    std::printf("\n== %zu links ==\n", large);
    bench_scale(large);
  }
  std::printf("\nhost: %zu hardware threads\n",
              static_cast<std::size_t>(std::thread::hardware_concurrency()));
  return 0;
}
