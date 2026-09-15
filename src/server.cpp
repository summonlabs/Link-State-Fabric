#include "linkstate/server.hpp"

#include <algorithm>
#include <cstdint>
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

void fill_result(ResultMessage& result, const Outcome& outcome, const LinkStateView* view) {
  result = ResultMessage{};
  result.code = outcome.code;
  result.detail = outcome.detail;
  result.state_generation = outcome.state_generation;
  result.evidence_generation = outcome.evidence_generation;
  result.global_generation = outcome.global_generation;
  if (view != nullptr) {
    result.found = true;
    result.state = view->state();
    result.state_generation = view->record.state_generation;
    result.evidence_generation = view->record.evidence_generation;
    result.global_generation = view->record.global_generation;
    result.topology_generation = view->record.binding.topology_generation;
    result.endpoint_generation = view->record.binding.endpoints.local_generation;
  }
}

/// Persists the journal when a request changed durable state.
///
/// Writing synchronously after each accepted mutation keeps durable structure
/// and generation counters crash consistent without a wall clock checkpoint
/// timer; the caller chooses whether a journal is configured at all.
void checkpoint(LinkStateEngine& engine, const CoordinatorConfig& config, const Outcome& outcome) {
  if (config.journal_path.empty()) {
    return;
  }
  if (!outcome.committed() && outcome.code != OutcomeCode::EvidenceConflict) {
    return;
  }
  PersistenceConfig persistence;
  persistence.path = config.journal_path;
  persistence.include_history = config.journal_history;
  (void)engine.save(persistence);
}

}  // namespace

const char* to_string(SessionEvent::Kind kind) noexcept {
  switch (kind) {
    case SessionEvent::Kind::Accepted:
      return "ACCEPTED";
    case SessionEvent::Kind::Closed:
      return "CLOSED";
  }
  return "CLOSED";
}

struct CoordinatorServer::Session {
  detail::socket_handle socket = detail::invalid_socket;
  std::thread thread;
  std::mutex mutex;
  std::atomic<bool> finished{false};
  std::atomic<bool> closed{false};
  std::string agent;
  std::vector<std::pair<PublisherId, WorkerBootId>> registrations;

  Session() = default;
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
};

std::string CoordinatorConfig::validate() const {
  if (bind_address.empty()) {
    return "bind address is required";
  }
  if (max_sessions == 0 || max_sessions > limits::max_sessions) {
    return "max_sessions must be between 1 and limits::max_sessions";
  }
  if (coordinator_name.empty()) {
    return "coordinator name is required";
  }
  if (!journal_path.empty()) {
    PersistenceConfig persistence;
    persistence.path = journal_path;
    persistence.include_history = journal_history;
    const std::string journal_error = persistence.validate();
    if (!journal_error.empty()) {
      return journal_error;
    }
  }
  return std::string();
}

CoordinatorServer::CoordinatorServer(LinkStateEngine& engine, CoordinatorConfig config)
    : engine_(engine), config_(std::move(config)) {}

CoordinatorServer::~CoordinatorServer() { (void)stop(); }

Outcome CoordinatorServer::start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return Outcome::make(OutcomeCode::Idempotent, "the coordinator is already listening");
  }
  const std::string config_error = config_.validate();
  if (!config_error.empty()) {
    return Outcome::make(OutcomeCode::MalformedRequest, config_error);
  }
  if (!detail::net_available()) {
    return Outcome::make(OutcomeCode::UnsupportedCapability,
                         "the distributed transport is unavailable on this platform");
  }
  std::string error;
  std::uint16_t bound_port = 0;
  const detail::socket_handle listener =
      detail::net_listen(config_.bind_address, config_.port, error, bound_port);
  if (listener == detail::invalid_socket) {
    return Outcome::make(OutcomeCode::TransportFailure, error);
  }
  stop_handle_ = detail::net_create_stop_handle();
  if (stop_handle_ == nullptr) {
    detail::net_close(listener);
    return Outcome::make(OutcomeCode::TransportFailure, "the stop handle could not be created");
  }
  listener_ = from_socket(listener);
  port_ = bound_port;
  stopping_ = false;
  running_ = true;
  accept_thread_ = std::thread([this] { accept_loop(); });
  std::string detail_text = "listening on ";
  detail_text += config_.bind_address;
  detail_text.push_back(':');
  detail_text += detail::format_u64(port_);
  return Outcome::make(OutcomeCode::Committed, detail_text);
}

