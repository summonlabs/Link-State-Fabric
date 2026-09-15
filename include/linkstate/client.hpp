#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/export.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/protocol.hpp"

namespace linkstate {

/// Publisher client configuration.
struct LSF_EXPORT PublisherClientConfig {
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  PublisherId publisher;
  WorkerBootId worker_boot;
  SourceId source;
  FabricId fabric;
  /// Links this worker is authorized to observe.
  std::vector<LinkId> links;
  std::string agent_name = "link-state-worker";

  std::string validate() const;
};

/// Worker side of the distributed publication protocol.
///
/// The client performs synchronous request/response exchanges over one framed
/// TCP connection. It learns the coordinator epoch from the hello handshake, so
/// a worker that reconnects after a coordinator restart automatically presents
/// the new epoch instead of replaying the old one.
class LSF_EXPORT PublisherClient {
 public:
  explicit PublisherClient(PublisherClientConfig config);
  ~PublisherClient();

  PublisherClient(const PublisherClient&) = delete;
  PublisherClient& operator=(const PublisherClient&) = delete;

  /// Connects and performs the hello handshake.
  Outcome connect();

  /// Registers this publisher incarnation for the configured links.
  Outcome register_publisher();

  /// Sends a protocol shutdown and closes the connection.
  Outcome disconnect();

  /// Closes the socket without a protocol shutdown, modelling abrupt loss.
  void abandon();

  Outcome publish(const PublishEvidenceRequest& request, LinkStateView* view = nullptr);
  Outcome withdraw(const WithdrawEvidenceRequest& request, LinkStateView* view = nullptr);
  Outcome invalidate_publisher(const InvalidatePublisherRequest& request);
  bool query_state(const LinkId& link, LinkStateView& view);

  CoordinatorEpoch epoch() const;
  bool connected() const;
  const std::string& last_error() const;
  std::uint64_t requests_sent() const;
  std::uint64_t responses_received() const;

 private:
  Outcome transact(const Message& request, MessageType expected_response, Message& response,
                   LinkStateView* view);

  PublisherClientConfig config_;
  mutable std::mutex mutex_;
  void* socket_ = nullptr;
  bool connected_ = false;
  CoordinatorEpoch epoch_;
  std::uint64_t request_counter_ = 0;
  std::uint64_t requests_sent_ = 0;
  std::uint64_t responses_received_ = 0;
  std::string last_error_;
};

}  // namespace linkstate
