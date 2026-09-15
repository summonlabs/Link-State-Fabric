#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/snapshot.hpp"
#include "lsf_test.hpp"

using namespace linkstate;

namespace {

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}
  double seconds() const {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void report(const char* label, std::size_t operations, double seconds) {
  const double per_operation = operations == 0 ? 0.0 : seconds / static_cast<double>(operations);
  std::printf("  measured %-34s %8zu ops in %7.3f s  (%8.3f us/op)\n", label, operations, seconds,
              per_operation * 1e6);
  std::fflush(stdout);
}

EngineConfig scale_config(std::size_t max_links) {
  EngineConfig config;
  config.max_links = max_links;
  config.max_publishers = 64;
  config.max_evidence_per_link = 4;
  config.max_history_per_link = 4;
  config.max_batch_size = 4096;
  return config;
}

void run_scale(std::size_t link_count, const char* label) {
  LinkStateEngine engine(scale_config(link_count + 16));
  std::vector<LinkId> links;
  links.reserve(link_count);
  for (std::size_t index = 0; index < link_count; ++index) {
    links.push_back(LinkId::from_validated("link.scale." + std::to_string(index)));
  }

  // One publisher per observation domain, mirroring a fabric where each agent
  // observes the links it is wired to rather than the whole fabric.
  const std::size_t scope_size = 8192;
  const std::size_t publisher_count = (link_count + scope_size - 1) / scope_size;
  struct Agent {
    PublisherId publisher;
    WorkerBootId boot;
    SourceId source;
    std::vector<LinkId> links;
  };
  std::vector<Agent> agents;
  agents.reserve(publisher_count);
  for (std::size_t index = 0; index < publisher_count; ++index) {
    Agent agent;
    agent.publisher =
        lsf_test::publisher_id("scale-agent-" + std::to_string(index));
    agent.boot = lsf_test::boot_id("scale-boot-" + std::to_string(index));
    agent.source = lsf_test::source_id("scale-source-" + std::to_string(index));
    const std::size_t first = index * scope_size;
    const std::size_t last = (std::min)(first + scope_size, link_count);
    agent.links.assign(links.begin() + static_cast<std::ptrdiff_t>(first),
                       links.begin() + static_cast<std::ptrdiff_t>(last));
    agents.push_back(std::move(agent));
  }

  const Stopwatch bind_watch;
  for (std::size_t index = 0; index < link_count; ++index) {
    SyntheticLinkSpec spec;
    spec.fabric = lsf_test::fabric_id("scale");
    spec.site = lsf_test::site_id("scale");
    spec.link = links[index];
    spec.local_endpoint = EndpointId::from_validated("ep.scale." + std::to_string(index) + ".a");
    spec.remote_endpoint = EndpointId::from_validated("ep.scale." + std::to_string(index) + ".b");
    spec.topology_generation = TopologyGeneration::from_value(1);
    spec.endpoint_generation = EndpointGeneration::from_value(1);
    spec.link_class = LinkClass::SwitchToSwitch;
    LSF_CHECK(engine.bind_link(BindLinkRequest{to_binding(spec)}).outcome.code ==
              OutcomeCode::Committed);
  }
  report((std::string(label) + " bind").c_str(), link_count, bind_watch.seconds());
  LSF_CHECK(engine.link_count() == link_count);

  for (const Agent& agent : agents) {
    PublisherRegistration registration;
    registration.publisher = agent.publisher;
    registration.worker_boot = agent.boot;
    registration.source = agent.source;
    registration.scope.links = agent.links;
    registration.epoch = engine.coordinator_epoch();
    LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
              OutcomeCode::Committed);
  }

  const Stopwatch publish_watch;
  for (const Agent& agent : agents) {
    for (const LinkId& link : agent.links) {
      PublishEvidenceRequest publication;
      publication.publication =
          PublicationId::from_validated("pub." + link.value() + "." + agent.publisher.value());
      EvidenceRecord& observation = publication.observation;
      observation.key.link = link;
      observation.key.source = agent.source;
      observation.key.kind = EvidenceKind::CarrierState;
      observation.observation =
          ObservationId::from_validated("obs." + link.value() + "." + agent.publisher.value());
      observation.publisher = agent.publisher;
      observation.worker_boot = agent.boot;
      observation.epoch = engine.coordinator_epoch();
      observation.source_generation = SourceGeneration::from_value(1);
      observation.sequence = 1;
      observation.topology_generation = TopologyGeneration::from_value(1);
      observation.claim = EvidenceClaim::Up;
      const LinkMutationResult result = engine.publish_evidence(publication);
      LSF_CHECK(result.outcome.code == OutcomeCode::Committed);
    }
  }
  report((std::string(label) + " publish").c_str(), link_count, publish_watch.seconds());
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).size() == link_count);
  LSF_CHECK(engine.evidence_count() == link_count);

  const Stopwatch query_watch;
  for (std::size_t index = 0; index < link_count; ++index) {
    const std::optional<LinkStateView> view = engine.link_state(links[index]);
    LSF_CHECK(view.has_value());
  }
  report((std::string(label) + " query").c_str(), link_count, query_watch.seconds());

  const Stopwatch snapshot_watch;
  SnapshotRequest request;
  request.label = label;
  const Snapshot snapshot = engine.snapshot(request);
  report((std::string(label) + " snapshot").c_str(), snapshot.records.size(),
         snapshot_watch.seconds());
  LSF_CHECK(snapshot.records.size() == link_count);
  LSF_CHECK(engine.snapshot_is_current(snapshot));

  const Stopwatch digest_watch;
  const Digest digest = engine.state_digest();
  report((std::string(label) + " state digest").c_str(), link_count, digest_watch.seconds());
  LSF_CHECK(!digest.is_zero());

  const Stopwatch revalidate_watch;
  for (const Agent& agent : agents) {
    InvalidatePublisherRequest invalidation;
    invalidation.publisher = agent.publisher;
    invalidation.worker_boot = agent.boot;
    invalidation.epoch = engine.coordinator_epoch();
    invalidation.reason = "mass invalidation";
    LSF_CHECK(engine.invalidate_publisher(invalidation).outcome.code == OutcomeCode::Committed);
  }
  report((std::string(label) + " mass revalidation").c_str(), link_count,
         revalidate_watch.seconds());
  LSF_CHECK(engine.links_requiring_revalidation().size() == link_count);
  LSF_CHECK(engine.evidence_count() == 0);
}

}  // namespace