Outcome CoordinatorServer::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return Outcome::make(OutcomeCode::Idempotent, "the coordinator is not listening");
    }
    stopping_ = true;
  }
  detail::net_signal_stop(stop_handle_);
  if (listener_ != nullptr) {
    detail::net_close(to_socket(listener_));
    listener_ = nullptr;
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  std::vector<std::shared_ptr<Session>> sessions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions = sessions_;
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    detail::socket_handle handle = detail::invalid_socket;
    {
      std::lock_guard<std::mutex> session_lock(session->mutex);
      handle = session->socket;
    }
    bool expected = false;
    if (handle != detail::invalid_socket &&
        session->closed.compare_exchange_strong(expected, true)) {
      detail::net_shutdown(handle);
      detail::net_close(handle);
    }
  }
  for (const std::shared_ptr<Session>& session : sessions) {
    if (session->thread.joinable()) {
      session->thread.join();
    }
    if (config_.fence_on_shutdown && !session->registrations.empty()) {
      std::size_t fenced = 0;
      for (const auto& registration : session->registrations) {
        InvalidatePublisherRequest request;
        request.publisher = registration.first;
        request.worker_boot = registration.second;
        request.epoch = engine_.coordinator_epoch();
        request.reason = "coordinator shutdown";
        const LinkMutationResult result = engine_.invalidate_publisher(request);
        if (!result.outcome.is_error()) {
          ++fences_applied_;
          ++fenced;
        }
      }
      if (config_.session_observer && fenced > 0) {
        SessionEvent event;
        event.kind = SessionEvent::Kind::Closed;
        event.agent = session->agent;
        event.registrations = session->registrations.size();
        event.fenced = fenced;
        const auto& registration = session->registrations.front();
        event.publisher = registration.first;
        event.worker_boot = registration.second;
        config_.session_observer(event);
      }
      session->registrations.clear();
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
  }
  detail::net_destroy_stop_handle(stop_handle_);
  stop_handle_ = nullptr;
  port_ = 0;
  running_ = false;
  return Outcome::make(OutcomeCode::Committed, "the coordinator stopped");
}

bool CoordinatorServer::running() const { return running_; }

std::uint16_t CoordinatorServer::port() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return port_;
}

std::string CoordinatorServer::endpoint() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!running_ || port_ == 0) {
    return std::string();
  }
  std::string text = config_.bind_address;
  text.push_back(':');
  text += detail::format_u64(port_);
  return text;
}

std::size_t CoordinatorServer::session_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t count = 0;
  for (const std::shared_ptr<Session>& session : sessions_) {
    if (!session->finished) {
      ++count;
    }
  }
  return count;
}

std::uint64_t CoordinatorServer::sessions_accepted() const { return sessions_accepted_; }
std::uint64_t CoordinatorServer::frames_received() const { return frames_received_; }
std::uint64_t CoordinatorServer::frames_rejected() const { return frames_rejected_; }
std::uint64_t CoordinatorServer::requests_handled() const { return requests_handled_; }
std::uint64_t CoordinatorServer::fences_applied() const { return fences_applied_; }

void CoordinatorServer::accept_loop() {
  const detail::socket_handle listener = to_socket(listener_);
  while (!stopping_) {
    std::vector<std::shared_ptr<Session>> finished;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = sessions_.begin(); it != sessions_.end();) {
        if ((*it)->finished) {
          finished.push_back(*it);
          it = sessions_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (const std::shared_ptr<Session>& session : finished) {
      if (session->thread.joinable()) {
        session->thread.join();
      }
    }

    bool stopped = false;
    std::string error;
    const detail::socket_handle client = detail::net_accept(listener, stop_handle_, stopped, error);
    if (stopped || stopping_) {
      break;
    }
    if (client == detail::invalid_socket) {
      break;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (sessions_.size() >= config_.max_sessions) {
        detail::net_shutdown(client);
        detail::net_close(client);
        continue;
      }
    }
    std::shared_ptr<Session> session = std::make_shared<Session>();
    session->socket = client;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sessions_.push_back(session);
    }
    ++sessions_accepted_;
    session->thread = std::thread([this, session] { serve(session); });
  }
}

void CoordinatorServer::fail_protocol(const std::shared_ptr<Session>& session,
                                      const std::string& detail_text) {
  Message message;
  message.type = MessageType::Error;
  message.result.code = OutcomeCode::MalformedRequest;
  message.result.detail = detail_text;
  std::string error;
  const std::vector<std::byte> payload = encode_message(message, error);
  if (!error.empty()) {
    return;
  }
  const std::vector<std::byte> frame = encode_frame(MessageType::Error, 0, payload);
  (void)detail::net_send_all(session->socket, frame, error);
}

