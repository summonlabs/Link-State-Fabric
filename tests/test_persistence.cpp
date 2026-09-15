#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "journal_builder.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/persistence.hpp"
#include "lsf_test.hpp"

using namespace linkstate;
using lsf_test::CraftedEvidence;
using lsf_test::CraftedJournal;
using lsf_test::CraftedLink;

namespace {

PersistenceConfig scratch_config(const std::string& name) {
  PersistenceConfig config;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("lsf-test-" + name + ".lsfjournal");
  config.path = path.string();
  config.max_records = 4096;
  config.include_history = true;
  return config;
}

void write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

Outcome inspect_bytes(const CraftedJournal& journal, JournalSummary& summary,
                      PersistenceConfig config = {}) {
  const std::vector<std::byte> bytes = lsf_test::build_journal(journal, config);
  JournalPayload payload;
  return decode_journal(std::span<const std::byte>(bytes), config, payload, summary);
}

EngineConfig persistence_engine_config() {
  EngineConfig config;
  config.max_links = 128;
  config.max_publishers = 16;
  config.max_evidence_per_link = 8;
  config.max_history_per_link = 8;
  return config;
}

}  // namespace

LSF_TEST(crafted_journal_round_trips_through_the_decoder) {
  const CraftedJournal journal = lsf_test::valid_journal();
  JournalSummary summary;
  const Outcome outcome = inspect_bytes(journal, summary);
  if (outcome.code != OutcomeCode::Committed) {
    std::printf("  decode detail: %s\n", outcome.detail.c_str());
    std::fflush(stdout);
  }
  LSF_REQUIRE(outcome.code == OutcomeCode::Committed);
  LSF_CHECK(summary.record_count == 1);
  LSF_CHECK(summary.evidence_count == 1);
  LSF_CHECK(summary.format_version == journal_format_version);
  LSF_CHECK(summary.epoch.value() == 1);
  LSF_CHECK(!summary.file_digest.is_zero());
  LSF_CHECK(summary.render().find("format_version") != std::string::npos);
}

