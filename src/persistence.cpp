#include "linkstate/persistence.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

#include "text.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace linkstate {
namespace {

constexpr std::size_t kMaxJournalBytes = 1u << 31;  // 2 GiB hard cap

void append_u8(std::vector<std::byte>& out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void append_u16(std::vector<std::byte>& out, std::uint16_t value) {
  append_u8(out, static_cast<std::uint8_t>(value & 0xffu));
  append_u8(out, static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void append_u64(std::vector<std::byte>& out, std::uint64_t value) {
  for (unsigned shift = 0; shift < 64u; shift += 8u) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffull));
  }
}

void append_bytes(std::vector<std::byte>& out, std::span<const std::byte> bytes) {
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_text(std::vector<std::byte>& out, std::string_view text) {
  append_u32(out, static_cast<std::uint32_t>(text.size()));
  const auto* data = reinterpret_cast<const std::byte*>(text.data());
  append_bytes(out, std::span<const std::byte>(data, text.size()));
}

void append_digest(std::vector<std::byte>& out, const Digest& digest) {
  append_bytes(out, digest.bytes());
}

void encode_evidence(std::vector<std::byte>& out, const EvidenceRecord& record) {
  append_text(out, record.key.link.value());
  append_text(out, record.key.source.value());
  append_text(out, record.key.endpoint.value());
  append_u8(out, static_cast<std::uint8_t>(record.key.direction));
  append_u8(out, static_cast<std::uint8_t>(record.key.kind));
  append_text(out, record.observation.value());
  append_text(out, record.publisher.value());
  append_text(out, record.worker_boot.value());
  append_u64(out, record.epoch.value());
  append_u64(out, record.source_generation.value());
  append_u64(out, record.sequence);
  append_u64(out, record.topology_generation.value());
  append_u64(out, record.endpoint_generation.value());
  append_u8(out, static_cast<std::uint8_t>(record.evidence_class));
  append_u8(out, static_cast<std::uint8_t>(record.claim));
  append_u8(out, static_cast<std::uint8_t>(record.degradation_cause));
  append_u64(out, record.observed_at_unix_nanos);
  append_text(out, record.detail);
  append_u32(out, static_cast<std::uint32_t>(record.metadata.size()));
  append_bytes(out, std::span<const std::byte>(record.metadata.data(), record.metadata.size()));
  append_u8(out, static_cast<std::uint8_t>(record.status));
  append_u64(out, record.evidence_generation.value());
  append_u64(out, record.commit_sequence.value());
  append_digest(out, record.record_digest);
}

void encode_link(std::vector<std::byte>& out, const PersistedLink& link, bool include_history) {
  const LinkStateRecord& record = link.record;
  append_text(out, record.record.value());
  append_text(out, record.binding.fabric.value());
  append_text(out, record.binding.site.value());
  append_text(out, record.binding.link.value());
  append_u64(out, record.binding.topology_generation.value());
  append_u8(out, static_cast<std::uint8_t>(record.binding.link_class));
  append_u8(out, static_cast<std::uint8_t>(record.binding.symmetry));

  const EndpointBinding& endpoints = record.binding.endpoints;
  append_text(out, endpoints.local.value());
  append_u64(out, endpoints.local_generation.value());
  append_text(out, endpoints.local_device.value());
  append_text(out, endpoints.local_port.value());
  append_text(out, endpoints.remote.value());
  append_u64(out, endpoints.remote_generation.value());
  append_text(out, endpoints.remote_device.value());
  append_text(out, endpoints.remote_port.value());

  append_u32(out, static_cast<std::uint32_t>(record.binding.member_links.size()));
  for (const LinkId& member : record.binding.member_links) {
    append_text(out, member.value());
  }
  append_u32(out, static_cast<std::uint32_t>(record.binding.backing_links.size()));
  for (const BackingReference& backing : record.binding.backing_links) {
    append_text(out, backing.link.value());
    append_u64(out, backing.generation.value());
  }

  append_u8(out, static_cast<std::uint8_t>(record.state));
  append_u8(out, static_cast<std::uint8_t>(record.result_class));
  append_u8(out, static_cast<std::uint8_t>(record.degradation_cause));
  append_u64(out, record.state_generation.value());
  append_u64(out, record.evidence_generation.value());
  append_u64(out, record.global_generation.value());
  append_text(out, record.last_publication.value());
  append_text(out, record.last_transition.value());
  append_text(out, record.last_publisher.value());
  append_u8(out, static_cast<std::uint8_t>(record.last_outcome));
  append_u8(out, record.ever_established ? 1u : 0u);
  append_u8(out, record.durable ? 1u : 0u);
  append_u8(out, record.admin_intent.has_value() ? 1u : 0u);
  if (record.admin_intent.has_value()) {
    append_u8(out, static_cast<std::uint8_t>(record.admin_intent->state));
    append_u8(out, static_cast<std::uint8_t>(record.admin_intent->cause));
    append_text(out, record.admin_intent->publication.value());
    append_text(out, record.admin_intent->publisher.value());
    append_u64(out, record.admin_intent->generation.value());
  }
  append_digest(out, record.provenance_digest);

  if (include_history) {
    append_u32(out, static_cast<std::uint32_t>(record.history.size()));
    for (const StateHistoryEntry& history : record.history) {
      append_u64(out, history.state_generation.value());
      append_u8(out, static_cast<std::uint8_t>(history.from_state));
      append_u8(out, static_cast<std::uint8_t>(history.to_state));
      append_u8(out, static_cast<std::uint8_t>(history.trigger));
      append_u8(out, static_cast<std::uint8_t>(history.result_class));
      append_u8(out, static_cast<std::uint8_t>(history.cause));
      append_text(out, history.publication.value());
      append_text(out, history.publisher.value());
      append_u64(out, history.evidence_generation.value());
      append_u64(out, history.topology_generation.value());
      append_u8(out, static_cast<std::uint8_t>(history.outcome));
    }
  } else {
    append_u32(out, 0);
  }

  append_u32(out, static_cast<std::uint32_t>(link.evidence.size()));
  for (const EvidenceRecord& evidence : link.evidence) {
    encode_evidence(out, evidence);
  }
}

void encode_payload(std::vector<std::byte>& out, const JournalPayload& payload,
                    const PersistenceConfig& config) {
  append_u32(out, static_cast<std::uint32_t>(payload.links.size()));
  for (const PersistedLink& link : payload.links) {
    encode_link(out, link, config.include_history);
  }
  append_u32(out, static_cast<std::uint32_t>(payload.fences.size()));
  for (const PublisherFence& fence : payload.fences) {
    append_text(out, fence.publisher.value());
    append_text(out, fence.worker_boot.value());
    append_u64(out, fence.epoch.value());
    append_text(out, fence.reason);
  }
}

std::size_t max_journal_bytes(const PersistenceConfig& config) {
  std::size_t bound = journal_header_bytes + journal_trailer_bytes;
  const std::size_t per_record = 16384;
  if (config.max_records > (kMaxJournalBytes - bound) / per_record) {
    return kMaxJournalBytes;
  }
  bound += config.max_records * per_record;
  return (std::min)(bound, kMaxJournalBytes);
}

}  // namespace

std::string JournalSummary::render() const {
  std::string out;
  out += "format_version        ";
  out += std::to_string(format_version);
  out.push_back('\n');
  out += "flags                 ";
  out += detail::format_u64(flags);
  out.push_back('\n');
  out += "layout_generation     ";
  out += layout.to_string();
  out.push_back('\n');
  out += "coordinator_epoch     ";
  out += epoch.to_string();
  out.push_back('\n');
  out += "global_generation     ";
  out += global_generation.to_string();
  out.push_back('\n');
  out += "global_state_gen      ";
  out += global_state_generation.to_string();
  out.push_back('\n');
  out += "topology_generation   ";
  out += topology_generation.to_string();
  out.push_back('\n');
  out += "records               ";
  out += std::to_string(record_count);
  out.push_back('\n');
  out += "evidence              ";
  out += std::to_string(evidence_count);
  out.push_back('\n');
  out += "fences                ";
  out += std::to_string(fence_count);
  out.push_back('\n');
  out += "file_size             ";
  out += detail::format_u64(file_size);
  out.push_back('\n');
  out += "payload_digest        ";
  out += payload_digest.to_hex();
  out.push_back('\n');
  out += "file_digest           ";
  out += file_digest.to_hex();
  out.push_back('\n');
  return out;
}

std::vector<std::byte> encode_journal(const JournalPayload& payload,
                                      const PersistenceConfig& config,
                                      JournalSummary& summary) {
  std::vector<std::byte> body;
  encode_payload(body, payload, config);

  std::vector<std::byte> header;
  header.reserve(journal_header_bytes);
  append_u8(header, static_cast<std::uint8_t>('L'));
  append_u8(header, static_cast<std::uint8_t>('S'));
  append_u8(header, static_cast<std::uint8_t>('F'));
  append_u8(header, static_cast<std::uint8_t>('1'));
  append_u16(header, journal_format_version);
  append_u16(header, static_cast<std::uint16_t>(config.include_history ? 1u : 0u));
  append_u64(header, payload.layout.value());
  append_u64(header, payload.epoch.value());
  append_u64(header, payload.global_generation.value());
  append_u64(header, payload.global_state_generation.value());
  append_u64(header, payload.topology_generation.value());
  append_u64(header, static_cast<std::uint64_t>(payload.links.size()));
  append_u64(header, static_cast<std::uint64_t>(body.size()));
  header.resize(journal_header_bytes, std::byte{0});

  const Digest payload_digest = sha256(std::span<const std::byte>(body.data(), body.size()));
  Hasher file_hasher;
  file_hasher.update(std::span<const std::byte>(header.data(), header.size()));
  file_hasher.update(std::span<const std::byte>(body.data(), body.size()));
  const Digest file_digest = file_hasher.finalize();

  std::vector<std::byte> out;
  out.reserve(header.size() + body.size() + journal_trailer_bytes);
  append_bytes(out, std::span<const std::byte>(header.data(), header.size()));
  append_bytes(out, std::span<const std::byte>(body.data(), body.size()));
  append_digest(out, payload_digest);
  append_digest(out, file_digest);

  summary.format_version = journal_format_version;
  summary.flags = config.include_history ? 1u : 0u;
  summary.layout = payload.layout;
  summary.epoch = payload.epoch;
  summary.global_generation = payload.global_generation;
  summary.global_state_generation = payload.global_state_generation;
  summary.topology_generation = payload.topology_generation;
  summary.record_count = payload.links.size();
  summary.evidence_count = 0;
  for (const PersistedLink& link : payload.links) {
    summary.evidence_count += link.evidence.size();
  }
  summary.fence_count = payload.fences.size();
  summary.payload_digest = payload_digest;
  summary.file_digest = file_digest;
  summary.file_size = out.size();
  return out;
}

namespace {

/// Bounds checked reader. Every method fails closed and records the first
/// failure, so decoding can never read past the buffer or allocate from an
/// unvalidated length.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  bool ok() const noexcept { return ok_; }
  OutcomeCode code() const noexcept { return code_; }
  const std::string& detail() const noexcept { return detail_; }
  bool exhausted() const noexcept { return offset_ == data_.size(); }
  std::size_t offset() const noexcept { return offset_; }