LSF_TEST(snapshot_binds_generations_and_reports_currentness) {
  LinkStateEngine engine(scale_config(32));
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("a")}).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("b")}).outcome.code ==
              OutcomeCode::Committed);
  lsf_test::TestPublisher publisher(engine, "agent",
                                    {lsf_test::link_id("a"), lsf_test::link_id("b")}, "source",
                                    "boot");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);

  SnapshotRequest request;
  request.label = "before";
  const Snapshot first = engine.snapshot(request);
  LSF_CHECK(first.records.size() == 2);
  LSF_CHECK(first.epoch == engine.coordinator_epoch());
  LSF_CHECK(first.global_state_generation == engine.global_state_generation());
  LSF_CHECK(engine.snapshot_is_current(first));
  LSF_CHECK(first.render().find("snapshot") != std::string::npos);

  LSF_REQUIRE(engine.publish_evidence(publisher.request("b", EvidenceClaim::Down)).outcome.code ==
              OutcomeCode::Committed);
  LSF_CHECK(!engine.snapshot_is_current(first));
  const std::optional<Snapshot> retained = engine.retained_snapshot(first.id);
  LSF_REQUIRE(retained.has_value());
  LSF_CHECK(retained->digest == first.digest);
  LSF_CHECK(retained->records.size() == 2);

  SnapshotRequest second_request;
  second_request.state_filter = LinkOperationalState::Down;
  const Snapshot second = engine.snapshot(second_request);
  LSF_CHECK(second.records.size() == 1);
  LSF_CHECK(second.records.front().link == lsf_test::link_id("b"));
  LSF_CHECK(second.digest != first.digest);

  SnapshotRequest third_request;
  third_request.links = {lsf_test::link_id("a")};
  const Snapshot third = engine.snapshot(third_request);
  LSF_CHECK(third.records.size() == 1);

  const std::vector<SnapshotId> ids = engine.retained_snapshot_ids();
  LSF_CHECK(ids.size() == 3);
}

LSF_TEST(snapshot_registry_is_bounded) {
  EngineConfig config = scale_config(16);
  config.max_retained_snapshots = 4;
  LinkStateEngine engine(config);
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("a")}).outcome.code ==
              OutcomeCode::Committed);
  std::vector<SnapshotId> ids;
  for (int index = 0; index < 10; ++index) {
    ids.push_back(engine.snapshot(SnapshotRequest{}).id);
  }
  const std::vector<SnapshotId> retained = engine.retained_snapshot_ids();
  LSF_CHECK_EQ(retained.size(), std::size_t{4});
  LSF_CHECK(!engine.retained_snapshot(ids.front()).has_value());
  LSF_CHECK(engine.retained_snapshot(ids.back()).has_value());
}

