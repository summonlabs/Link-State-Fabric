#include "linkstate/snapshot.hpp"

#include <algorithm>

namespace linkstate {

Digest digest_snapshot(std::vector<SnapshotRecord> records, GlobalGeneration global_generation,
                       LinkStateGeneration global_state_generation, CoordinatorEpoch epoch) {
  std::sort(records.begin(), records.end());
  Hasher hasher;
  hasher.update_u64(global_generation.value());
  hasher.update_u64(global_state_generation.value());
  hasher.update_u64(epoch.value());
  hasher.update_u32(static_cast<std::uint32_t>(records.size()));
  for (const SnapshotRecord& record : records) {
    hasher.update_text(record.link.value());
    hasher.update_u8(static_cast<std::uint8_t>(record.state));
    hasher.update_u8(static_cast<std::uint8_t>(record.result_class));
    hasher.update_u64(record.state_generation.value());
    hasher.update_u64(record.evidence_generation.value());
    hasher.update_u64(record.topology_generation.value());
    hasher.update_u32(static_cast<std::uint32_t>(record.current_evidence));
    hasher.update_bool(record.conflicted);
    hasher.update(record.record_digest.bytes());
  }
  return hasher.finalize();
}

std::string Snapshot::render() const {
  std::string out;
  out += "snapshot ";
  out += id.to_string();
  out.push_back('\n');
  out += "label                 ";
  out += label;
  out.push_back('\n');
  out += "global_generation     ";
  out += global_generation.to_string();
  out.push_back('\n');
  out += "global_state_gen      ";
  out += global_state_generation.to_string();
  out.push_back('\n');
  out += "coordinator_epoch     ";
  out += epoch.to_string();
  out.push_back('\n');
  out += "topology_generation   ";
  out += topology_generation.to_string();
  out.push_back('\n');
  out += "sequence              ";
  out += std::to_string(sequence);
  out.push_back('\n');
  out += "records               ";
  out += std::to_string(records.size());
  out.push_back('\n');
  out += "digest                ";
  out += digest.to_hex();
  out.push_back('\n');
  for (const SnapshotRecord& record : records) {
    out += "  ";
    out += record.link.value();
    out += " state=";
    out += linkstate::to_string(record.state);
    out += " state_gen=";
    out += record.state_generation.to_string();
    out += " evidence_gen=";
    out += record.evidence_generation.to_string();
    out += " topology_gen=";
    out += record.topology_generation.to_string();
    out += " evidence=";
    out += std::to_string(record.current_evidence);
    if (record.conflicted) {
      out += " CONFLICTED";
    }
    out.push_back('\n');
  }
  return out;
}

}  // namespace linkstate
