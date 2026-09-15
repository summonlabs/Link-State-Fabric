#pragma once

// Independent writer for the Link State Fabric journal format.
//
// The production encoder lives in the library; this builder re-implements the
// documented layout so that the decoder can be validated against an
// independent producer, including deliberately malformed payloads that the
// production encoder would never emit.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/persistence.hpp"

namespace lsf_test {

class ByteWriter {
 public:
  void u8(std::uint8_t value) { data.push_back(static_cast<std::byte>(value)); }
  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
  }
  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32u; shift += 8u) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffu));
    }
  }
  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64u; shift += 8u) {
      u8(static_cast<std::uint8_t>((value >> shift) & 0xffull));
    }
  }
  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (const char ch : value) {
      data.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
  }
  void digest(const linkstate::Digest& value) {
    for (const std::byte item : value.bytes()) {
      data.push_back(item);
    }
  }
  void raw(const std::vector<std::byte>& value) {
    data.insert(data.end(), value.begin(), value.end());
  }

  std::vector<std::byte> data;
};

/// Description of one evidence record inside a crafted journal.
struct CraftedEvidence {
  std::string link = "link.crafted";
  std::string source = "source.crafted";
  std::string endpoint;
  std::uint8_t direction = 0;
  std::uint8_t kind = 0;
  std::string observation = "obs.crafted";
  std::string publisher = "publisher.crafted";
  std::string worker_boot = "boot.crafted";
  std::uint64_t epoch = 1;
  std::uint64_t source_generation = 1;
  std::uint64_t sequence = 1;
  std::uint64_t topology_generation = 1;
  std::uint64_t endpoint_generation = 0;
  std::uint8_t evidence_class = 0;
  std::uint8_t claim = 1;
  std::uint8_t degradation_cause = 0;
  std::uint64_t observed_at = 0;
  std::string detail;
  std::uint32_t metadata_length_override = 0;
  std::uint8_t status = 0;
  std::uint64_t evidence_generation = 1;
  std::uint64_t commit_sequence = 1;
  bool digest_matches_content = true;
};

/// Description of one link record inside a crafted journal.
struct CraftedLink {
  std::string record = "lsr.link.crafted";
  std::string fabric = "fabric.crafted";
  std::string site = "site.crafted";
  std::string link = "link.crafted";
  std::uint64_t topology_generation = 1;
  std::uint8_t link_class = 1;
  std::uint8_t symmetry = 1;
  std::string local_endpoint = "ep.crafted.a";
  std::uint64_t local_endpoint_generation = 1;
  std::string local_device = "dev.crafted.a";
  std::string local_port = "port.crafted.a";
  std::string remote_endpoint = "ep.crafted.b";
  std::uint64_t remote_endpoint_generation = 1;
  std::string remote_device = "dev.crafted.b";
  std::string remote_port = "port.crafted.b";
  std::vector<std::string> member_links;
  std::vector<std::pair<std::string, std::uint64_t>> backing_links;
  std::uint8_t state = 1;
  std::uint8_t result_class = 1;
  std::uint8_t degradation_cause = 0;
  std::uint64_t state_generation = 1;
  std::uint64_t evidence_generation = 1;
  std::uint64_t global_generation = 1;
  std::string last_publication = "pub.crafted";
  std::string last_transition = "st.link.crafted.1";
  std::string last_publisher = "publisher.crafted";
  std::uint8_t last_outcome = 0;
  bool ever_established = true;
  bool durable = true;
  bool has_intent = false;
  std::uint8_t intent_state = 4;
  std::uint8_t intent_cause = 0;
  std::string intent_publication = "pub.intent";
  std::string intent_publisher = "publisher.crafted";
  std::uint64_t intent_generation = 1;
  bool provenance_matches = false;
  std::vector<CraftedEvidence> evidence;
};

struct CraftedJournal {
  std::uint16_t version = linkstate::journal_format_version;
  std::uint16_t flags = 0;
  std::uint64_t layout = 2;
  std::uint64_t epoch = 1;
  std::uint64_t global_generation = 8;
  std::uint64_t global_state_generation = 4;
  std::uint64_t topology_generation = 1;
  std::uint64_t record_count_override = 0;
  std::vector<CraftedLink> links;
  std::vector<std::string> fence_publishers;
  bool trailing_byte = false;
  bool corrupt_payload_digest = false;
  bool corrupt_file_digest = false;
  std::uint32_t fence_count_override = 0;
};

std::vector<std::byte> build_payload(const CraftedJournal& journal);
std::vector<std::byte> build_journal(const CraftedJournal& journal,
                                     linkstate::PersistenceConfig config = {});

/// A minimal but complete and valid crafted journal with one link.
CraftedJournal valid_journal();

}  // namespace lsf_test
