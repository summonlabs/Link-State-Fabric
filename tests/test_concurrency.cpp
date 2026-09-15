#include <algorithm>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <filesystem>
#include <latch>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "helpers.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/persistence.hpp"
#include "lsf_test.hpp"

using namespace linkstate;
using lsf_test::binding_for;
using lsf_test::link_id;
using lsf_test::TestPublisher;

namespace {

EngineConfig concurrent_config() {
  EngineConfig config;
  config.max_links = 4096;
  config.max_publishers = 256;
  config.max_evidence_per_link = 8;
  config.max_history_per_link = 4;
  return config;
}

PublishEvidenceRequest direct_request(const PublisherId& publisher, const WorkerBootId& boot,
                                      const SourceId& source, const LinkId& link,
                                      EvidenceClaim claim, std::uint64_t sequence,
                                      const std::string& tag) {
  PublishEvidenceRequest publication;
  publication.publication = PublicationId::from_validated("pub." + tag);
  EvidenceRecord& observation = publication.observation;
  observation.key.link = link;
  observation.key.source = source;
  observation.key.kind = EvidenceKind::CarrierState;
  observation.observation = ObservationId::from_validated("obs." + tag);
  observation.publisher = publisher;
  observation.worker_boot = boot;
  observation.epoch = CoordinatorEpoch::from_value(0);
  observation.source_generation = SourceGeneration::from_value(1);
  observation.sequence = sequence;
  observation.topology_generation = TopologyGeneration::from_value(1);
  observation.claim = claim;
  return publication;
}

void register_agent(LinkStateEngine& engine, const PublisherId& publisher, const WorkerBootId& boot,
                    const SourceId& source, const std::vector<LinkId>& links) {
  PublisherRegistration registration;
  registration.publisher = publisher;
  registration.worker_boot = boot;
  registration.source = source;
  registration.scope.links = links;
  registration.scope.fabric = FabricId{};
  registration.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
            OutcomeCode::Committed);
}

/// Verifies the invariants that must hold after every mutation.
void check_invariants(const LinkStateEngine& engine, const std::vector<LinkId>& links,
                      GlobalGeneration& last_global) {
  const GlobalGeneration current = engine.global_generation();
  LSF_CHECK(current >= last_global);
  last_global = current;

  std::size_t indexed = 0;
  for (std::size_t index = 0; index < link_operational_state_count; ++index) {
    const auto state = static_cast<LinkOperationalState>(index);
    const std::vector<LinkStateView> views = engine.links_in_state(state);
    indexed += views.size();
    for (const LinkStateView& view : views) {
      LSF_CHECK(view.record.state == state);
      LSF_CHECK(view.link().valid());
    }
  }
  LSF_CHECK_EQ(indexed, engine.link_count());

  for (const LinkId& link : links) {
    const std::optional<LinkStateView> view = engine.link_state(link);
    if (!view.has_value()) {
      continue;
    }
    if (view->record.state == LinkOperationalState::Retired) {
      LSF_CHECK(!view->record.ever_established);
    }
    for (const EvidenceRecord& record : engine.evidence_for_link(link)) {
      LSF_CHECK(record.key.link == link);
      LSF_CHECK(record.status == EvidenceStatus::Current);
      LSF_CHECK(record.publisher.valid());
    }
    LSF_CHECK(view->record.provenance_digest ==
              digest_evidence_set(engine.evidence_for_link(link)));
  }
  const Digest first = engine.state_digest();
  LSF_CHECK(first == engine.state_digest());
}

}  // namespace