  bool u8(std::uint8_t& value) {
    if (!require(1)) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(data_[offset_]);
    ++offset_;
    return true;
  }

  bool u16(std::uint16_t& value) {
    if (!require(2)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(data_[offset_]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data_[offset_ + 1])) << 8u));
    offset_ += 2;
    return true;
  }

  bool u32(std::uint32_t& value) {
    if (!require(4)) {
      return false;
    }
    value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset_ + index]))
               << (8u * index);
    }
    offset_ += 4;
    return true;
  }

  bool u64(std::uint64_t& value) {
    if (!require(8)) {
      return false;
    }
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[offset_ + index]))
               << (8u * index);
    }
    offset_ += 8;
    return true;
  }

  bool raw(std::size_t count, std::span<const std::byte>& out) {
    if (!require(count)) {
      return false;
    }
    out = data_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

  bool text(std::size_t max_length, std::string& out) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return false;
    }
    if (length > max_length) {
      fail(OutcomeCode::ResourceLimit, "text field length exceeds the maximum accepted length");
      return false;
    }
    std::span<const std::byte> slice;
    if (!raw(length, slice)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(slice.data()), slice.size());
    std::string error;
    if (!detail::validate_text(out, max_length, error)) {
      fail(OutcomeCode::PersistenceCorrupt, "text field is malformed: " + error);
      return false;
    }
    return true;
  }

  bool blob(std::size_t max_length, std::vector<std::byte>& out) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return false;
    }
    if (length > max_length) {
      fail(OutcomeCode::ResourceLimit, "binary field length exceeds the maximum accepted length");
      return false;
    }
    std::span<const std::byte> slice;
    if (!raw(length, slice)) {
      return false;
    }
    out.assign(slice.begin(), slice.end());
    return true;
  }

  bool digest(Digest& out) {
    std::span<const std::byte> slice;
    if (!raw(Digest::byte_count, slice)) {
      return false;
    }
    std::array<std::byte, Digest::byte_count> bytes{};
    std::copy(slice.begin(), slice.end(), bytes.begin());
    out = Digest::from_bytes(bytes);
    return true;
  }

  void fail(OutcomeCode code, std::string detail_text) {
    if (ok_) {
      ok_ = false;
      code_ = code;
      detail_ = std::move(detail_text);
    }
  }

 private:
  bool require(std::size_t count) {
    if (!ok_) {
      return false;
    }
    if (count > data_.size() - offset_) {
      fail(OutcomeCode::PersistenceCorrupt, "journal is truncated");
      return false;
    }
    return true;
  }

  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  OutcomeCode code_ = OutcomeCode::PersistenceCorrupt;
  std::string detail_;
};