LSF_TEST(journal_corruption_matrix_rejects_safely) {
  struct Case {
    const char* name;
    OutcomeCode expected;
    CraftedJournal journal;
  };
  std::vector<Case> cases;

  CraftedJournal bad_version = lsf_test::valid_journal();
  bad_version.version = 99;
  cases.push_back({"unsupported version", OutcomeCode::UnsupportedState, bad_version});

  CraftedJournal bad_payload_digest = lsf_test::valid_journal();
  bad_payload_digest.corrupt_payload_digest = true;
  cases.push_back({"payload digest", OutcomeCode::PersistenceCorrupt, bad_payload_digest});

  CraftedJournal bad_file_digest = lsf_test::valid_journal();
  bad_file_digest.corrupt_file_digest = true;
  cases.push_back({"file digest", OutcomeCode::PersistenceCorrupt, bad_file_digest});

  CraftedJournal trailing = lsf_test::valid_journal();
  trailing.trailing_byte = true;
  cases.push_back({"trailing bytes", OutcomeCode::PersistenceCorrupt, trailing});

  CraftedJournal oversized_records = lsf_test::valid_journal();
  oversized_records.record_count_override = 10'000'000;
  cases.push_back({"record count bound", OutcomeCode::ResourceLimit, oversized_records});

  CraftedJournal oversized_fences = lsf_test::valid_journal();
  oversized_fences.fence_count_override = 4'000'000'000u;
  cases.push_back({"fence count bound", OutcomeCode::ResourceLimit, oversized_fences});

  CraftedJournal malformed_identifier = lsf_test::valid_journal();
  malformed_identifier.links.front().local_endpoint = "bad endpoint id";
  cases.push_back({"malformed identifier", OutcomeCode::PersistenceCorrupt, malformed_identifier});

  CraftedJournal invalid_enum = lsf_test::valid_journal();
  invalid_enum.links.front().state = 200;
  cases.push_back({"invalid state enum", OutcomeCode::PersistenceCorrupt, invalid_enum});

  CraftedJournal invalid_claim = lsf_test::valid_journal();
  invalid_claim.links.front().evidence.front().claim = 60;
  cases.push_back({"invalid claim enum", OutcomeCode::PersistenceCorrupt, invalid_claim});

  CraftedJournal duplicate_link = lsf_test::valid_journal();
  duplicate_link.links.push_back(duplicate_link.links.front());
  cases.push_back({"duplicate link", OutcomeCode::PersistenceCorrupt, duplicate_link});

  CraftedJournal duplicate_evidence = lsf_test::valid_journal();
  duplicate_evidence.links.front().evidence.push_back(
      duplicate_evidence.links.front().evidence.front());
  cases.push_back({"duplicate evidence", OutcomeCode::PersistenceCorrupt, duplicate_evidence});

  CraftedJournal zero_topology = lsf_test::valid_journal();
  zero_topology.links.front().topology_generation = 0;
  cases.push_back({"zero topology generation", OutcomeCode::PersistenceCorrupt, zero_topology});

  CraftedJournal impossible_generation = lsf_test::valid_journal();
  impossible_generation.links.front().state_generation = 99;
  cases.push_back({"impossible state generation", OutcomeCode::PersistenceCorrupt,
                   impossible_generation});

  CraftedJournal impossible_global = lsf_test::valid_journal();
  impossible_global.links.front().global_generation = 999;
  cases.push_back({"impossible global generation", OutcomeCode::PersistenceCorrupt,
                   impossible_global});

  CraftedJournal evidence_digest = lsf_test::valid_journal();
  evidence_digest.links.front().evidence.front().digest_matches_content = false;
  cases.push_back({"evidence digest", OutcomeCode::PersistenceCorrupt, evidence_digest});

  CraftedJournal oversized_metadata = lsf_test::valid_journal();
  oversized_metadata.links.front().evidence.front().metadata_length_override = 0xFFFFFFFFu;
  cases.push_back({"oversized metadata", OutcomeCode::ResourceLimit, oversized_metadata});

  CraftedJournal mismatched_link = lsf_test::valid_journal();
  mismatched_link.links.front().evidence.front().link = "link.other";
  cases.push_back({"evidence of another link", OutcomeCode::PersistenceCorrupt, mismatched_link});

  PersistenceConfig config;
  config.max_records = 4096;

  {
    std::vector<std::byte> mutated = lsf_test::build_journal(lsf_test::valid_journal(), config);
    mutated[0] = static_cast<std::byte>('X');
    JournalPayload payload;
    JournalSummary summary;
    LSF_CHECK(decode_journal(std::span<const std::byte>(mutated), config, payload, summary).code ==
              OutcomeCode::PersistenceCorrupt);
  }
  {
    std::vector<std::byte> mutated = lsf_test::build_journal(lsf_test::valid_journal(), config);
    mutated[10] = static_cast<std::byte>(0x7f);
    JournalPayload payload;
    JournalSummary summary;
    LSF_CHECK(decode_journal(std::span<const std::byte>(mutated), config, payload, summary).code ==
              OutcomeCode::PersistenceCorrupt);
  }
  {
    std::vector<std::byte> mutated = lsf_test::build_journal(lsf_test::valid_journal(), config);
    mutated.resize(mutated.size() - 3);
    JournalPayload payload;
    JournalSummary summary;
    LSF_CHECK(decode_journal(std::span<const std::byte>(mutated), config, payload, summary).code ==
              OutcomeCode::PersistenceCorrupt);
  }

  for (Case& item : cases) {
    JournalSummary summary;
    const std::vector<std::byte> bytes = lsf_test::build_journal(item.journal, config);
    JournalPayload payload;
    const Outcome outcome = decode_journal(std::span<const std::byte>(bytes), config, payload,
                                           summary);
    if (outcome.code != item.expected) {
      std::printf("  case %s produced %s\n", item.name,
                  std::string(linkstate::to_string(outcome.code)).c_str());
    }
    LSF_CHECK(outcome.code == item.expected);
  }
}

LSF_TEST(journal_truncation_is_rejected_at_every_length) {
  const std::vector<std::byte> bytes = lsf_test::build_journal(lsf_test::valid_journal());
  PersistenceConfig config;
  config.max_records = 4096;
  for (std::size_t length = 0; length < bytes.size(); ++length) {
    JournalPayload payload;
    JournalSummary summary;
    const Outcome outcome =
        decode_journal(std::span<const std::byte>(bytes.data(), length), config, payload, summary);
    LSF_CHECK(outcome.code == OutcomeCode::PersistenceCorrupt);
  }
}

