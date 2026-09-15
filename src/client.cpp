#include "linkstate/client.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "net.hpp"
#include "text.hpp"

namespace linkstate {
namespace {

detail::socket_handle to_socket(void* handle) {
  return static_cast<detail::socket_handle>(reinterpret_cast<std::uintptr_t>(handle));
}

void* from_socket(detail::socket_handle socket) {
  return reinterpret_cast<void*>(static_cast<std::uintptr_t>(socket));
}

LinkId request_link(const Message& request) {
  switch (request.type) {
    case MessageType::PublishEvidence:
      return request.publish.observation.key.link;
    case MessageType::WithdrawEvidence:
      return request.withdraw.key.link;
    case MessageType::QueryState:
      return request.query.link;
    default:
      return LinkId{};
  }
}

LinkStateView view_from(const ResultMessage& result, const LinkId& link) {
  LinkStateView view;
  view.record.binding.link = link;
  view.record.state = result.state;
  view.record.state_generation = result.state_generation;
  view.record.evidence_generation = result.evidence_generation;
  view.record.global_generation = result.global_generation;
  view.record.binding.topology_generation = result.topology_generation;
  view.record.binding.endpoints.local_generation = result.endpoint_generation;
  view.record.result_class = StateResultClass::AuthoritativeUnknown;
  return view;
}

}  // namespace

std::string PublisherClientConfig::validate() const {
  if (address.empty()) {
    return "coordinator address is required";
  }
  if (port == 0) {
    return "coordinator port is required";
  }
  if (!publisher.valid()) {
    return "publisher identity is required";
  }
  if (!worker_boot.valid()) {
    return "worker boot identity is required";
  }
  if (!source.valid()) {
    return "source identity is required";
  }
  if (links.empty()) {
    return "at least one authorized link is required";
  }
  if (agent_name.empty()) {
    return "agent name is required";
  }
  return std::string();
}

PublisherClient::PublisherClient(PublisherClientConfig config) : config_(std::move(config)) {}

PublisherClient::~PublisherClient() {
  (void)disconnect();
}

Outcome PublisherClient::connect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) {
    return Outcome::make(OutcomeCode::Idempotent, "the client is already connected");
  }
  const std::string config_error = config_.validate();
  if (!config_error.empty()) {
    last_error_ = config_error;
    return Outcome::make(OutcomeCode::MalformedRequest, config_error);
  }
  if (!detail::net_available()) {
    last_error_ = "the distributed transport is unavailable on this platform";
    return Outcome::make(OutcomeCode::UnsupportedCapability, last_error_);
  }
  std::string error;
  const detail::socket_handle socket =
      detail::net_connect(config_.address, config_.port, error);
  if (socket == detail::invalid_socket) {
    last_error_ = error;
    return Outcome::make(OutcomeCode::TransportFailure, error);
  }
  socket_ = from_socket(socket);
  connected_ = true;

  Message hello;
  hello.type = MessageType::Hello;
  hello.hello.agent = config_.agent_name;
  hello.hello.worker_boot = config_.worker_boot;
  hello.hello.epoch = epoch_;
  Message ack;
  const Outcome outcome = transact(hello, MessageType::HelloAck, ack, nullptr);
  if (outcome.is_error()) {
    connected_ = false;
    bool expected = false;
    (void)expected;
    detail::net_shutdown(socket);
    detail::net_close(socket);
    socket_ = nullptr;
    last_error_ = outcome.detail;
    return outcome;
  }
  if (ack.hello_ack.version != protocol_version) {
    connected_ = false;
    detail::net_shutdown(socket);
    detail::net_close(socket);
    socket_ = nullptr;
    last_error_ = "the coordinator speaks an unsupported protocol version";
    return Outcome::make(OutcomeCode::UnsupportedState, last_error_);
  }
  epoch_ = ack.hello_ack.epoch;
  last_error_.clear();
  std::string detail_text = "connected to ";
  detail_text += ack.hello_ack.coordinator;
  detail_text += " at coordinator epoch ";
  detail_text += epoch_.to_string();
  return Outcome::make(OutcomeCode::Committed, detail_text);
}