template <class Tag>
bool read_id(Reader& reader, StrongId<Tag>& out, bool allow_empty) {
  std::string text;
  if (!reader.text(limits::max_identifier_length, text)) {
    return false;
  }
  if (text.empty()) {
    if (allow_empty) {
      out = StrongId<Tag>{};
      return true;
    }
    reader.fail(OutcomeCode::PersistenceCorrupt, "a required identifier is empty");
    return false;
  }
  const IdValidation validation = validate_identifier(text);
  if (validation != IdValidation::Ok) {
    reader.fail(OutcomeCode::PersistenceCorrupt,
                std::string("identifier is malformed: ") + std::string(to_string(validation)));
    return false;
  }
  out = StrongId<Tag>::from_validated(std::move(text));
  return true;
}

template <class Tag>
bool read_generation(Reader& reader, StrongGeneration<Tag>& out) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) {
    return false;
  }
  out = StrongGeneration<Tag>::from_value(value);
  return true;
}

bool read_enum(Reader& reader, std::uint8_t maximum, std::uint8_t& out, const char* field) {
  std::uint8_t value = 0;
  if (!reader.u8(value)) {
    return false;
  }
  if (value > maximum) {
    reader.fail(OutcomeCode::PersistenceCorrupt,
                std::string("field ") + field + " holds an invalid enumeration value");
    return false;
  }
  out = value;
  return true;
}