LSF_TEST(engine_journal_round_trips_and_recovers_conservatively) {
  const PersistenceConfig config = scratch_config("recovery");
  LinkStateEngine engine(persistence_engine_config());
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("a")}).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("b")}).outcome.code ==
              OutcomeCode::Committed);
  lsf_test::TestPublisher publisher(engine, "agent",
                                    {lsf_test::link_id("a"), lsf_test::link_id("b")},
                                    "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.publish_evidence(publisher.request("b", EvidenceClaim::Down)).outcome.code ==
              OutcomeCode::Committed);
  const Digest digest_before = engine.state_digest();
  const Outcome saved = engine.save(config);
  LSF_REQUIRE(saved.code == OutcomeCode::Committed);
  LSF_CHECK(saved.detail.find("records and") != std::string::npos);

  JournalSummary summary;
  LSF_CHECK(inspect_journal(config, summary).code == OutcomeCode::Committed);
  LSF_CHECK(summary.record_count == 2);
  LSF_CHECK(summary.evidence_count == 2);

  LinkStateEngine recovered(persistence_engine_config());
  const LoadOutcome loaded = recovered.load(config);
  LSF_REQUIRE(loaded.outcome.code == OutcomeCode::Committed);
  LSF_CHECK_EQ(loaded.records_loaded, std::size_t{2});
  LSF_CHECK_EQ(loaded.records_revalidated, std::size_t{2});
  LSF_CHECK(loaded.layout.value() == 2);
  LSF_CHECK(recovered.link_count() == 2);
  LSF_CHECK(recovered.evidence_count() == 0);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("a"))->state() ==
            LinkOperationalState::RevalidationRequired);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("b"))->state() ==
            LinkOperationalState::RevalidationRequired);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("a"))->record.ever_established);
  LSF_CHECK(recovered.coordinator_epoch() == engine.coordinator_epoch());
  LSF_CHECK(recovered.coordinator_epoch().value() == 0);
  LSF_CHECK(recovered.global_generation().value() == engine.global_generation().value());
  LSF_CHECK(recovered.global_state_generation().value() ==
            engine.global_state_generation().value());
  LSF_CHECK(recovered.links_requiring_revalidation().size() == 2);
  LSF_CHECK(recovered.explain(lsf_test::link_id("a")).render().find("RECOVERY") !=
            std::string::npos);
  LSF_CHECK(!(recovered.state_digest() == digest_before));

  // Fresh authority must republish before the link is authoritative again.
  lsf_test::TestPublisher fresh(recovered, "agent",
                                {lsf_test::link_id("a"), lsf_test::link_id("b")}, "port-agent",
                                "boot-2");
  LSF_CHECK(recovered.publish_evidence(fresh.request("a", EvidenceClaim::Up)).outcome.code ==
            OutcomeCode::Committed);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("a"))->state() == LinkOperationalState::Up);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("b"))->state() ==
            LinkOperationalState::RevalidationRequired);

  std::error_code error;
  std::filesystem::remove(config.path, error);
}