LSF_TEST(snapshot_digest_is_insertion_order_independent) {
  std::vector<SnapshotRecord> forward;
  std::vector<SnapshotRecord> reverse;
  for (int index = 0; index < 16; ++index) {
    SnapshotRecord record;
    record.link = LinkId::from_validated("link.snap." + std::to_string(index));
    record.state = index % 2 == 0 ? LinkOperationalState::Up : LinkOperationalState::Down;
    record.state_generation = LinkStateGeneration::from_value(static_cast<std::uint64_t>(index));
    forward.push_back(record);
    reverse.insert(reverse.begin(), record);
  }
  const Digest first =
      digest_snapshot(forward, GlobalGeneration::from_value(9),
                      LinkStateGeneration::from_value(4), CoordinatorEpoch::from_value(2));
  const Digest second =
      digest_snapshot(reverse, GlobalGeneration::from_value(9),
                      LinkStateGeneration::from_value(4), CoordinatorEpoch::from_value(2));
  LSF_CHECK(first == second);
  reverse.front().state = LinkOperationalState::Faulted;
  LSF_CHECK(digest_snapshot(reverse, GlobalGeneration::from_value(9),
                            LinkStateGeneration::from_value(4),
                            CoordinatorEpoch::from_value(2)) != first);
}

LSF_TEST(scale_ten_thousand_links) { run_scale(10000, "10k"); }

LSF_TEST(scale_one_hundred_thousand_links) { run_scale(100000, "100k"); }

LSF_TEST(synthetic_fabric_backend_exercises_the_public_api) {
  LinkStateEngine engine(scale_config(256));
  SyntheticFabricSpec spec;
  spec.fabric = lsf_test::fabric_id("synthetic");
  spec.site = lsf_test::site_id("synthetic");
  spec.switch_count = 4;
  spec.links_per_switch_pair = 2;
  spec.site_count = 2;
  spec.topology_generation = TopologyGeneration::from_value(1);
  spec.endpoint_generation = EndpointGeneration::from_value(1);
  spec.link_class = LinkClass::SwitchToSwitch;
  const std::vector<SyntheticLinkSpec> links = build_synthetic_fabric(spec);
  LSF_CHECK(links.size() > 6);
  std::size_t multi_site = 0;
  for (const SyntheticLinkSpec& link : links) {
    if (link.local_endpoint.valid() && link.remote_endpoint.valid()) {
      ++multi_site;
    }
  }
  LSF_CHECK(multi_site == links.size());

  SyntheticFabric fabric(engine, lsf_test::publisher_id("synthetic-agent"),
                         lsf_test::boot_id("synthetic-boot"),
                         lsf_test::source_id("synthetic-source"));
  LSF_REQUIRE(fabric.bind(links).code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_count() == links.size());
  LSF_REQUIRE(fabric.publish(links.front().link, EvidenceClaim::Up).outcome.code ==
              OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links.front().link)->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.link_state(links.front().link)->synthetic_backed);
  LSF_REQUIRE(fabric.publish(links.front().link, EvidenceClaim::Degraded,
                             DegradationCause::DegradedOpticalSignal)
                  .outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links.front().link)->state() == LinkOperationalState::Degraded);
  LSF_REQUIRE(fabric.publish(links.front().link, EvidenceClaim::AdminDisabled).outcome.code ==
              OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links.front().link)->state() ==
            LinkOperationalState::AdminDisabled);

  const std::vector<EvidenceRecord> records = engine.evidence_for_link(links.front().link);
  LSF_REQUIRE(records.size() == 1);
  LSF_CHECK(records.front().evidence_class == EvidenceClass::Synthetic);
  LSF_REQUIRE(fabric.withdraw(links.front().link, fabric.source()).outcome.code ==
              OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links.front().link)->state() ==
            LinkOperationalState::RevalidationRequired);
  LSF_CHECK(fabric.sequence_of(links.front().link, fabric.source(),
                               EvidenceKind::SyntheticTest) >= 4);
}

LSF_TEST(synthetic_inventory_supports_large_scale_inventories) {
  LinkStateEngine engine(scale_config(5000));
  const std::vector<SyntheticLinkSpec> links = build_synthetic_inventory(
      2000, lsf_test::fabric_id("inventory"), lsf_test::site_id("inventory"),
      LinkClass::InfiniBandLike, TopologyGeneration::from_value(1),
      EndpointGeneration::from_value(1));
  LSF_REQUIRE(links.size() == 2000);
  SyntheticFabric fabric(engine, lsf_test::publisher_id("inventory-agent"),
                         lsf_test::boot_id("inventory-boot"),
                         lsf_test::source_id("inventory-source"));
  LSF_REQUIRE(fabric.bind(links).code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_count() == 2000);
  const Stopwatch watch;
  for (const SyntheticLinkSpec& link : links) {
    LSF_CHECK(fabric.publish(link.link, EvidenceClaim::Up).outcome.code == OutcomeCode::Committed);
  }
  report("synthetic inventory publish", links.size(), watch.seconds());
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).size() == 2000);
}