bool read_bool(Reader& reader, bool& out, const char* field) {
  std::uint8_t value = 0;
  if (!reader.u8(value)) {
    return false;
  }
  if (value > 1u) {
    reader.fail(OutcomeCode::PersistenceCorrupt,
                std::string("field ") + field + " holds an invalid boolean value");
    return false;
  }
  out = value == 1u;
  return true;
}

bool decode_evidence(Reader& reader, EvidenceRecord& record) {
  if (!read_id(reader, record.key.link, false)) return false;
  if (!read_id(reader, record.key.source, false)) return false;
  if (!read_id(reader, record.key.endpoint, true)) return false;
  std::uint8_t direction = 0;
  std::uint8_t kind = 0;
  if (!read_enum(reader, 2, direction, "evidence direction")) return false;
  if (!read_enum(reader, 10, kind, "evidence kind")) return false;
  record.key.direction = static_cast<EvidenceDirection>(direction);
  record.key.kind = static_cast<EvidenceKind>(kind);
  if (!read_id(reader, record.observation, false)) return false;
  if (!read_id(reader, record.publisher, false)) return false;
  if (!read_id(reader, record.worker_boot, false)) return false;
  if (!read_generation(reader, record.epoch)) return false;
  if (!read_generation(reader, record.source_generation)) return false;
  std::uint64_t sequence = 0;
  if (!reader.u64(sequence)) return false;
  record.sequence = sequence;
  if (!read_generation(reader, record.topology_generation)) return false;
  if (!read_generation(reader, record.endpoint_generation)) return false;
  std::uint8_t evidence_class = 0;
  std::uint8_t claim = 0;
  std::uint8_t cause = 0;
  if (!read_enum(reader, 2, evidence_class, "evidence class")) return false;
  if (!read_enum(reader, 6, claim, "evidence claim")) return false;
  if (!read_enum(reader, 8, cause, "degradation cause")) return false;
  record.evidence_class = static_cast<EvidenceClass>(evidence_class);
  record.claim = static_cast<EvidenceClaim>(claim);
  record.degradation_cause = static_cast<DegradationCause>(cause);
  std::uint64_t observed = 0;
  if (!reader.u64(observed)) return false;
  record.observed_at_unix_nanos = observed;
  if (!reader.text(limits::max_detail_length, record.detail)) return false;
  if (!reader.blob(limits::max_evidence_metadata_bytes, record.metadata)) return false;
  std::uint8_t status = 0;
  if (!read_enum(reader, 5, status, "evidence status")) return false;
  record.status = static_cast<EvidenceStatus>(status);
  if (!read_generation(reader, record.evidence_generation)) return false;
  if (!read_generation(reader, record.commit_sequence)) return false;
  if (!reader.digest(record.record_digest)) return false;
  if (record.compute_digest() != record.record_digest) {
    reader.fail(OutcomeCode::PersistenceCorrupt,
                "evidence record digest does not match its content");
    return false;
  }
  return true;
}