bool CoordinatorServer::handle_frame(const std::shared_ptr<Session>& session, const Frame& frame,
                                     std::vector<std::byte>& response) {
  Message request;
  std::string error;
  if (!decode_message(frame.type, frame.payload, request, error)) {
    ++frames_rejected_;
    Message message;
    message.type = MessageType::Error;
    message.result.code = OutcomeCode::MalformedRequest;
    message.result.detail = error;
    const std::vector<std::byte> payload = encode_message(message, error);
    if (error.empty()) {
      response = encode_frame(MessageType::Error, frame.request_id, payload);
    }
    return false;
  }
  ++requests_handled_;

  Message reply;
  bool keep_open = true;
  switch (frame.type) {
    case MessageType::Hello: {
      session->agent = request.hello.agent;
      reply.type = MessageType::HelloAck;
      reply.hello_ack.version = protocol_version;
      reply.hello_ack.epoch = engine_.coordinator_epoch();
      reply.hello_ack.coordinator = config_.coordinator_name;
      reply.hello_ack.link_count = engine_.link_count();
      break;
    }
    case MessageType::RegisterPublisher: {
      RegisterPublisherRequest registration;
      registration.registration.publisher = request.registration.publisher;
      registration.registration.worker_boot = request.registration.worker_boot;
      registration.registration.source = request.registration.source;
      registration.registration.scope.fabric = request.registration.fabric;
      registration.registration.scope.links = request.registration.links;
      registration.registration.epoch = request.registration.epoch;
      const LinkMutationResult result = engine_.register_publisher(registration);
      checkpoint(engine_, config_, result.outcome);
      if (!result.outcome.is_error()) {
        bool known = false;
        for (const auto& entry : session->registrations) {
          if (entry.first == request.registration.publisher &&
              entry.second == request.registration.worker_boot) {
            known = true;
            break;
          }
        }
        if (!known) {
          session->registrations.emplace_back(request.registration.publisher,
                                              request.registration.worker_boot);
        }
      }
      reply.type = MessageType::RegisterResult;
      fill_result(reply.result, result.outcome, nullptr);
      break;
    }
    case MessageType::PublishEvidence: {
      PublishEvidenceRequest publication;
      publication.publication = request.publish.publication;
      publication.observation = request.publish.observation;
      if (request.publish.has_expected_evidence_generation) {
        publication.expected_evidence_generation = request.publish.expected_evidence_generation;
      }
      if (request.publish.has_expected_state_generation) {
        publication.expected_state_generation = request.publish.expected_state_generation;
      }
      const LinkMutationResult result = engine_.publish_evidence(publication);
      checkpoint(engine_, config_, result.outcome);
      reply.type = MessageType::PublishResult;
      fill_result(reply.result, result.outcome,
                  result.view.has_value() ? &result.view.value() : nullptr);
      break;
    }
    case MessageType::WithdrawEvidence: {
      WithdrawEvidenceRequest withdrawal;
      withdrawal.key = request.withdraw.key;
      withdrawal.observation = request.withdraw.observation;
      withdrawal.publisher = request.withdraw.publisher;
      withdrawal.worker_boot = request.withdraw.worker_boot;
      withdrawal.epoch = request.withdraw.epoch;
      withdrawal.source_generation = request.withdraw.source_generation;
      withdrawal.sequence = request.withdraw.sequence;
      withdrawal.reason = request.withdraw.reason;
      const LinkMutationResult result = engine_.withdraw_evidence(withdrawal);
      checkpoint(engine_, config_, result.outcome);
      reply.type = MessageType::WithdrawResult;
      fill_result(reply.result, result.outcome,
                  result.view.has_value() ? &result.view.value() : nullptr);
      break;
    }
    case MessageType::InvalidatePublisher: {
      bool authorized = false;
      for (const auto& entry : session->registrations) {
        if (entry.first == request.invalidate.publisher) {
          authorized = true;
          break;
        }
      }
      if (!authorized) {
        reply.type = MessageType::InvalidateResult;
        reply.result.code = OutcomeCode::UnauthorizedScope;
        reply.result.detail =
            "a session may only invalidate publisher incarnations it registered";
        break;
      }
      InvalidatePublisherRequest invalidation;
      invalidation.publisher = request.invalidate.publisher;
      invalidation.worker_boot = request.invalidate.worker_boot;
      invalidation.epoch = request.invalidate.epoch;
      invalidation.reason = request.invalidate.reason;
      const LinkMutationResult result = engine_.invalidate_publisher(invalidation);
      checkpoint(engine_, config_, result.outcome);
      if (!result.outcome.is_error()) {
        ++fences_applied_;
      }
      reply.type = MessageType::InvalidateResult;
      fill_result(reply.result, result.outcome, nullptr);
      break;
    }
    case MessageType::QueryState: {
      reply.type = MessageType::QueryResult;
      const std::optional<LinkStateView> view = engine_.link_state(request.query.link);
      if (view.has_value()) {
        const Outcome outcome = Outcome::make(OutcomeCode::Committed, "link state");
        fill_result(reply.result, outcome, &view.value());
      } else {
        reply.result.code = OutcomeCode::UnknownLink;
        reply.result.detail = "link is not bound in this engine";
        reply.result.found = false;
      }
      break;
    }
    case MessageType::Shutdown: {
      reply.type = MessageType::Shutdown;
      reply.result.code = OutcomeCode::Committed;
      reply.result.detail = "session shutdown acknowledged";
      keep_open = false;
      break;
    }
    case MessageType::HelloAck:
    case MessageType::RegisterResult:
    case MessageType::PublishResult:
    case MessageType::WithdrawResult:
    case MessageType::InvalidateResult:
    case MessageType::QueryResult:
    case MessageType::Error:
    default: {
      ++frames_rejected_;
      reply.type = MessageType::Error;
      reply.result.code = OutcomeCode::MalformedRequest;
      reply.result.detail = "a worker session may only send request messages";
      break;
    }
  }

  const std::vector<std::byte> payload = encode_message(reply, error);
  if (!error.empty()) {
    return false;
  }
  response = encode_frame(reply.type, frame.request_id, payload);
  return keep_open;
}