LSF_TEST(concurrent_publication_on_disjoint_links_commits_everything) {
  LinkStateEngine engine(concurrent_config());
  constexpr std::size_t kLinks = 256;
  constexpr std::size_t kThreads = 8;
  std::vector<LinkId> links;
  for (std::size_t index = 0; index < kLinks; ++index) {
    links.push_back(LinkId::from_validated("link.concurrent." + std::to_string(index)));
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("concurrent." + std::to_string(index))})
                    .outcome.code == OutcomeCode::Committed);
  }
  for (std::size_t index = 0; index < kThreads; ++index) {
    register_agent(engine, lsf_test::publisher_id("agent-" + std::to_string(index)),
                   lsf_test::boot_id("boot-" + std::to_string(index)),
                   lsf_test::source_id("source-" + std::to_string(index)), links);
  }

  std::barrier start(static_cast<std::ptrdiff_t>(kThreads));
  std::atomic<std::size_t> committed{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (std::size_t index = 0; index < kThreads; ++index) {
    threads.emplace_back([&engine, &links, &start, &committed, index, kThreads] {
      const PublisherId publisher = lsf_test::publisher_id("agent-" + std::to_string(index));
      const WorkerBootId boot = lsf_test::boot_id("boot-" + std::to_string(index));
      const SourceId source = lsf_test::source_id("source-" + std::to_string(index));
      start.arrive_and_wait();
      for (std::size_t link_index = index; link_index < links.size(); link_index += kThreads) {
        const PublishEvidenceRequest request =
            direct_request(publisher, boot, source, links[link_index], EvidenceClaim::Up, 1,
                           "concurrent." + std::to_string(link_index));
        if (engine.publish_evidence(request).outcome.code == OutcomeCode::Committed) {
          committed.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  LSF_CHECK_EQ(committed.load(), kLinks);
  LSF_CHECK(engine.evidence_count() == kLinks);
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).size() == kLinks);
}

LSF_TEST(racing_up_and_down_with_the_same_expected_generation_has_one_winner) {
  for (int attempt = 0; attempt < 32; ++attempt) {
    LinkStateEngine engine(concurrent_config());
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("race")}).outcome.code ==
                OutcomeCode::Committed);
    const LinkId link = link_id("race");
    register_agent(engine, lsf_test::publisher_id("up"), lsf_test::boot_id("boot-up"),
                   lsf_test::source_id("source-up"), {link});
    register_agent(engine, lsf_test::publisher_id("down"), lsf_test::boot_id("boot-down"),
                   lsf_test::source_id("source-down"), {link});

    PublishEvidenceRequest up =
        direct_request(lsf_test::publisher_id("up"), lsf_test::boot_id("boot-up"),
                       lsf_test::source_id("source-up"), link, EvidenceClaim::Up, 1, "race.up");
    PublishEvidenceRequest down =
        direct_request(lsf_test::publisher_id("down"), lsf_test::boot_id("boot-down"),
                       lsf_test::source_id("source-down"), link, EvidenceClaim::Down, 1,
                       "race.down");
    up.expected_evidence_generation = EvidenceGeneration::from_value(0);
    up.expected_state_generation = LinkStateGeneration::from_value(0);
    down.expected_evidence_generation = EvidenceGeneration::from_value(0);
    down.expected_state_generation = LinkStateGeneration::from_value(0);

    std::barrier start(2);
    std::atomic<int> committed{0};
    std::atomic<int> stale{0};
    std::vector<OutcomeCode> codes(2);
    std::thread first([&] {
      start.arrive_and_wait();
      codes[0] = engine.publish_evidence(up).outcome.code;
    });
    std::thread second([&] {
      start.arrive_and_wait();
      codes[1] = engine.publish_evidence(down).outcome.code;
    });
    first.join();
    second.join();
    for (const OutcomeCode code : codes) {
      if (code == OutcomeCode::Committed) {
        ++committed;
      } else if (code == OutcomeCode::StaleGeneration) {
        ++stale;
      }
    }
    LSF_CHECK_EQ(committed.load(), 1);
    LSF_CHECK_EQ(stale.load(), 1);
    LSF_CHECK(engine.evidence_count() == 1);
    const LinkOperationalState state = engine.link_state(link)->state();
    LSF_CHECK(state == LinkOperationalState::Up || state == LinkOperationalState::Down);
  }
}

