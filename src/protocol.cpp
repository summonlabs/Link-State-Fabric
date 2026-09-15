#include "linkstate/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "linkstate/limits.hpp"
#include "text.hpp"

namespace linkstate {
namespace {

class Writer {
 public:
  void u8(std::uint8_t value) { data_.push_back(static_cast<std::byte>(value)); }
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
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    data_.insert(data_.end(), bytes, bytes + value.size());
  }
  void raw(std::span<const std::byte> value) { data_.insert(data_.end(), value.begin(), value.end()); }

  std::vector<std::byte> take() { return std::move(data_); }
  const std::vector<std::byte>& data() const { return data_; }

 private:
  std::vector<std::byte> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  bool exhausted() const { return offset_ == data_.size(); }

  void fail(std::string text) {
    if (ok_) {
      ok_ = false;
      error_ = std::move(text);
    }
  }

  bool u8(std::uint8_t& value) {
    if (!require(1)) return false;
    value = std::to_integer<std::uint8_t>(data_[offset_]);
    ++offset_;
    return true;
  }
  bool u16(std::uint16_t& value) {
    if (!require(2)) return false;
    value = static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(data_[offset_]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data_[offset_ + 1])) << 8u));
    offset_ += 2;
    return true;
  }
  bool u32(std::uint32_t& value) {
    if (!require(4)) return false;
    value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data_[offset_ + index]))
               << (8u * index);
    }
    offset_ += 4;
    return true;
  }
  bool u64(std::uint64_t& value) {
    if (!require(8)) return false;
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[offset_ + index]))
               << (8u * index);
    }
    offset_ += 8;
    return true;
  }
  bool text(std::size_t max_length, std::string& out) {
    std::uint32_t length = 0;
    if (!u32(length)) return false;
    if (length > max_length) {
      fail("text field exceeds the accepted length");
      return false;
    }
    if (!require(length)) return false;
    out.assign(reinterpret_cast<const char*>(data_.data() + offset_), length);
    offset_ += length;
    std::string text_error;
    if (!detail::validate_text(out, max_length, text_error)) {
      fail("text field is malformed: " + text_error);
      return false;
    }
    return true;
  }
  bool raw(std::size_t count, std::span<const std::byte>& out) {
    if (!require(count)) return false;
    out = data_.subspan(offset_, count);
    offset_ += count;
    return true;
  }

 private:
  bool require(std::size_t count) {
    if (!ok_) return false;
    if (count > data_.size() - offset_) {
      fail("message is truncated");
      return false;
    }
    return true;
  }
  std::span<const std::byte> data_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  std::string error_;
};

template <class Tag>
void write_id(Writer& writer, const StrongId<Tag>& id) { writer.text(id.value()); }

template <class Tag>
bool read_id(Reader& reader, StrongId<Tag>& out, bool allow_empty) {
  std::string text;
  if (!reader.text(limits::max_identifier_length, text)) return false;
  if (text.empty()) {
    if (allow_empty) {
      out = StrongId<Tag>{};
      return true;
    }
    reader.fail("a required identifier is empty");
    return false;
  }
  const IdValidation validation = validate_identifier(text);
  if (validation != IdValidation::Ok) {
    reader.fail(std::string("identifier is malformed: ") + std::string(to_string(validation)));
    return false;
  }
  out = StrongId<Tag>::from_validated(std::move(text));
  return true;
}

template <class Tag>
void write_generation(Writer& writer, StrongGeneration<Tag> generation) {
  writer.u64(generation.value());
}

template <class Tag>
bool read_generation(Reader& reader, StrongGeneration<Tag>& out) {
  std::uint64_t value = 0;
  if (!reader.u64(value)) return false;
  out = StrongGeneration<Tag>::from_value(value);
  return true;
}

bool read_enum(Reader& reader, std::uint8_t maximum, std::uint8_t& out, const char* field) {
  std::uint8_t value = 0;
  if (!reader.u8(value)) return false;
  if (value > maximum) {
    reader.fail(std::string(field) + " holds an invalid enumeration value");
    return false;
  }
  out = value;
  return true;
}