void CoordinatorServer::serve(const std::shared_ptr<Session>& session) {
  std::vector<std::byte> buffer;
  std::vector<std::byte> chunk(8192);
  const std::size_t buffered_limit =
      limits::max_frame_payload_bytes + frame_header_bytes + frame_integrity_bytes;
  while (!stopping_) {
    std::string error;
    const int received = detail::net_receive(session->socket, chunk.data(), chunk.size(), error);
    if (received <= 0) {
      break;
    }
    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + received);
    bool close_session = false;
    while (!close_session) {
      const FrameDecodeResult decoded =
          decode_frame(std::span<const std::byte>(buffer.data(), buffer.size()));
      if (decoded.status == FrameDecodeStatus::Incomplete) {
        break;
      }
      if (decoded.status != FrameDecodeStatus::Complete) {
        ++frames_rejected_;
        fail_protocol(session, std::string("protocol violation: ") + to_string(decoded.status) +
                                   " (" + decoded.detail + ")");
        close_session = true;
        break;
      }
      buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
      ++frames_received_;
      std::vector<std::byte> response;
      const bool keep_open = handle_frame(session, decoded.frame, response);
      if (!response.empty()) {
        if (!detail::net_send_all(session->socket, response, error)) {
          close_session = true;
          break;
        }
      }
      if (!keep_open) {
        close_session = true;
        break;
      }
    }
    if (close_session) {
      break;
    }
    if (buffer.size() > buffered_limit) {
      ++frames_rejected_;
      fail_protocol(session, "buffered data exceeds the maximum frame size");
      break;
    }
  }
  end_session(session);
}

void CoordinatorServer::end_session(const std::shared_ptr<Session>& session) {
  detail::socket_handle handle = detail::invalid_socket;
  {
    std::lock_guard<std::mutex> lock(session->mutex);
    handle = session->socket;
  }
  bool expected = false;
  if (handle != detail::invalid_socket && session->closed.compare_exchange_strong(expected, true)) {
    detail::net_shutdown(handle);
    detail::net_close(handle);
  }

  SessionEvent event;
  event.kind = SessionEvent::Kind::Closed;
  event.agent = session->agent;
  event.registrations = session->registrations.size();
  if (config_.fence_on_disconnect && !session->registrations.empty()) {
    for (const auto& registration : session->registrations) {
      InvalidatePublisherRequest request;
      request.publisher = registration.first;
      request.worker_boot = registration.second;
      request.epoch = engine_.coordinator_epoch();
      request.reason = "worker session ended";
      const LinkMutationResult result = engine_.invalidate_publisher(request);
      checkpoint(engine_, config_, result.outcome);
      if (!result.outcome.is_error()) {
        ++fences_applied_;
        ++event.fenced;
        event.publisher = registration.first;
        event.worker_boot = registration.second;
      }
    }
    session->registrations.clear();
  }
  session->finished = true;
  // The observer runs after the socket is closed and after every fencing call
  // returned: no server lock and no engine lock is held here.
  if (config_.session_observer) {
    config_.session_observer(event);
  }
}

}  // namespace linkstate