bool decode_link(Reader& reader, PersistedLink& link, bool include_history) {
  LinkStateRecord& record = link.record;
  if (!read_id(reader, record.record, false)) return false;
  if (!read_id(reader, record.binding.fabric, true)) return false;
  if (!read_id(reader, record.binding.site, true)) return false;
  if (!read_id(reader, record.binding.link, false)) return false;
  if (!read_generation(reader, record.binding.topology_generation)) return false;
  std::uint8_t link_class = 0;
  if (!read_enum(reader, 9, link_class, "link class")) return false;
  record.binding.link_class = static_cast<LinkClass>(link_class);
  std::uint8_t symmetry = 0;
  if (!read_enum(reader, 2, symmetry, "symmetry guarantee")) return false;
  record.binding.symmetry = static_cast<LinkSymmetryGuarantee>(symmetry);

  EndpointBinding& endpoints = record.binding.endpoints;
  if (!read_id(reader, endpoints.local, false)) return false;
  if (!read_generation(reader, endpoints.local_generation)) return false;
  if (!read_id(reader, endpoints.local_device, true)) return false;
  if (!read_id(reader, endpoints.local_port, true)) return false;
  if (!read_id(reader, endpoints.remote, true)) return false;
  if (!read_generation(reader, endpoints.remote_generation)) return false;
  if (!read_id(reader, endpoints.remote_device, true)) return false;
  if (!read_id(reader, endpoints.remote_port, true)) return false;

  std::uint32_t member_count = 0;
  if (!reader.u32(member_count)) return false;
  if (member_count > limits::max_authority_scope_links) {
    reader.fail(OutcomeCode::ResourceLimit, "member link count exceeds the accepted maximum");
    return false;
  }
  record.binding.member_links.clear();
  record.binding.member_links.reserve(member_count);
  for (std::uint32_t index = 0; index < member_count; ++index) {
    LinkId member;
    if (!read_id(reader, member, false)) return false;
    record.binding.member_links.push_back(member);
  }
  std::uint32_t backing_count = 0;
  if (!reader.u32(backing_count)) return false;
  if (backing_count > limits::max_authority_scope_links) {
    reader.fail(OutcomeCode::ResourceLimit, "backing link count exceeds the accepted maximum");
    return false;
  }
  record.binding.backing_links.clear();
  record.binding.backing_links.reserve(backing_count);
  for (std::uint32_t index = 0; index < backing_count; ++index) {
    BackingReference reference;
    if (!read_id(reader, reference.link, false)) return false;
    if (!read_generation(reader, reference.generation)) return false;
    record.binding.backing_links.push_back(reference);
  }

  std::uint8_t state = 0;
  std::uint8_t result_class = 0;
  std::uint8_t cause = 0;
  if (!read_enum(reader, 8, state, "operational state")) return false;
  if (!read_enum(reader, 9, result_class, "state result class")) return false;
  if (!read_enum(reader, 8, cause, "degradation cause")) return false;
  record.state = static_cast<LinkOperationalState>(state);
  record.result_class = static_cast<StateResultClass>(result_class);
  record.degradation_cause = static_cast<DegradationCause>(cause);
  if (!read_generation(reader, record.state_generation)) return false;
  if (!read_generation(reader, record.evidence_generation)) return false;
  if (!read_generation(reader, record.global_generation)) return false;
  if (!read_id(reader, record.last_publication, true)) return false;
  if (!read_id(reader, record.last_transition, true)) return false;
  if (!read_id(reader, record.last_publisher, true)) return false;
  std::uint8_t last_outcome = 0;
  if (!read_enum(reader, 23, last_outcome, "outcome code")) return false;
  record.last_outcome = static_cast<OutcomeCode>(last_outcome);
  if (!read_bool(reader, record.ever_established, "ever established")) return false;
  if (!read_bool(reader, record.durable, "durable")) return false;
  bool has_intent = false;
  if (!read_bool(reader, has_intent, "administrative intent presence")) return false;
  if (has_intent) {
    AdministrativeIntent intent;
    std::uint8_t intent_state = 0;
    std::uint8_t intent_cause = 0;
    if (!read_enum(reader, 8, intent_state, "administrative intent state")) return false;
    if (!read_enum(reader, 8, intent_cause, "administrative intent cause")) return false;
    intent.state = static_cast<LinkOperationalState>(intent_state);
    intent.cause = static_cast<DegradationCause>(intent_cause);
    if (!read_id(reader, intent.publication, false)) return false;
    if (!read_id(reader, intent.publisher, false)) return false;
    if (!read_generation(reader, intent.generation)) return false;
    record.admin_intent = intent;
  } else {
    record.admin_intent.reset();
  }
  if (!reader.digest(record.provenance_digest)) return false;

  std::uint32_t history_count = 0;
  if (!reader.u32(history_count)) return false;
  if (history_count > limits::max_history_per_link) {
    reader.fail(OutcomeCode::ResourceLimit, "state history count exceeds the accepted maximum");
    return false;
  }
  if (include_history || history_count > 0) {
    record.history.clear();
    record.history.reserve(history_count);
    for (std::uint32_t index = 0; index < history_count; ++index) {
      StateHistoryEntry history;
      if (!read_generation(reader, history.state_generation)) return false;
      std::uint8_t from_state = 0;
      std::uint8_t to_state = 0;
      std::uint8_t trigger = 0;
      std::uint8_t history_class = 0;
      std::uint8_t history_cause = 0;
      if (!read_enum(reader, 8, from_state, "history from state")) return false;
      if (!read_enum(reader, 8, to_state, "history to state")) return false;
      if (!read_enum(reader, 9, trigger, "history trigger")) return false;
      if (!read_enum(reader, 9, history_class, "history result class")) return false;
      if (!read_enum(reader, 8, history_cause, "history cause")) return false;
      history.from_state = static_cast<LinkOperationalState>(from_state);
      history.to_state = static_cast<LinkOperationalState>(to_state);
      history.trigger = static_cast<TransitionTrigger>(trigger);
      history.result_class = static_cast<StateResultClass>(history_class);
      history.cause = static_cast<DegradationCause>(history_cause);
      if (!read_id(reader, history.publication, true)) return false;
      if (!read_id(reader, history.publisher, true)) return false;
      if (!read_generation(reader, history.evidence_generation)) return false;
      if (!read_generation(reader, history.topology_generation)) return false;
      std::uint8_t history_outcome = 0;
      if (!read_enum(reader, 23, history_outcome, "history outcome")) return false;
      history.outcome = static_cast<OutcomeCode>(history_outcome);
      record.history.push_back(history);
    }
  }

  std::uint32_t evidence_count = 0;
  if (!reader.u32(evidence_count)) return false;
  if (evidence_count > limits::max_evidence_per_link) {
    reader.fail(OutcomeCode::ResourceLimit, "evidence count exceeds the accepted maximum");
    return false;
  }
  link.evidence.clear();
  link.evidence.reserve(evidence_count);
  std::unordered_set<std::string> seen_keys;
  for (std::uint32_t index = 0; index < evidence_count; ++index) {
    EvidenceRecord evidence;
    if (!decode_evidence(reader, evidence)) return false;
    if (evidence.key.link != record.binding.link) {
      reader.fail(OutcomeCode::PersistenceCorrupt,
                  "evidence record refers to a different link than its owning record");
      return false;
    }
    std::string key = evidence.key.to_string();
    if (!seen_keys.insert(key).second) {
      reader.fail(OutcomeCode::PersistenceCorrupt, "duplicate evidence key in the journal");
      return false;
    }
    link.evidence.push_back(std::move(evidence));
  }
  return true;
}

}  // namespace

