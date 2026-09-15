#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "linkstate/authority.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/record.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// Frame magic: the four bytes "LSFP", written and compared verbatim so that a
/// hex dump of a frame shows the documented marker.
inline constexpr char frame_magic_text[] = "LSFP";

/// Version of the framed link-state publication protocol.
inline constexpr std::uint16_t protocol_version = 1;

/// Fixed frame header size in bytes.
inline constexpr std::size_t frame_header_bytes = 24;

/// Integrity field size in bytes: the leading bytes of the SHA-256 of the frame.
inline constexpr std::size_t frame_integrity_bytes = 8;

/// Message types carried by a frame.
enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  RegisterPublisher = 3,
  RegisterResult = 4,
  PublishEvidence = 5,
  PublishResult = 6,
  WithdrawEvidence = 7,
  WithdrawResult = 8,
  InvalidatePublisher = 9,
  InvalidateResult = 10,
  QueryState = 11,
  QueryResult = 12,
  Shutdown = 13,
  Error = 14,
};

LSF_EXPORT const char* to_string(MessageType type) noexcept;
LSF_EXPORT bool is_known_message_type(std::uint16_t value) noexcept;

/// Result of decoding one frame from a byte stream.
enum class FrameDecodeStatus : std::uint8_t {
  Complete = 0,
  Incomplete,
  Malformed,
  UnsupportedVersion,
  TooLarge,
};

LSF_EXPORT const char* to_string(FrameDecodeStatus status) noexcept;

/// A decoded frame.
struct LSF_EXPORT Frame {
  MessageType type = MessageType::Hello;
  std::uint64_t request_id = 0;
  std::vector<std::byte> payload;
};

/// Decoding result for one frame.
struct LSF_EXPORT FrameDecodeResult {
  FrameDecodeStatus status = FrameDecodeStatus::Incomplete;
  Frame frame;
  std::size_t consumed = 0;
  std::string detail;
};

/// Encodes one frame: header, payload and integrity field.
LSF_EXPORT std::vector<std::byte> encode_frame(MessageType type, std::uint64_t request_id,
                                               std::span<const std::byte> payload);

/// Decodes the first frame of a stream buffer.
///
/// The decoder validates magic, version, message type, bounded payload length
/// and the integrity field before reporting Complete. Incomplete means more
/// bytes are required; every other status is a protocol violation.
LSF_EXPORT FrameDecodeResult decode_frame(std::span<const std::byte> buffer);

/// Hello handshake sent by a connecting worker.
struct LSF_EXPORT HelloMessage {
  std::string agent;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
};

/// Hello response carrying the current coordinator epoch.
struct LSF_EXPORT HelloAckMessage {
  std::uint16_t version = protocol_version;
  CoordinatorEpoch epoch;
  std::string coordinator;
  std::uint64_t link_count = 0;
};

/// Publisher registration.
struct LSF_EXPORT RegisterMessage {
  PublisherId publisher;
  WorkerBootId worker_boot;
  SourceId source;
  FabricId fabric;
  CoordinatorEpoch epoch;
  std::vector<LinkId> links;
};

/// Evidence publication request.
struct LSF_EXPORT PublishMessage {
  PublicationId publication;
  EvidenceRecord observation;
  bool has_expected_evidence_generation = false;
  EvidenceGeneration expected_evidence_generation;
  bool has_expected_state_generation = false;
  LinkStateGeneration expected_state_generation;
};

/// Evidence withdrawal request.
struct LSF_EXPORT WithdrawMessage {
  EvidenceKey key;
  ObservationId observation;
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  SourceGeneration source_generation;
  std::uint64_t sequence = 0;
  std::string reason;
};

/// Publisher invalidation request.
struct LSF_EXPORT InvalidateMessage {
  PublisherId publisher;
  WorkerBootId worker_boot;
  CoordinatorEpoch epoch;
  std::string reason;
};

/// State query.
struct LSF_EXPORT QueryMessage {
  LinkId link;
};

/// Uniform result of a mutation or query.
struct LSF_EXPORT ResultMessage {
  OutcomeCode code = OutcomeCode::MalformedRequest;
  std::string detail;
  LinkOperationalState state = LinkOperationalState::Unknown;
  LinkStateGeneration state_generation;
  EvidenceGeneration evidence_generation;
  GlobalGeneration global_generation;
  /// Structural binding of the queried link, so that a worker can bind its
  /// evidence to the exact topology and endpoint generations it observed.
  TopologyGeneration topology_generation;
  EndpointGeneration endpoint_generation;
  bool found = false;
};

/// Union style protocol message.
struct LSF_EXPORT Message {
  MessageType type = MessageType::Hello;
  HelloMessage hello;
  HelloAckMessage hello_ack;
  RegisterMessage registration;
  PublishMessage publish;
  WithdrawMessage withdraw;
  InvalidateMessage invalidate;
  QueryMessage query;
  ResultMessage result;
};

/// Encodes a message payload. Returns an empty vector and sets error on failure.
LSF_EXPORT std::vector<std::byte> encode_message(const Message& message, std::string& error);

/// Decodes a message payload for the given type.
LSF_EXPORT bool decode_message(MessageType type, std::span<const std::byte> payload, Message& message,
                               std::string& error);

}  // namespace linkstate