Outcome PublisherClient::register_publisher() {
  std::lock_guard<std::mutex> lock(mutex_);
  Message request;
  request.type = MessageType::RegisterPublisher;
  request.registration.publisher = config_.publisher;
  request.registration.worker_boot = config_.worker_boot;
  request.registration.source = config_.source;
  request.registration.fabric = config_.fabric;
  request.registration.epoch = epoch_;
  request.registration.links = config_.links;
  Message response;
  return transact(request, MessageType::RegisterResult, response, nullptr);
}

Outcome PublisherClient::disconnect() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!connected_ || socket_ == nullptr) {
    return Outcome::make(OutcomeCode::Idempotent, "the client is not connected");
  }
  Message request;
  request.type = MessageType::Shutdown;
  request.result.detail = "worker shutdown";
  Message response;
  const Outcome outcome = transact(request, MessageType::Shutdown, response, nullptr);
  const detail::socket_handle socket = to_socket(socket_);
  detail::net_shutdown(socket);
  detail::net_close(socket);
  socket_ = nullptr;
  connected_ = false;
  if (outcome.is_error()) {
    last_error_ = outcome.detail;
    return outcome;
  }
  return Outcome::make(OutcomeCode::Committed, "the session was shut down cleanly");
}

void PublisherClient::abandon() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (socket_ == nullptr) {
    connected_ = false;
    return;
  }
  const detail::socket_handle socket = to_socket(socket_);
  detail::net_shutdown(socket);
  detail::net_close(socket);
  socket_ = nullptr;
  connected_ = false;
}

Outcome PublisherClient::publish(const PublishEvidenceRequest& request, LinkStateView* view) {
  std::lock_guard<std::mutex> lock(mutex_);
  Message message;
  message.type = MessageType::PublishEvidence;
  message.publish.publication = request.publication;
  message.publish.observation = request.observation;
  if (request.expected_evidence_generation.has_value()) {
    message.publish.has_expected_evidence_generation = true;
    message.publish.expected_evidence_generation = request.expected_evidence_generation.value();
  }
  if (request.expected_state_generation.has_value()) {
    message.publish.has_expected_state_generation = true;
    message.publish.expected_state_generation = request.expected_state_generation.value();
  }
  Message response;
  return transact(message, MessageType::PublishResult, response, view);
}

Outcome PublisherClient::withdraw(const WithdrawEvidenceRequest& request, LinkStateView* view) {
  std::lock_guard<std::mutex> lock(mutex_);
  Message message;
  message.type = MessageType::WithdrawEvidence;
  message.withdraw.key = request.key;
  message.withdraw.observation = request.observation;
  message.withdraw.publisher = request.publisher;
  message.withdraw.worker_boot = request.worker_boot;
  message.withdraw.epoch = request.epoch;
  message.withdraw.source_generation = request.source_generation;
  message.withdraw.sequence = request.sequence;
  message.withdraw.reason = request.reason;
  Message response;
  return transact(message, MessageType::WithdrawResult, response, view);
}

Outcome PublisherClient::invalidate_publisher(const InvalidatePublisherRequest& request) {
  std::lock_guard<std::mutex> lock(mutex_);
  Message message;
  message.type = MessageType::InvalidatePublisher;
  message.invalidate.publisher = request.publisher;
  message.invalidate.worker_boot = request.worker_boot.has_value() ? request.worker_boot.value()
                                                                 : WorkerBootId{};
  message.invalidate.epoch = request.epoch;
  message.invalidate.reason = request.reason;
  Message response;
  return transact(message, MessageType::InvalidateResult, response, nullptr);
}

bool PublisherClient::query_state(const LinkId& link, LinkStateView& view) {
  std::lock_guard<std::mutex> lock(mutex_);
  Message message;
  message.type = MessageType::QueryState;
  message.query.link = link;
  Message response;
  const Outcome outcome = transact(message, MessageType::QueryResult, response, &view);
  return !outcome.is_error();
}

CoordinatorEpoch PublisherClient::epoch() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return epoch_;
}