LSF_TEST(unknown_and_retired_records_recover_without_fabricated_truth) {
  const PersistenceConfig config = scratch_config("unknown-retired");
  LinkStateEngine engine(persistence_engine_config());
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("x")}).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("y")}).outcome.code ==
              OutcomeCode::Committed);
  lsf_test::TestPublisher publisher(engine, "agent",
                                    {lsf_test::link_id("y")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("y", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  RetireLinkRequest retirement;
  retirement.link = lsf_test::link_id("y");
  retirement.topology_generation = TopologyGeneration::from_value(1);
  retirement.publication = PublicationId::from_validated("pub.retire");
  retirement.publisher = publisher.publisher();
  retirement.worker_boot = publisher.boot();
  retirement.epoch = engine.coordinator_epoch();
  LSF_REQUIRE(engine.retire_link(retirement).outcome.code == OutcomeCode::Committed);
  LSF_REQUIRE(engine.save(config).code == OutcomeCode::Committed);

  LinkStateEngine recovered(persistence_engine_config());
  const LoadOutcome loaded = recovered.load(config);
  LSF_REQUIRE(loaded.outcome.code == OutcomeCode::Committed);
  LSF_CHECK_EQ(loaded.records_left_unknown, std::size_t{1});
  LSF_CHECK(recovered.link_state(lsf_test::link_id("x"))->state() ==
            LinkOperationalState::Unknown);
  LSF_CHECK(recovered.link_state(lsf_test::link_id("y"))->state() == LinkOperationalState::Retired);
  LSF_CHECK(recovered.publish_evidence(publisher.request("y", EvidenceClaim::Up)).outcome.code ==
            OutcomeCode::RetiredLink);

  std::error_code error;
  std::filesystem::remove(config.path, error);
}

LSF_TEST(corrupt_journal_leaves_the_engine_untouched) {
  const PersistenceConfig config = scratch_config("untouched");
  LinkStateEngine engine(persistence_engine_config());
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for("a")}).outcome.code ==
              OutcomeCode::Committed);
  lsf_test::TestPublisher publisher(engine, "agent", {lsf_test::link_id("a")}, "port-agent",
                                    "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.save(config).code == OutcomeCode::Committed);

  std::vector<std::byte> bytes;
  JournalSummary read_summary;
  LSF_REQUIRE(read_journal_file(config, bytes, read_summary).code == OutcomeCode::Committed);
  LSF_REQUIRE(bytes.size() > 80);
  bytes[bytes.size() / 2] = static_cast<std::byte>(
      std::to_integer<unsigned>(bytes[bytes.size() / 2]) ^ 0x5au);
  write_bytes(config.path, bytes);

  const Digest digest_before = engine.state_digest();
  const LoadOutcome loaded = engine.load(config);
  LSF_CHECK(loaded.outcome.code == OutcomeCode::PersistenceCorrupt);
  LSF_CHECK(engine.state_digest() == digest_before);
  LSF_CHECK(engine.link_state(lsf_test::link_id("a"))->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.evidence_count() == 1);

  std::error_code error;
  std::filesystem::remove(config.path, error);
}

LSF_TEST(persistence_paths_are_validated) {
  PersistenceConfig config;
  config.path = "..";
  LSF_CHECK(!config.validate().empty());

  PersistenceConfig traversal;
  traversal.path = "../escape.lsfjournal";
  LSF_CHECK(!traversal.validate().empty());
  std::vector<std::byte> bytes;
  JournalSummary summary;
  LSF_CHECK(read_journal_file(traversal, bytes, summary).code == OutcomeCode::MalformedRequest);

  PersistenceConfig empty;
  empty.path.clear();
  LSF_CHECK(!empty.validate().empty());

  PersistenceConfig control;
  control.path = std::string("bad\nname.lsfjournal");
  LSF_CHECK(!control.validate().empty());

  PersistenceConfig zero_records;
  zero_records.max_records = 0;
  LSF_CHECK(!zero_records.validate().empty());

  PersistenceConfig missing_directory;
  missing_directory.path = (std::filesystem::temp_directory_path() / "lsf-missing-dir-xyz" /
                            "journal.lsfjournal")
                               .string();
  const std::vector<std::byte> payload;
  LSF_CHECK(write_journal_file(missing_directory, std::span<const std::byte>()).code ==
            OutcomeCode::PersistenceIoFailure);

  PersistenceConfig readable;
  readable.path = (std::filesystem::temp_directory_path() / "lsf-does-not-exist.lsfjournal").string();
  std::error_code error;
  std::filesystem::remove(readable.path, error);
  LSF_CHECK(read_journal_file(readable, bytes, summary).code ==
            OutcomeCode::PersistenceIoFailure);
}

LSF_TEST(engine_configuration_is_validated) {
  EngineConfig config;
  LSF_CHECK(config.validate().empty());
  config.max_links = 0;
  LSF_CHECK(!config.validate().empty());
  EngineConfig too_many;
  too_many.max_links = limits::max_links + 1;
  LSF_CHECK(!too_many.validate().empty());
  EngineConfig bad_history;
  bad_history.max_history_per_link = limits::max_history_per_link + 1;
  LSF_CHECK(!bad_history.validate().empty());
  EngineConfig bad_batch;
  bad_batch.max_batch_size = 0;
  LSF_CHECK(!bad_batch.validate().empty());
  EngineConfig bad_publishers;
  bad_publishers.max_publishers = limits::max_publishers + 1;
  LSF_CHECK(!bad_publishers.validate().empty());
}
