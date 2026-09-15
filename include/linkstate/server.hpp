#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/export.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/protocol.hpp"

namespace linkstate {

/// Session lifecycle event reported to an optional observer.
struct LSF_EXPORT SessionEvent {
  enum class Kind : std::uint8_t {
    /// A session accepted a connection.
    Accepted = 0,
    /// A session ended and its publisher incarnations were evaluated.
    Closed,
  };

  Kind kind = Kind::Accepted;
  std::string agent;
  std::size_t registrations = 0;
  std::size_t fenced = 0;
  PublisherId publisher;
  WorkerBootId worker_boot;
};

LSF_EXPORT const char* to_string(SessionEvent::Kind kind) noexcept;

/// Coordinator configuration.
struct LSF_EXPORT CoordinatorConfig {
  /// Loopback address the coordinator binds. Binding a non loopback address is
  /// the caller's explicit decision.
  std::string bind_address = "127.0.0.1";
  /// Listening port. Zero selects an ephemeral port reported by port().
  std::uint16_t port = 0;
  /// Maximum number of concurrent worker sessions.
  std::size_t max_sessions = limits::max_sessions;
  /// Name reported in the hello handshake.
  std::string coordinator_name = "link-state-coordinator";
  /// Fence the publisher incarnations registered on a session when the session
  /// ends, whether the end was graceful or abrupt. This is how real worker
  /// death is detected: the connection closes.
  bool fence_on_disconnect = true;
  /// Invalidate the publisher incarnations of every session when the
  /// coordinator stops. A coordinator shutdown is not a worker death, so this
  /// defaults to false: the caller decides.
  bool fence_on_shutdown = false;

  /// Journal file written after every request that changed durable state.
  ///
  /// An empty path disables durable writes. Writing synchronously after each
  /// accepted mutation makes durable structure and generation counters crash
  /// consistent without any wall clock checkpoint timer; the cost is one
  /// journal write per accepted request, which the caller controls by choosing
  /// whether to configure a journal at all.
  std::string journal_path;
  /// Include per-link state history in the journal.
  bool journal_history = false;

  /// Optional session lifecycle observer.
  ///
  /// The observer is invoked from the session's own thread after the socket has
  /// been closed and after any publisher fencing has completed. It is never
  /// invoked while the server holds an internal lock and never while the engine
  /// lock is held, so it may call back into the engine or the server safely.
  std::function<void(const SessionEvent&)> session_observer;

  std::string validate() const;
};

/// Coordinator side of the distributed publication protocol.
///
/// Thread safety: start, stop, port, endpoint and the counters are safe to call
/// from any thread. The server owns its accept thread and one thread per
/// session. It holds no engine lock while performing transport work, and it
/// holds no transport lock while calling into the engine, so the engine and the
/// transport can never deadlock against each other.
class LSF_EXPORT CoordinatorServer {
 public:
  CoordinatorServer(LinkStateEngine& engine, CoordinatorConfig config = {});
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  /// Binds, listens and starts accepting sessions.
  Outcome start();

  /// Stops accepting, closes every session and joins every thread. Idempotent.
  Outcome stop();

  bool running() const;

  /// The bound port, or zero when the server is not listening.
  std::uint16_t port() const;

  /// "address:port" of the listening endpoint, empty when not listening.
  std::string endpoint() const;

  std::size_t session_count() const;

  std::uint64_t sessions_accepted() const;
  std::uint64_t frames_received() const;
  std::uint64_t frames_rejected() const;
  std::uint64_t requests_handled() const;
  std::uint64_t fences_applied() const;

 private:
  struct Session;

  void accept_loop();
  void serve(const std::shared_ptr<Session>& session);
  void end_session(const std::shared_ptr<Session>& session);
  bool handle_frame(const std::shared_ptr<Session>& session, const Frame& frame,
                    std::vector<std::byte>& response);
  void fail_protocol(const std::shared_ptr<Session>& session, const std::string& detail);

  LinkStateEngine& engine_;
  CoordinatorConfig config_;
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Session>> sessions_;
  std::thread accept_thread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> sessions_accepted_{0};
  std::atomic<std::uint64_t> frames_received_{0};
  std::atomic<std::uint64_t> frames_rejected_{0};
  std::atomic<std::uint64_t> requests_handled_{0};
  std::atomic<std::uint64_t> fences_applied_{0};
  std::uint16_t port_ = 0;
  void* listener_ = nullptr;
  void* stop_handle_ = nullptr;
};

}  // namespace linkstate