Outcome decode_journal(std::span<const std::byte> bytes, const PersistenceConfig& config,
                       JournalPayload& payload, JournalSummary& summary) {
  const std::string config_error = config.validate();
  if (!config_error.empty()) {
    return Outcome::make(OutcomeCode::MalformedRequest, config_error);
  }
  if (bytes.size() < journal_header_bytes + journal_trailer_bytes) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt, "journal is shorter than its fixed frame");
  }
  Reader header(bytes.first(journal_header_bytes));
  std::uint8_t magic[4] = {0, 0, 0, 0};
  for (std::size_t index = 0; index < 4; ++index) {
    if (!header.u8(magic[index])) {
      return Outcome::make(header.code(), header.detail());
    }
  }
  if (magic[0] != static_cast<std::uint8_t>('L') || magic[1] != static_cast<std::uint8_t>('S') ||
      magic[2] != static_cast<std::uint8_t>('F') || magic[3] != static_cast<std::uint8_t>('1')) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt, "journal magic does not match LSF1");
  }
  std::uint16_t version = 0;
  std::uint16_t flags = 0;
  if (!header.u16(version) || !header.u16(flags)) {
    return Outcome::make(header.code(), header.detail());
  }
  if (version != journal_format_version) {
    return Outcome::make(OutcomeCode::UnsupportedState,
                         "journal format version " + std::to_string(version) +
                             " is not supported by this build");
  }
  std::uint64_t layout = 0;
  std::uint64_t epoch = 0;
  std::uint64_t global = 0;
  std::uint64_t global_state = 0;
  std::uint64_t topology = 0;
  std::uint64_t record_count = 0;
  std::uint64_t payload_length = 0;
  if (!header.u64(layout) || !header.u64(epoch) || !header.u64(global) ||
      !header.u64(global_state) || !header.u64(topology) || !header.u64(record_count) ||
      !header.u64(payload_length)) {
    return Outcome::make(header.code(), header.detail());
  }
  if (record_count > config.max_records) {
    return Outcome::make(OutcomeCode::ResourceLimit,
                         "journal declares more records than the configured maximum");
  }
  const std::size_t frame = journal_header_bytes + journal_trailer_bytes;
  if (payload_length > bytes.size() - frame) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt, "journal payload is truncated");
  }
  if (payload_length != bytes.size() - frame) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt,
                         "journal payload length does not match the file size");
  }

  const auto payload_span = bytes.subspan(journal_header_bytes,
                                          static_cast<std::size_t>(payload_length));
  const auto trailer_span = bytes.subspan(bytes.size() - journal_trailer_bytes);
  std::array<std::byte, Digest::byte_count> payload_digest_bytes{};
  std::array<std::byte, Digest::byte_count> file_digest_bytes{};
  std::copy_n(trailer_span.begin(), Digest::byte_count, payload_digest_bytes.begin());
  std::copy_n(trailer_span.begin() + static_cast<std::ptrdiff_t>(Digest::byte_count),
              Digest::byte_count, file_digest_bytes.begin());
  const Digest stored_payload_digest = Digest::from_bytes(payload_digest_bytes);
  const Digest stored_file_digest = Digest::from_bytes(file_digest_bytes);
  if (sha256(payload_span) != stored_payload_digest) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt,
                         "journal payload digest does not match its content");
  }
  Hasher file_hasher;
  file_hasher.update(bytes.first(bytes.size() - journal_trailer_bytes));
  if (file_hasher.finalize() != stored_file_digest) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt,
                         "journal file digest does not match its content");
  }

  Reader reader(payload_span);
  std::uint32_t links = 0;
  if (!reader.u32(links)) {
    return Outcome::make(reader.code(), reader.detail());
  }
  if (links != record_count) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt,
                         "journal link count does not match the header record count");
  }

  JournalPayload decoded;
  decoded.layout = LayoutGeneration::from_value(layout);
  decoded.epoch = CoordinatorEpoch::from_value(epoch);
  decoded.global_generation = GlobalGeneration::from_value(global);
  decoded.global_state_generation = LinkStateGeneration::from_value(global_state);
  decoded.topology_generation = TopologyGeneration::from_value(topology);
  decoded.links.reserve(links);
  std::unordered_set<std::string> seen_links;
  for (std::uint32_t index = 0; index < links; ++index) {
    PersistedLink link;
    if (!decode_link(reader, link, (flags & 1u) != 0u)) {
      return Outcome::make(reader.code(), reader.detail());
    }
    const LinkStateRecord& record = link.record;
    if (!seen_links.insert(record.binding.link.value()).second) {
      return Outcome::make(OutcomeCode::PersistenceCorrupt, "duplicate link record in the journal");
    }
    if (record.binding.topology_generation.is_zero()) {
      return Outcome::make(OutcomeCode::PersistenceCorrupt,
                           "link record has a zero topology generation");
    }
    if (record.state_generation > decoded.global_state_generation) {
      return Outcome::make(OutcomeCode::PersistenceCorrupt,
                           "link state generation exceeds the global state generation");
    }
    if (record.global_generation > decoded.global_generation) {
      return Outcome::make(OutcomeCode::PersistenceCorrupt,
                           "link global generation exceeds the journal global generation");
    }
    if (record.state == LinkOperationalState::Retired && !record.ever_established &&
        record.state_generation.value() == 0) {
      return Outcome::make(OutcomeCode::PersistenceCorrupt,
                           "retired link record has no state generation");
    }
    decoded.links.push_back(std::move(link));
  }

  std::uint32_t fences = 0;
  if (!reader.u32(fences)) {
    return Outcome::make(reader.code(), reader.detail());
  }
  if (fences > limits::max_publishers) {
    return Outcome::make(OutcomeCode::ResourceLimit, "fence count exceeds the accepted maximum");
  }
  decoded.fences.reserve(fences);
  for (std::uint32_t index = 0; index < fences; ++index) {
    PublisherFence fence;
    if (!read_id(reader, fence.publisher, false)) {
      return Outcome::make(reader.code(), reader.detail());
    }
    if (!read_id(reader, fence.worker_boot, false)) {
      return Outcome::make(reader.code(), reader.detail());
    }
    if (!read_generation(reader, fence.epoch)) {
      return Outcome::make(reader.code(), reader.detail());
    }
    if (!reader.text(limits::max_detail_length, fence.reason)) {
      return Outcome::make(reader.code(), reader.detail());
    }
    decoded.fences.push_back(std::move(fence));
  }
  if (!reader.exhausted()) {
    return Outcome::make(OutcomeCode::PersistenceCorrupt,
                         "journal payload holds trailing bytes after the last record");
  }

  summary.format_version = version;
  summary.flags = flags;
  summary.layout = decoded.layout;
  summary.epoch = decoded.epoch;
  summary.global_generation = decoded.global_generation;
  summary.global_state_generation = decoded.global_state_generation;
  summary.topology_generation = decoded.topology_generation;
  summary.record_count = decoded.links.size();
  summary.evidence_count = 0;
  for (const PersistedLink& link : decoded.links) {
    summary.evidence_count += link.evidence.size();
  }
  summary.fence_count = decoded.fences.size();
  summary.payload_digest = stored_payload_digest;
  summary.file_digest = stored_file_digest;
  summary.file_size = bytes.size();
  payload = std::move(decoded);
  return Outcome::make(OutcomeCode::Committed, "journal decoded");
}