void write_evidence(Writer& writer, const EvidenceRecord& record) {
  write_id(writer, record.key.link);
  write_id(writer, record.key.source);
  write_id(writer, record.key.endpoint);
  writer.u8(static_cast<std::uint8_t>(record.key.direction));
  writer.u8(static_cast<std::uint8_t>(record.key.kind));
  write_id(writer, record.observation);
  write_id(writer, record.publisher);
  write_id(writer, record.worker_boot);
  write_generation(writer, record.epoch);
  write_generation(writer, record.source_generation);
  writer.u64(record.sequence);
  write_generation(writer, record.topology_generation);
  write_generation(writer, record.endpoint_generation);
  writer.u8(static_cast<std::uint8_t>(record.evidence_class));
  writer.u8(static_cast<std::uint8_t>(record.claim));
  writer.u8(static_cast<std::uint8_t>(record.degradation_cause));
  writer.u64(record.observed_at_unix_nanos);
  writer.text(record.detail);
  writer.u32(static_cast<std::uint32_t>(record.metadata.size()));
  writer.raw(std::span<const std::byte>(record.metadata.data(), record.metadata.size()));
}

bool read_evidence(Reader& reader, EvidenceRecord& record) {
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
  if (!reader.u64(record.sequence)) return false;
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
  if (!reader.u64(record.observed_at_unix_nanos)) return false;
  if (!reader.text(limits::max_detail_length, record.detail)) return false;
  std::uint32_t metadata_length = 0;
  if (!reader.u32(metadata_length)) return false;
  if (metadata_length > limits::max_evidence_metadata_bytes) {
    reader.fail("evidence metadata exceeds the accepted length");
    return false;
  }
  std::span<const std::byte> metadata;
  if (!reader.raw(metadata_length, metadata)) return false;
  record.metadata.assign(metadata.begin(), metadata.end());
  return true;
}

}  // namespace

const char* to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
      return "HELLO";
    case MessageType::HelloAck:
      return "HELLO_ACK";
    case MessageType::RegisterPublisher:
      return "REGISTER_PUBLISHER";
    case MessageType::RegisterResult:
      return "REGISTER_RESULT";
    case MessageType::PublishEvidence:
      return "PUBLISH_EVIDENCE";
    case MessageType::PublishResult:
      return "PUBLISH_RESULT";
    case MessageType::WithdrawEvidence:
      return "WITHDRAW_EVIDENCE";
    case MessageType::WithdrawResult:
      return "WITHDRAW_RESULT";
    case MessageType::InvalidatePublisher:
      return "INVALIDATE_PUBLISHER";
    case MessageType::InvalidateResult:
      return "INVALIDATE_RESULT";
    case MessageType::QueryState:
      return "QUERY_STATE";
    case MessageType::QueryResult:
      return "QUERY_RESULT";
    case MessageType::Shutdown:
      return "SHUTDOWN";
    case MessageType::Error:
      return "ERROR";
  }
  return "UNKNOWN";
}

bool is_known_message_type(std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(MessageType::Hello) &&
         value <= static_cast<std::uint16_t>(MessageType::Error);
}

const char* to_string(FrameDecodeStatus status) noexcept {
  switch (status) {
    case FrameDecodeStatus::Complete:
      return "COMPLETE";
    case FrameDecodeStatus::Incomplete:
      return "INCOMPLETE";
    case FrameDecodeStatus::Malformed:
      return "MALFORMED";
    case FrameDecodeStatus::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case FrameDecodeStatus::TooLarge:
      return "TOO_LARGE";
  }
  return "MALFORMED";
}

std::vector<std::byte> encode_frame(MessageType type, std::uint64_t request_id,
                                    std::span<const std::byte> payload) {
  Writer header;
  for (const char marker : std::string_view(frame_magic_text)) {
    header.u8(static_cast<std::uint8_t>(marker));
  }
  header.u16(protocol_version);
  header.u16(static_cast<std::uint16_t>(type));
  header.u64(request_id);
  header.u32(static_cast<std::uint32_t>(payload.size()));
  header.u32(0);

  std::vector<std::byte> frame = header.take();
  frame.insert(frame.end(), payload.begin(), payload.end());
  const Digest integrity = sha256(std::span<const std::byte>(frame.data(), frame.size()));
  frame.insert(frame.end(), integrity.bytes().begin(),
               integrity.bytes().begin() + static_cast<std::ptrdiff_t>(frame_integrity_bytes));
  return frame;
}