LSF_TEST(fencing_races_against_publication) {
  std::size_t fenced_publications = 0;
  for (int attempt = 0; attempt < 32; ++attempt) {
    LinkStateEngine engine(concurrent_config());
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("fence-race")}).outcome.code ==
                OutcomeCode::Committed);
    const LinkId link = link_id("fence-race");
    register_agent(engine, lsf_test::publisher_id("victim"), lsf_test::boot_id("boot-victim"),
                   lsf_test::source_id("source-victim"), {link});

    std::barrier start(2);
    OutcomeCode publish_code = OutcomeCode::MalformedRequest;
    std::thread publisher([&] {
      const PublishEvidenceRequest request =
          direct_request(lsf_test::publisher_id("victim"), lsf_test::boot_id("boot-victim"),
                         lsf_test::source_id("source-victim"), link, EvidenceClaim::Up, 1,
                         "fence.race");
      start.arrive_and_wait();
      publish_code = engine.publish_evidence(request).outcome.code;
    });
    std::thread fencer([&] {
      InvalidatePublisherRequest invalidation;
      invalidation.publisher = lsf_test::publisher_id("victim");
      invalidation.worker_boot = lsf_test::boot_id("boot-victim");
      invalidation.epoch = CoordinatorEpoch::from_value(0);
      invalidation.reason = "race";
      start.arrive_and_wait();
      (void)engine.invalidate_publisher(invalidation);
    });
    publisher.join();
    fencer.join();

    if (publish_code == OutcomeCode::Committed) {
      ++fenced_publications;
      // The evidence was committed first and then fenced, so the link must not
      // present it as current.
      LSF_CHECK(engine.evidence_count() == 0);
      LSF_CHECK(engine.link_state(link)->state() ==
                LinkOperationalState::RevalidationRequired);
    } else {
      LSF_CHECK(publish_code == OutcomeCode::StaleAuthority ||
                publish_code == OutcomeCode::StaleCoordinatorEpoch);
      LSF_CHECK(engine.evidence_count() == 0);
    }
  }
  std::printf("  publications that landed before fencing: %zu of 32\n", fenced_publications);
  std::fflush(stdout);
}

LSF_TEST(epoch_advance_races_against_publication) {
  for (int attempt = 0; attempt < 32; ++attempt) {
    LinkStateEngine engine(concurrent_config());
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("epoch-race")}).outcome.code ==
                OutcomeCode::Committed);
    const LinkId link = link_id("epoch-race");
    register_agent(engine, lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                   lsf_test::source_id("source-agent"), {link});

    std::barrier start(2);
    OutcomeCode publish_code = OutcomeCode::MalformedRequest;
    std::thread publisher([&] {
      const PublishEvidenceRequest request =
          direct_request(lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                         lsf_test::source_id("source-agent"), link, EvidenceClaim::Up, 1,
                         "epoch.race");
      start.arrive_and_wait();
      publish_code = engine.publish_evidence(request).outcome.code;
    });
    std::thread coordinator([&] {
      start.arrive_and_wait();
      AdvanceEpochRequest advance;
      advance.new_epoch = CoordinatorEpoch::from_value(1);
      advance.reason = "race";
      (void)engine.advance_coordinator_epoch(advance);
    });
    publisher.join();
    coordinator.join();

    LSF_CHECK(engine.coordinator_epoch().value() == 1);
    LSF_CHECK(engine.evidence_count() == 0);
    LSF_CHECK(publish_code == OutcomeCode::Committed ||
              publish_code == OutcomeCode::StaleCoordinatorEpoch);
    if (publish_code == OutcomeCode::Committed) {
      LSF_CHECK(engine.link_state(link)->state() ==
                LinkOperationalState::RevalidationRequired);
    }
  }
}

LSF_TEST(snapshot_is_consistent_while_mutations_run) {
  LinkStateEngine engine(concurrent_config());
  std::vector<LinkId> links;
  for (int index = 0; index < 64; ++index) {
    links.push_back(link_id("snap-race." + std::to_string(index)));
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("snap-race." + std::to_string(index))})
                    .outcome.code == OutcomeCode::Committed);
  }
  register_agent(engine, lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                 lsf_test::source_id("source-agent"), links);

  std::latch started(1);
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> snapshots{0};
  std::thread reader([&] {
    started.wait();
    while (!stop.load()) {
      const Snapshot snapshot = engine.snapshot(SnapshotRequest{});
      // A snapshot taken under the shared lock must be internally consistent:
      // its digest must match a recomputation from its own records.
      const Digest recomputed = digest_snapshot(snapshot.records, snapshot.global_generation,
                                                snapshot.global_state_generation, snapshot.epoch);
      LSF_CHECK(recomputed == snapshot.digest);
      snapshots.fetch_add(1);
    }
  });
  started.count_down();
  for (std::size_t sequence = 1; sequence <= 8; ++sequence) {
    for (const LinkId& link : links) {
      const PublishEvidenceRequest request =
          direct_request(lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                         lsf_test::source_id("source-agent"), link,
                         sequence % 2 == 0 ? EvidenceClaim::Up : EvidenceClaim::Down, sequence,
                         "snap-race." + std::to_string(sequence) + "." + link.value());
      LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
    }
  }
  stop = true;
  reader.join();
  LSF_CHECK(snapshots.load() > 0);
  LSF_CHECK(engine.evidence_count() == links.size());
}