Outcome read_journal_file(const PersistenceConfig& config, std::vector<std::byte>& bytes,
                          JournalSummary& summary) {
  const std::string config_error = config.validate();
  if (!config_error.empty()) {
    return Outcome::make(OutcomeCode::MalformedRequest, config_error);
  }
  std::error_code error;
  const std::uintmax_t size = std::filesystem::file_size(config.path, error);
  if (error) {
    return Outcome::make(OutcomeCode::PersistenceIoFailure,
                         "cannot read the size of the journal file");
  }
  if (size > static_cast<std::uintmax_t>(max_journal_bytes(config))) {
    return Outcome::make(OutcomeCode::ResourceLimit,
                         "journal file exceeds the configured size bound");
  }
  std::ifstream input(config.path, std::ios::binary);
  if (!input) {
    return Outcome::make(OutcomeCode::PersistenceIoFailure, "cannot open the journal file");
  }
  bytes.assign(static_cast<std::size_t>(size), std::byte{0});
  if (size > 0) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!input) {
      return Outcome::make(OutcomeCode::PersistenceIoFailure, "journal file could not be read");
    }
  }
  summary.file_size = size;
  return Outcome::make(OutcomeCode::Committed, "journal read");
}

Outcome write_journal_file(const PersistenceConfig& config, std::span<const std::byte> bytes) {
  const std::string config_error = config.validate();
  if (!config_error.empty()) {
    return Outcome::make(OutcomeCode::MalformedRequest, config_error);
  }
  const std::filesystem::path target(config.path);
  const std::filesystem::path directory =
      target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) {
    if (!config.create_parent_directory) {
      return Outcome::make(OutcomeCode::PersistenceIoFailure,
                           "the journal directory does not exist");
    }
    std::filesystem::create_directories(directory, error);
    if (error) {
      return Outcome::make(OutcomeCode::PersistenceIoFailure,
                           "the journal directory could not be created");
    }
  }
  std::filesystem::path temporary = target;
  temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      return Outcome::make(OutcomeCode::PersistenceIoFailure,
                           "the temporary journal file could not be created");
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(temporary, error);
      return Outcome::make(OutcomeCode::PersistenceIoFailure,
                           "the temporary journal file could not be written");
    }
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.wstring().c_str(), target.wstring().c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary, error);
    return Outcome::make(OutcomeCode::PersistenceIoFailure,
                         "the journal file could not be replaced atomically");
  }
#else
  std::filesystem::rename(temporary, target, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return Outcome::make(OutcomeCode::PersistenceIoFailure,
                         "the journal file could not be replaced atomically");
  }
#endif
  return Outcome::make(OutcomeCode::Committed, "journal written");
}

}  // namespace linkstate