FrameDecodeResult decode_frame(std::span<const std::byte> buffer) {
  FrameDecodeResult result;
  if (buffer.size() < frame_header_bytes) {
    result.status = FrameDecodeStatus::Incomplete;
    result.detail = "frame header is not complete";
    return result;
  }
  Reader reader(buffer);
  std::uint16_t version = 0;
  std::uint16_t type = 0;
  std::uint64_t request_id = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t reserved = 0;
  bool magic_ok = true;
  for (const char marker : std::string_view(frame_magic_text)) {
    std::uint8_t value = 0;
    if (!reader.u8(value)) {
      magic_ok = false;
      break;
    }
    if (value != static_cast<std::uint8_t>(marker)) {
      magic_ok = false;
    }
  }
  if (!magic_ok) {
    result.status = FrameDecodeStatus::Malformed;
    result.detail = "frame magic does not match LSFP";
    return result;
  }
  if (!reader.u16(version) || !reader.u16(type) || !reader.u64(request_id) ||
      !reader.u32(payload_length) || !reader.u32(reserved)) {
    result.status = FrameDecodeStatus::Malformed;
    result.detail = "frame header could not be decoded";
    return result;
  }
  if (version != protocol_version) {
    result.status = FrameDecodeStatus::UnsupportedVersion;
    result.detail = "frame protocol version is not supported";
    return result;
  }
  if (!is_known_message_type(type)) {
    result.status = FrameDecodeStatus::Malformed;
    result.detail = "frame carries an unknown message type";
    return result;
  }
  if (reserved != 0) {
    result.status = FrameDecodeStatus::Malformed;
    result.detail = "frame reserved field is not zero";
    return result;
  }
  if (payload_length > limits::max_frame_payload_bytes) {
    result.status = FrameDecodeStatus::TooLarge;
    result.detail = "frame payload exceeds the maximum accepted size";
    return result;
  }
  const std::size_t total = frame_header_bytes + static_cast<std::size_t>(payload_length) +
                            frame_integrity_bytes;
  if (buffer.size() < total) {
    result.status = FrameDecodeStatus::Incomplete;
    result.detail = "frame payload is not complete";
    return result;
  }
  const Digest integrity =
      sha256(buffer.first(frame_header_bytes + static_cast<std::size_t>(payload_length)));
  if (!std::equal(integrity.bytes().begin(),
                  integrity.bytes().begin() + static_cast<std::ptrdiff_t>(frame_integrity_bytes),
                  buffer.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes + payload_length))) {
    result.status = FrameDecodeStatus::Malformed;
    result.detail = "frame integrity check failed";
    return result;
  }
  result.frame.type = static_cast<MessageType>(type);
  result.frame.request_id = request_id;
  result.frame.payload.assign(
      buffer.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes),
      buffer.begin() + static_cast<std::ptrdiff_t>(frame_header_bytes + payload_length));
  result.status = FrameDecodeStatus::Complete;
  result.consumed = total;
  return result;
}