LSF_TEST(topology_supersession_races_against_publication) {
  for (int attempt = 0; attempt < 16; ++attempt) {
    LinkStateEngine engine(concurrent_config());
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("super-race")}).outcome.code ==
                OutcomeCode::Committed);
    const LinkId link = link_id("super-race");
    register_agent(engine, lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                   lsf_test::source_id("source-agent"), {link});
    LSF_REQUIRE(engine
                    .publish_evidence(direct_request(
                        lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                        lsf_test::source_id("source-agent"), link, EvidenceClaim::Up, 1, "super.1"))
                    .outcome.code == OutcomeCode::Committed);

    std::barrier start(2);
    OutcomeCode publish_code = OutcomeCode::MalformedRequest;
    std::thread publisher([&] {
      const PublishEvidenceRequest request =
          direct_request(lsf_test::publisher_id("agent"), lsf_test::boot_id("boot-agent"),
                         lsf_test::source_id("source-agent"), link, EvidenceClaim::Up, 2,
                         "super.2");
      start.arrive_and_wait();
      publish_code = engine.publish_evidence(request).outcome.code;
    });
    std::thread topology([&] {
      start.arrive_and_wait();
      SupersedeTopologyRequest supersession;
      supersession.link = link;
      supersession.new_topology_generation = TopologyGeneration::from_value(2);
      supersession.replacement = binding_for("super-race", 2, 2);
      supersession.reason = "race";
      (void)engine.supersede_topology(supersession);
    });
    publisher.join();
    topology.join();

    LSF_CHECK(engine.topology_binding(link)->topology_generation.value() == 2);
    LSF_CHECK(publish_code == OutcomeCode::Committed || publish_code == OutcomeCode::StaleTopology);
    const LinkStateView view = engine.link_state(link).value();
    if (view.record.binding.topology_generation.value() == 2 && !view.conflicted) {
      // Any evidence that survived must be bound to the current structure.
      for (const EvidenceRecord& record : engine.evidence_for_link(link)) {
        LSF_CHECK(record.topology_generation == view.record.binding.topology_generation);
      }
      LSF_CHECK(view.record.provenance_digest ==
                digest_evidence_set(engine.evidence_for_link(link)));
    }
  }
}

