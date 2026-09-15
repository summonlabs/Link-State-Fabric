#include "journal_builder.hpp"

#include <array>

using namespace linkstate;

namespace lsf_test {
namespace {

void write_evidence(ByteWriter& writer, const CraftedEvidence& evidence) {
  writer.text(evidence.link);
  writer.text(evidence.source);
  writer.text(evidence.endpoint);
  writer.u8(evidence.direction);
  writer.u8(evidence.kind);
  writer.text(evidence.observation);
  writer.text(evidence.publisher);
  writer.text(evidence.worker_boot);
  writer.u64(evidence.epoch);
  writer.u64(evidence.source_generation);
  writer.u64(evidence.sequence);
  writer.u64(evidence.topology_generation);
  writer.u64(evidence.endpoint_generation);
  writer.u8(evidence.evidence_class);
  writer.u8(evidence.claim);
  writer.u8(evidence.degradation_cause);
  writer.u64(evidence.observed_at);
  writer.text(evidence.detail);
  writer.u32(evidence.metadata_length_override);
  writer.u8(evidence.status);
  writer.u64(evidence.evidence_generation);
  writer.u64(evidence.commit_sequence);
  if (evidence.digest_matches_content) {
    EvidenceRecord record;
    record.key.link = LinkId::from_validated(evidence.link);
    record.key.source = SourceId::from_validated(evidence.source);
    if (!evidence.endpoint.empty()) {
      record.key.endpoint = EndpointId::from_validated(evidence.endpoint);
    }
    record.key.direction = static_cast<EvidenceDirection>(evidence.direction);
    record.key.kind = static_cast<EvidenceKind>(evidence.kind);
    record.observation = ObservationId::from_validated(evidence.observation);
    record.publisher = PublisherId::from_validated(evidence.publisher);
    record.worker_boot = WorkerBootId::from_validated(evidence.worker_boot);
    record.epoch = CoordinatorEpoch::from_value(evidence.epoch);
    record.source_generation = SourceGeneration::from_value(evidence.source_generation);
    record.sequence = evidence.sequence;
    record.topology_generation = TopologyGeneration::from_value(evidence.topology_generation);
    record.endpoint_generation = EndpointGeneration::from_value(evidence.endpoint_generation);
    record.evidence_class = static_cast<EvidenceClass>(evidence.evidence_class);
    record.claim = static_cast<EvidenceClaim>(evidence.claim);
    record.degradation_cause = static_cast<DegradationCause>(evidence.degradation_cause);
    record.detail = evidence.detail;
    writer.digest(record.compute_digest());
  } else {
    writer.digest(sha256(std::string("deliberately wrong")));
  }
}

void write_link(ByteWriter& writer, const CraftedLink& link) {
  writer.text(link.record);
  writer.text(link.fabric);
  writer.text(link.site);
  writer.text(link.link);
  writer.u64(link.topology_generation);
  writer.u8(link.link_class);
  writer.u8(link.symmetry);
  writer.text(link.local_endpoint);
  writer.u64(link.local_endpoint_generation);
  writer.text(link.local_device);
  writer.text(link.local_port);
  writer.text(link.remote_endpoint);
  writer.u64(link.remote_endpoint_generation);
  writer.text(link.remote_device);
  writer.text(link.remote_port);
  writer.u32(static_cast<std::uint32_t>(link.member_links.size()));
  for (const std::string& member : link.member_links) {
    writer.text(member);
  }
  writer.u32(static_cast<std::uint32_t>(link.backing_links.size()));
  for (const auto& backing : link.backing_links) {
    writer.text(backing.first);
    writer.u64(backing.second);
  }
  writer.u8(link.state);
  writer.u8(link.result_class);
  writer.u8(link.degradation_cause);
  writer.u64(link.state_generation);
  writer.u64(link.evidence_generation);
  writer.u64(link.global_generation);
  writer.text(link.last_publication);
  writer.text(link.last_transition);
  writer.text(link.last_publisher);
  writer.u8(link.last_outcome);
  writer.u8(link.ever_established ? 1u : 0u);
  writer.u8(link.durable ? 1u : 0u);
  writer.u8(link.has_intent ? 1u : 0u);
  if (link.has_intent) {
    writer.u8(link.intent_state);
    writer.u8(link.intent_cause);
    writer.text(link.intent_publication);
    writer.text(link.intent_publisher);
    writer.u64(link.intent_generation);
  }
  if (link.provenance_matches) {
    writer.digest(sha256(std::string("crafted provenance")));
  } else {
    writer.digest(Digest{});
  }
  writer.u32(0);
  writer.u32(static_cast<std::uint32_t>(link.evidence.size()));
  for (const CraftedEvidence& evidence : link.evidence) {
    write_evidence(writer, evidence);
  }
}

}  // namespace

std::vector<std::byte> build_payload(const CraftedJournal& journal) {
  ByteWriter writer;
  writer.u32(static_cast<std::uint32_t>(journal.links.size()));
  for (const CraftedLink& link : journal.links) {
    write_link(writer, link);
  }
  writer.u32(journal.fence_count_override != 0
                 ? journal.fence_count_override
                 : static_cast<std::uint32_t>(journal.fence_publishers.size()));
  for (const std::string& publisher : journal.fence_publishers) {
    writer.text(publisher);
    writer.text("boot.crafted");
    writer.u64(1);
    writer.text("crafted fence");
  }
  if (journal.trailing_byte) {
    writer.u8(0xff);
  }
  return writer.data;
}

std::vector<std::byte> build_journal(const CraftedJournal& journal,
                                     linkstate::PersistenceConfig config) {
  (void)config;
  const std::vector<std::byte> payload = build_payload(journal);
  ByteWriter header;
  header.u8(static_cast<std::uint8_t>('L'));
  header.u8(static_cast<std::uint8_t>('S'));
  header.u8(static_cast<std::uint8_t>('F'));
  header.u8(static_cast<std::uint8_t>('1'));
  header.u16(journal.version);
  header.u16(journal.flags);
  header.u64(journal.layout);
  header.u64(journal.epoch);
  header.u64(journal.global_generation);
  header.u64(journal.global_state_generation);
  header.u64(journal.topology_generation);
  header.u64(journal.record_count_override != 0 ? journal.record_count_override
                                                : journal.links.size());
  header.u64(payload.size());
  while (header.data.size() < linkstate::journal_header_bytes) {
    header.u8(0);
  }

  Digest payload_digest = sha256(std::span<const std::byte>(payload.data(), payload.size()));
  if (journal.corrupt_payload_digest) {
    payload_digest = sha256(std::string("wrong payload digest"));
  }
  Hasher file_hasher;
  file_hasher.update(std::span<const std::byte>(header.data.data(), header.data.size()));
  file_hasher.update(std::span<const std::byte>(payload.data(), payload.size()));
  Digest file_digest = file_hasher.finalize();
  if (journal.corrupt_file_digest) {
    file_digest = sha256(std::string("wrong file digest"));
  }

  ByteWriter out;
  out.raw(header.data);
  out.raw(payload);
  out.digest(payload_digest);
  out.digest(file_digest);
  return out.data;
}

CraftedJournal valid_journal() {
  CraftedJournal journal;
  CraftedLink link;
  CraftedEvidence evidence;
  link.evidence.push_back(evidence);
  journal.links.push_back(link);
  return journal;
}

}  // namespace lsf_test