std::vector<std::byte> encode_message(const Message& message, std::string& error) {
  Writer writer;
  error.clear();
  switch (message.type) {
    case MessageType::Hello:
      writer.text(message.hello.agent);
      write_id(writer, message.hello.worker_boot);
      write_generation(writer, message.hello.epoch);
      break;
    case MessageType::HelloAck:
      writer.u16(message.hello_ack.version);
      write_generation(writer, message.hello_ack.epoch);
      writer.text(message.hello_ack.coordinator);
      writer.u64(message.hello_ack.link_count);
      break;
    case MessageType::RegisterPublisher:
      write_id(writer, message.registration.publisher);
      write_id(writer, message.registration.worker_boot);
      write_id(writer, message.registration.source);
      write_id(writer, message.registration.fabric);
      write_generation(writer, message.registration.epoch);
      writer.u32(static_cast<std::uint32_t>(message.registration.links.size()));
      for (const LinkId& link : message.registration.links) {
        write_id(writer, link);
      }
      break;
    case MessageType::PublishEvidence:
      write_id(writer, message.publish.publication);
      write_evidence(writer, message.publish.observation);
      writer.u8(message.publish.has_expected_evidence_generation ? 1u : 0u);
      write_generation(writer, message.publish.expected_evidence_generation);
      writer.u8(message.publish.has_expected_state_generation ? 1u : 0u);
      write_generation(writer, message.publish.expected_state_generation);
      break;
    case MessageType::WithdrawEvidence:
      write_id(writer, message.withdraw.key.link);
      write_id(writer, message.withdraw.key.source);
      write_id(writer, message.withdraw.key.endpoint);
      writer.u8(static_cast<std::uint8_t>(message.withdraw.key.direction));
      writer.u8(static_cast<std::uint8_t>(message.withdraw.key.kind));
      write_id(writer, message.withdraw.observation);
      write_id(writer, message.withdraw.publisher);
      write_id(writer, message.withdraw.worker_boot);
      write_generation(writer, message.withdraw.epoch);
      write_generation(writer, message.withdraw.source_generation);
      writer.u64(message.withdraw.sequence);
      writer.text(message.withdraw.reason);
      break;
    case MessageType::InvalidatePublisher:
      write_id(writer, message.invalidate.publisher);
      write_id(writer, message.invalidate.worker_boot);
      write_generation(writer, message.invalidate.epoch);
      writer.text(message.invalidate.reason);
      break;
    case MessageType::QueryState:
      write_id(writer, message.query.link);
      break;
    case MessageType::RegisterResult:
    case MessageType::PublishResult:
    case MessageType::WithdrawResult:
    case MessageType::InvalidateResult:
    case MessageType::QueryResult:
    case MessageType::Error:
      writer.u8(static_cast<std::uint8_t>(message.result.code));
      writer.text(message.result.detail);
      writer.u8(static_cast<std::uint8_t>(message.result.state));
      write_generation(writer, message.result.state_generation);
      write_generation(writer, message.result.evidence_generation);
      write_generation(writer, message.result.global_generation);
      write_generation(writer, message.result.topology_generation);
      write_generation(writer, message.result.endpoint_generation);
      writer.u8(message.result.found ? 1u : 0u);
      break;
    case MessageType::Shutdown:
      writer.text(message.result.detail);
      break;
    default:
      error = "message type cannot be encoded";
      return {};
  }
  return writer.take();
}