LSF_TEST(property_based_randomised_operations_hold_the_invariants) {
  constexpr std::uint64_t kSeed = 20260101;
  std::mt19937_64 rng(kSeed);
  for (int round = 0; round < 8; ++round) {
    LinkStateEngine engine(concurrent_config());
    std::vector<LinkId> links;
    for (int index = 0; index < 24; ++index) {
      links.push_back(link_id("prop." + std::to_string(index)));
      LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("prop." + std::to_string(index))})
                      .outcome.code == OutcomeCode::Committed);
    }
    struct AgentState {
      PublisherId publisher;
      WorkerBootId boot;
      SourceId source;
      std::uint64_t boot_counter = 1;
      std::uint64_t sequence = 0;
      bool registered = false;
    };
    std::vector<AgentState> agents(4);
    for (std::size_t index = 0; index < agents.size(); ++index) {
      agents[index].publisher = lsf_test::publisher_id("prop-agent-" + std::to_string(index));
      agents[index].boot = lsf_test::boot_id("prop-boot-" + std::to_string(index) + "-1");
      agents[index].source = lsf_test::source_id("prop-source-" + std::to_string(index));
      register_agent(engine, agents[index].publisher, agents[index].boot, agents[index].source,
                     links);
      agents[index].registered = true;
    }

    GlobalGeneration last_global = engine.global_generation();
    for (int step = 0; step < 400; ++step) {
      const std::uint64_t choice = rng() % 100;
      const std::size_t agent_index = static_cast<std::size_t>(rng() % agents.size());
      AgentState& agent = agents[agent_index];
      const LinkId& link = links[static_cast<std::size_t>(rng() % links.size())];
      if (choice < 55) {
        ++agent.sequence;
        const EvidenceClaim claim =
            static_cast<EvidenceClaim>(rng() % static_cast<std::uint64_t>(evidence_claim_count));
        if (claim == EvidenceClaim::Degraded) {
          continue;
        }
        if (!agent.registered) {
          continue;
        }
        const PublishEvidenceRequest request =
            direct_request(agent.publisher, agent.boot, agent.source, link, claim,
                           agent.sequence,
                           "prop." + std::to_string(round) + "." + std::to_string(step));
        const LinkMutationResult result = engine.publish_evidence(request);
        LSF_CHECK(result.outcome.code == OutcomeCode::Committed ||
                  result.outcome.code == OutcomeCode::EvidenceConflict ||
                  result.outcome.code == OutcomeCode::StaleAuthority ||
                  result.outcome.code == OutcomeCode::StaleCoordinatorEpoch ||
                  result.outcome.code == OutcomeCode::RetiredLink ||
                  result.outcome.code == OutcomeCode::StaleTopology);
      } else if (choice < 70) {
        InvalidatePublisherRequest invalidation;
        invalidation.publisher = agent.publisher;
        invalidation.worker_boot = agent.boot;
        invalidation.epoch = engine.coordinator_epoch();
        invalidation.reason = "property test";
        (void)engine.invalidate_publisher(invalidation);
        agent.registered = false;
      } else if (choice < 80) {
        ++agent.boot_counter;
        agent.boot = lsf_test::boot_id("prop-boot-" + std::to_string(agent_index) + "-" +
                                       std::to_string(agent.boot_counter));
        agent.sequence = 0;
        register_agent(engine, agent.publisher, agent.boot, agent.source, links);
        agent.registered = true;
      } else if (choice < 90) {
        SupersedeTopologyRequest supersession;
        supersession.link = link;
        const TopologyGeneration current = engine.topology_binding(link)->topology_generation;
        supersession.new_topology_generation = TopologyGeneration::from_value(current.value() + 1);
        supersession.replacement = binding_for(link.value().substr(5), current.value() + 1, 1);
        supersession.reason = "property test";
        (void)engine.supersede_topology(supersession);
      } else if (choice < 95) {
        const std::vector<EvidenceRecord> records = engine.evidence_for_link(link);
        if (!records.empty()) {
          const EvidenceRecord& record = records.front();
          WithdrawEvidenceRequest withdrawal;
          withdrawal.key = record.key;
          withdrawal.observation = record.observation;
          withdrawal.publisher = record.publisher;
          withdrawal.worker_boot = record.worker_boot;
          withdrawal.epoch = record.epoch;
          withdrawal.source_generation = record.source_generation;
          withdrawal.sequence = record.sequence + 1u;
          withdrawal.reason = "property test";
          const OutcomeCode code = engine.withdraw_evidence(withdrawal).outcome.code;
          LSF_CHECK(code == OutcomeCode::Committed || code == OutcomeCode::StaleAuthority ||
                    code == OutcomeCode::StaleCoordinatorEpoch);
        }
      } else {
        AdvanceEpochRequest advance;
        advance.new_epoch =
            CoordinatorEpoch::from_value(engine.coordinator_epoch().value() + 1);
        advance.reason = "property test";
        LSF_CHECK(engine.advance_coordinator_epoch(advance).outcome.code ==
                  OutcomeCode::Committed);
        for (AgentState& other : agents) {
          other.registered = false;
        }
      }
      check_invariants(engine, links, last_global);
    }

    const PersistenceConfig config = [] {
      PersistenceConfig persistence;
      persistence.path =
          (std::filesystem::temp_directory_path() / "lsf-property.lsfjournal").string();
      persistence.max_records = 4096;
      return persistence;
    }();
    LSF_CHECK(engine.save(config).code == OutcomeCode::Committed);
    LinkStateEngine recovered(concurrent_config());
    const LoadOutcome loaded = recovered.load(config);
    LSF_CHECK(loaded.outcome.code == OutcomeCode::Committed);
    LSF_CHECK(recovered.link_count() == engine.link_count());
    std::error_code error;
    std::filesystem::remove(config.path, error);
  }
}
