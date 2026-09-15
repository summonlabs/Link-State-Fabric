#include "linkstate/record.hpp"

#include <algorithm>

#include "text.hpp"

namespace linkstate {

std::string StateHistoryEntry::render() const {
  std::string out;
  out += "gen=";
  out += state_generation.to_string();
  out += " ";
  out += linkstate::to_string(from_state);
  out += " -> ";
  out += linkstate::to_string(to_state);
  out += " trigger=";
  out += linkstate::to_string(trigger);
  out += " class=";
  out += linkstate::to_string(result_class);
  if (cause != DegradationCause::None) {
    out += " cause=";
    out += linkstate::to_string(cause);
  }
  out += " evidence_gen=";
  out += evidence_generation.to_string();
  out += " topology_gen=";
  out += topology_generation.to_string();
  if (publisher.valid()) {
    out += " publisher=";
    out += publisher.to_string();
  }
  if (publication.valid()) {
    out += " publication=";
    out += publication.to_string();
  }
  out += " outcome=";
  out += linkstate::to_string(outcome);
  return out;
}

std::string LinkStateRecord::render() const {
  std::string out;
  out += "link                  ";
  out += binding.link.to_string();
  out.push_back('\n');
  out += "record                ";
  out += record.valid() ? record.to_string() : std::string("<unassigned>");
  out.push_back('\n');
  out += "state                 ";
  out += linkstate::to_string(state);
  out.push_back('\n');
  out += "result_class          ";
  out += linkstate::to_string(result_class);
  out.push_back('\n');
  out += "degradation_cause     ";
  out += linkstate::to_string(degradation_cause);
  out.push_back('\n');
  out += "state_generation      ";
  out += state_generation.to_string();
  out.push_back('\n');
  out += "global_generation     ";
  out += global_generation.to_string();
  out.push_back('\n');
  out += "evidence_generation   ";
  out += evidence_generation.to_string();
  out.push_back('\n');
  out += "topology_generation   ";
  out += binding.topology_generation.to_string();
  out.push_back('\n');
  out += "link_class            ";
  out += linkstate::to_string(binding.link_class);
  out.push_back('\n');
  out += "symmetry              ";
  out += linkstate::to_string(binding.symmetry);
  out.push_back('\n');
  out += "endpoint_local        ";
  out += binding.endpoints.local.valid() ? binding.endpoints.local.to_string() : std::string("<none>");
  out.push_back('\n');
  out += "endpoint_remote       ";
  out += binding.endpoints.remote.valid() ? binding.endpoints.remote.to_string() : std::string("<none>");
  out.push_back('\n');
  out += "ever_established      ";
  out += ever_established ? "true" : "false";
  out.push_back('\n');
  out += "provenance_digest     ";
  out += provenance_digest.to_hex();
  out.push_back('\n');
  return out;
}

Digest digest_record(const LinkStateRecord& record) {
  Hasher hasher;
  hasher.update_text(record.binding.link.value());
  hasher.update_text(record.binding.fabric.value());
  hasher.update_text(record.binding.site.value());
  hasher.update_u64(record.binding.topology_generation.value());
  hasher.update_u8(static_cast<std::uint8_t>(record.binding.link_class));
  hasher.update_u8(static_cast<std::uint8_t>(record.binding.symmetry));
  hasher.update_text(record.binding.endpoints.local.value());
  hasher.update_u64(record.binding.endpoints.local_generation.value());
  hasher.update_text(record.binding.endpoints.remote.value());
  hasher.update_u64(record.binding.endpoints.remote_generation.value());
  hasher.update_u32(static_cast<std::uint32_t>(record.binding.member_links.size()));
  for (const LinkId& member : record.binding.member_links) {
    hasher.update_text(member.value());
  }
  hasher.update_u32(static_cast<std::uint32_t>(record.binding.backing_links.size()));
  for (const BackingReference& backing : record.binding.backing_links) {
    hasher.update_text(backing.link.value());
    hasher.update_u64(backing.generation.value());
  }
  hasher.update_u8(static_cast<std::uint8_t>(record.state));
  hasher.update_u8(static_cast<std::uint8_t>(record.result_class));
  hasher.update_u8(static_cast<std::uint8_t>(record.degradation_cause));
  hasher.update_u64(record.state_generation.value());
  hasher.update_u64(record.evidence_generation.value());
  hasher.update_bool(record.admin_intent.has_value());
  if (record.admin_intent.has_value()) {
    hasher.update_u8(static_cast<std::uint8_t>(record.admin_intent->state));
    hasher.update_u8(static_cast<std::uint8_t>(record.admin_intent->cause));
    hasher.update_text(record.admin_intent->publication.value());
    hasher.update_text(record.admin_intent->publisher.value());
    hasher.update_u64(record.admin_intent->generation.value());
  }
  hasher.update(record.provenance_digest.bytes());
  return hasher.finalize();
}

Digest digest_records(const std::vector<LinkStateRecord>& records) {
  std::vector<const LinkStateRecord*> ordered;
  ordered.reserve(records.size());
  for (const LinkStateRecord& record : records) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(),
            [](const LinkStateRecord* lhs, const LinkStateRecord* rhs) {
              return lhs->binding.link < rhs->binding.link;
            });
  Hasher hasher;
  hasher.update_u32(static_cast<std::uint32_t>(ordered.size()));
  for (const LinkStateRecord* record : ordered) {
    hasher.update(digest_record(*record).bytes());
  }
  return hasher.finalize();
}

}  // namespace linkstate