bool decode_message(MessageType type, std::span<const std::byte> payload, Message& message,
                    std::string& error) {
  Reader reader(payload);
  message = Message{};
  message.type = type;
  switch (type) {
    case MessageType::Hello:
      if (!reader.text(limits::max_identifier_length, message.hello.agent)) break;
      if (!read_id(reader, message.hello.worker_boot, false)) break;
      if (!read_generation(reader, message.hello.epoch)) break;
      break;
    case MessageType::HelloAck:
      if (!reader.u16(message.hello_ack.version)) break;
      if (!read_generation(reader, message.hello_ack.epoch)) break;
      if (!reader.text(limits::max_identifier_length, message.hello_ack.coordinator)) break;
      if (!reader.u64(message.hello_ack.link_count)) break;
      break;
    case MessageType::RegisterPublisher:
      if (!read_id(reader, message.registration.publisher, false)) break;
      if (!read_id(reader, message.registration.worker_boot, false)) break;
      if (!read_id(reader, message.registration.source, false)) break;
      if (!read_id(reader, message.registration.fabric, true)) break;
      if (!read_generation(reader, message.registration.epoch)) break;
      {
        std::uint32_t count = 0;
        if (!reader.u32(count)) break;
        if (count > limits::max_authority_scope_links) {
          reader.fail("registration link count exceeds the accepted maximum");
          break;
        }
        message.registration.links.clear();
        message.registration.links.reserve(count);
        bool failed = false;
        for (std::uint32_t index = 0; index < count; ++index) {
          LinkId link;
          if (!read_id(reader, link, false)) {
            failed = true;
            break;
          }
          message.registration.links.push_back(link);
        }
        if (failed) break;
      }
      break;
    case MessageType::PublishEvidence:
      if (!read_id(reader, message.publish.publication, false)) break;
      if (!read_evidence(reader, message.publish.observation)) break;
      {
        std::uint8_t flag = 0;
        if (!reader.u8(flag)) break;
        if (flag > 1u) {
          reader.fail("expected evidence generation flag is invalid");
          break;
        }
        message.publish.has_expected_evidence_generation = flag == 1u;
        if (!read_generation(reader, message.publish.expected_evidence_generation)) break;
        if (!reader.u8(flag)) break;
        if (flag > 1u) {
          reader.fail("expected state generation flag is invalid");
          break;
        }
        message.publish.has_expected_state_generation = flag == 1u;
        if (!read_generation(reader, message.publish.expected_state_generation)) break;
      }
      break;
    case MessageType::WithdrawEvidence:
      if (!read_id(reader, message.withdraw.key.link, false)) break;
      if (!read_id(reader, message.withdraw.key.source, false)) break;
      if (!read_id(reader, message.withdraw.key.endpoint, true)) break;
      {
        std::uint8_t direction = 0;
        std::uint8_t kind = 0;
        if (!read_enum(reader, 2, direction, "withdrawal direction")) break;
        if (!read_enum(reader, 10, kind, "withdrawal kind")) break;
        message.withdraw.key.direction = static_cast<EvidenceDirection>(direction);
        message.withdraw.key.kind = static_cast<EvidenceKind>(kind);
      }
      if (!read_id(reader, message.withdraw.observation, false)) break;
      if (!read_id(reader, message.withdraw.publisher, false)) break;
      if (!read_id(reader, message.withdraw.worker_boot, false)) break;
      if (!read_generation(reader, message.withdraw.epoch)) break;
      if (!read_generation(reader, message.withdraw.source_generation)) break;
      if (!reader.u64(message.withdraw.sequence)) break;
      if (!reader.text(limits::max_detail_length, message.withdraw.reason)) break;
      break;
    case MessageType::InvalidatePublisher:
      if (!read_id(reader, message.invalidate.publisher, false)) break;
      if (!read_id(reader, message.invalidate.worker_boot, true)) break;
      if (!read_generation(reader, message.invalidate.epoch)) break;
      if (!reader.text(limits::max_detail_length, message.invalidate.reason)) break;
      break;
    case MessageType::QueryState:
      if (!read_id(reader, message.query.link, false)) break;
      break;
    case MessageType::RegisterResult:
    case MessageType::PublishResult:
    case MessageType::WithdrawResult:
    case MessageType::InvalidateResult:
    case MessageType::QueryResult:
    case MessageType::Error:
      {
        std::uint8_t code = 0;
        if (!read_enum(reader, 23, code, "outcome code")) break;
        message.result.code = static_cast<OutcomeCode>(code);
        if (!reader.text(limits::max_detail_length, message.result.detail)) break;
        std::uint8_t state = 0;
        if (!read_enum(reader, 8, state, "state")) break;
        message.result.state = static_cast<LinkOperationalState>(state);
        if (!read_generation(reader, message.result.state_generation)) break;
        if (!read_generation(reader, message.result.evidence_generation)) break;
        if (!read_generation(reader, message.result.global_generation)) break;
        if (!read_generation(reader, message.result.topology_generation)) break;
        if (!read_generation(reader, message.result.endpoint_generation)) break;
        std::uint8_t found = 0;
        if (!reader.u8(found)) break;
        if (found > 1u) {
          reader.fail("found flag is invalid");
          break;
        }
        message.result.found = found == 1u;
      }
      break;
    case MessageType::Shutdown:
      if (!reader.text(limits::max_detail_length, message.result.detail)) break;
      break;
    default:
      error = "message type cannot be decoded";
      return false;
  }
  if (!reader.ok()) {
    error = reader.error();
    return false;
  }
  if (!reader.exhausted()) {
    error = "message payload holds trailing bytes";
    return false;
  }
  return true;
}

}  // namespace linkstate