bool PublisherClient::connected() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return connected_;
}

const std::string& PublisherClient::last_error() const { return last_error_; }

std::uint64_t PublisherClient::requests_sent() const { return requests_sent_; }

std::uint64_t PublisherClient::responses_received() const { return responses_received_; }

Outcome PublisherClient::transact(const Message& request, MessageType expected_response,
                                  Message& response, LinkStateView* view) {
  if (!connected_ || socket_ == nullptr) {
    return Outcome::make(OutcomeCode::TransportFailure, "the client is not connected");
  }
  std::string error;
  const std::vector<std::byte> payload = encode_message(request, error);
  if (!error.empty()) {
    return Outcome::make(OutcomeCode::MalformedRequest, error);
  }
  const std::uint64_t request_id = ++request_counter_;
  const std::vector<std::byte> frame = encode_frame(request.type, request_id, payload);
  const detail::socket_handle socket = to_socket(socket_);
  if (!detail::net_send_all(socket, frame, error)) {
    last_error_ = error;
    connected_ = false;
    return Outcome::make(OutcomeCode::TransportFailure, error);
  }
  ++requests_sent_;

  std::vector<std::byte> buffer;
  std::vector<std::byte> chunk(8192);
  const std::size_t buffered_limit =
      limits::max_frame_payload_bytes + frame_header_bytes + frame_integrity_bytes;
  while (true) {
    const FrameDecodeResult decoded =
        decode_frame(std::span<const std::byte>(buffer.data(), buffer.size()));
    if (decoded.status == FrameDecodeStatus::Complete) {
      buffer.erase(buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
      if (decoded.frame.request_id != request_id) {
        last_error_ = "the response does not match the outstanding request";
        return Outcome::make(OutcomeCode::TransportFailure, last_error_);
      }
      if (decoded.frame.type != expected_response) {
        if (decoded.frame.type == MessageType::Error) {
          Message failure;
          if (decode_message(MessageType::Error, decoded.frame.payload, failure, error)) {
            last_error_ = failure.result.detail;
            return Outcome::make(failure.result.code, failure.result.detail);
          }
        }
        last_error_ = "the coordinator replied with an unexpected message type";
        return Outcome::make(OutcomeCode::TransportFailure, last_error_);
      }
      if (!decode_message(decoded.frame.type, decoded.frame.payload, response, error)) {
        last_error_ = error;
        return Outcome::make(OutcomeCode::MalformedRequest, error);
      }
      ++responses_received_;
      // Only the result shaped responses carry an outcome code; the handshake
      // and the shutdown acknowledgement do not.
      Outcome outcome;
      switch (decoded.frame.type) {
        case MessageType::HelloAck:
          outcome = Outcome::make(OutcomeCode::Committed, "coordinator handshake complete");
          break;
        case MessageType::Shutdown:
          outcome = Outcome::make(OutcomeCode::Committed, response.result.detail);
          break;
        default:
          outcome = Outcome::make(response.result.code, response.result.detail);
          outcome.state_generation = response.result.state_generation;
          outcome.evidence_generation = response.result.evidence_generation;
          outcome.global_generation = response.result.global_generation;
          if (view != nullptr && response.result.found) {
            *view = view_from(response.result, request_link(request));
          }
          break;
      }
      return outcome;
    }
    if (decoded.status != FrameDecodeStatus::Incomplete) {
      last_error_ = std::string("protocol violation: ") + to_string(decoded.status);
      connected_ = false;
      return Outcome::make(OutcomeCode::TransportFailure, last_error_);
    }
    const int received = detail::net_receive(socket, chunk.data(), chunk.size(), error);
    if (received <= 0) {
      last_error_ = error.empty() ? "the coordinator closed the connection" : error;
      connected_ = false;
      return Outcome::make(OutcomeCode::TransportFailure, last_error_);
    }
    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + received);
    if (buffer.size() > buffered_limit) {
      last_error_ = "the response exceeds the maximum frame size";
      connected_ = false;
      return Outcome::make(OutcomeCode::TransportFailure, last_error_);
    }
  }
}

}  // namespace linkstate
