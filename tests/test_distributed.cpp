#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "helpers.hpp"
#include "linkstate/client.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/protocol.hpp"
#include "linkstate/server.hpp"
#include "lsf_test.hpp"

using namespace linkstate;

namespace {

PublishEvidenceRequest client_request(const PublisherClientConfig& config,
                                      const CoordinatorEpoch& epoch, const LinkId& link,
                                      EvidenceClaim claim, std::uint64_t sequence,
                                      const std::string& tag,
                                      TopologyGeneration topology_generation =
                                          TopologyGeneration::from_value(1)) {
  PublishEvidenceRequest publication;
  publication.publication = PublicationId::from_validated("pub." + tag);
  EvidenceRecord& observation = publication.observation;
  observation.key.link = link;
  observation.key.source = config.source;
  observation.key.kind = EvidenceKind::SyntheticTest;
  observation.observation = ObservationId::from_validated("obs." + tag);
  observation.publisher = config.publisher;
  observation.worker_boot = config.worker_boot;
  observation.epoch = epoch;
  observation.source_generation = SourceGeneration::from_value(1);
  observation.sequence = sequence;
  observation.topology_generation = topology_generation;
  observation.evidence_class = EvidenceClass::Synthetic;
  observation.claim = claim;
  return publication;
}

PublisherClientConfig client_config(const std::string& address, std::uint16_t port,
                                    const std::string& publisher, const std::string& boot,
                                    const std::string& source,
                                    const std::vector<LinkId>& links) {
  PublisherClientConfig config;
  config.address = address;
  config.port = port;
  config.publisher = lsf_test::publisher_id(publisher);
  config.worker_boot = lsf_test::boot_id(boot);
  config.source = lsf_test::source_id(source);
  config.links = links;
  config.agent_name = "test-agent";
  return config;
}

std::vector<LinkId> bind_links(LinkStateEngine& engine, const std::vector<std::string>& names) {
  std::vector<LinkId> links;
  for (const std::string& name : names) {
    LSF_REQUIRE(engine.bind_link(BindLinkRequest{lsf_test::binding_for(name)}).outcome.code ==
                OutcomeCode::Committed);
    links.push_back(lsf_test::link_id(name));
  }
  return links;
}

}  // namespace

LSF_TEST(frame_codec_round_trips_and_rejects_malformed_frames) {
  const std::vector<std::byte> payload = {std::byte{1}, std::byte{2}, std::byte{3}};
  const std::vector<std::byte> frame =
      encode_frame(MessageType::QueryState, 42, std::span<const std::byte>(payload));
  LSF_CHECK_EQ(frame.size(),
               frame_header_bytes + payload.size() + frame_integrity_bytes);
  const FrameDecodeResult decoded = decode_frame(std::span<const std::byte>(frame));
  LSF_REQUIRE(decoded.status == FrameDecodeStatus::Complete);
  LSF_CHECK(decoded.frame.type == MessageType::QueryState);
  LSF_CHECK(decoded.frame.request_id == 42);
  LSF_CHECK(decoded.frame.payload == payload);
  LSF_CHECK(decoded.consumed == frame.size());

  for (std::size_t length = 0; length < frame.size(); ++length) {
    const FrameDecodeResult partial =
        decode_frame(std::span<const std::byte>(frame.data(), length));
    LSF_CHECK(partial.status == FrameDecodeStatus::Incomplete);
  }

  std::vector<std::byte> bad_magic = frame;
  bad_magic[0] = std::byte{0x00};
  LSF_CHECK(decode_frame(std::span<const std::byte>(bad_magic)).status ==
            FrameDecodeStatus::Malformed);

  std::vector<std::byte> bad_version = frame;
  bad_version[4] = std::byte{0x7f};
  LSF_CHECK(decode_frame(std::span<const std::byte>(bad_version)).status ==
            FrameDecodeStatus::UnsupportedVersion);

  std::vector<std::byte> bad_type = frame;
  bad_type[6] = std::byte{0xff};
  LSF_CHECK(decode_frame(std::span<const std::byte>(bad_type)).status ==
            FrameDecodeStatus::Malformed);

  std::vector<std::byte> bad_reserved = frame;
  bad_reserved[20] = std::byte{0x01};
  LSF_CHECK(decode_frame(std::span<const std::byte>(bad_reserved)).status ==
            FrameDecodeStatus::Malformed);

  std::vector<std::byte> bad_integrity = frame;
  bad_integrity[frame_header_bytes] = std::byte{0x5a};
  LSF_CHECK(decode_frame(std::span<const std::byte>(bad_integrity)).status ==
            FrameDecodeStatus::Malformed);

  std::vector<std::byte> oversized(frame_header_bytes + frame_integrity_bytes, std::byte{0});
  oversized[0] = std::byte{'L'};
  oversized[1] = std::byte{'S'};
  oversized[2] = std::byte{'F'};
  oversized[3] = std::byte{'P'};
  oversized[4] = std::byte{protocol_version};
  oversized[6] = std::byte{static_cast<unsigned char>(MessageType::Hello)};
  const std::uint32_t huge = static_cast<std::uint32_t>(limits::max_frame_payload_bytes) + 1u;
  for (unsigned index = 0; index < 4; ++index) {
    oversized[16 + index] = static_cast<std::byte>((huge >> (8u * index)) & 0xffu);
  }
  LSF_CHECK(decode_frame(std::span<const std::byte>(oversized)).status ==
            FrameDecodeStatus::TooLarge);
}

LSF_TEST(message_codec_round_trips_every_request_and_result) {
  std::string error;

  Message publish;
  publish.type = MessageType::PublishEvidence;
  publish.publish.publication = PublicationId::from_validated("pub.codec");
  publish.publish.observation.key.link = lsf_test::link_id("codec");
  publish.publish.observation.key.source = lsf_test::source_id("codec-source");
  publish.publish.observation.key.kind = EvidenceKind::RemoteEndpointState;
  publish.publish.observation.key.direction = EvidenceDirection::Receive;
  publish.publish.observation.observation = ObservationId::from_validated("obs.codec");
  publish.publish.observation.publisher = lsf_test::publisher_id("codec-publisher");
  publish.publish.observation.worker_boot = lsf_test::boot_id("codec-boot");
  publish.publish.observation.epoch = CoordinatorEpoch::from_value(3);
  publish.publish.observation.source_generation = SourceGeneration::from_value(2);
  publish.publish.observation.sequence = 9;
  publish.publish.observation.topology_generation = TopologyGeneration::from_value(4);
  publish.publish.observation.endpoint_generation = EndpointGeneration::from_value(5);
  publish.publish.observation.evidence_class = EvidenceClass::Synthetic;
  publish.publish.observation.claim = EvidenceClaim::Degraded;
  publish.publish.observation.degradation_cause = DegradationCause::PartialLaneFailure;
  publish.publish.observation.detail = "codec detail";
  publish.publish.has_expected_state_generation = true;
  publish.publish.expected_state_generation = LinkStateGeneration::from_value(7);
  const std::vector<std::byte> publish_payload = encode_message(publish, error);
  LSF_REQUIRE(error.empty());
  Message decoded_publish;
  LSF_REQUIRE(decode_message(MessageType::PublishEvidence,
                             std::span<const std::byte>(publish_payload), decoded_publish, error));
  LSF_CHECK(decoded_publish.publish.observation == publish.publish.observation);
  LSF_CHECK(decoded_publish.publish.publication == publish.publish.publication);
  LSF_CHECK(decoded_publish.publish.has_expected_state_generation);

  // Trailing bytes are a protocol violation.
  std::vector<std::byte> trailing = publish_payload;
  trailing.push_back(std::byte{0});
  Message ignored;
  LSF_CHECK(!decode_message(MessageType::PublishEvidence,
                            std::span<const std::byte>(trailing), ignored, error));

  // An out of range enumeration is rejected.
  const std::vector<std::byte> short_payload(4, std::byte{0xff});
  LSF_CHECK(!decode_message(MessageType::QueryState,
                            std::span<const std::byte>(short_payload), ignored, error));

  Message registration;
  registration.type = MessageType::RegisterPublisher;
  registration.registration.publisher = lsf_test::publisher_id("codec-publisher");
  registration.registration.worker_boot = lsf_test::boot_id("codec-boot");
  registration.registration.source = lsf_test::source_id("codec-source");
  registration.registration.fabric = lsf_test::fabric_id("codec");
  registration.registration.epoch = CoordinatorEpoch::from_value(1);
  registration.registration.links = {lsf_test::link_id("codec"), lsf_test::link_id("codec2")};
  const std::vector<std::byte> registration_payload = encode_message(registration, error);
  LSF_REQUIRE(error.empty());
  Message decoded_registration;
  LSF_REQUIRE(decode_message(MessageType::RegisterPublisher,
                             std::span<const std::byte>(registration_payload),
                             decoded_registration, error));
  LSF_CHECK(decoded_registration.registration.links == registration.registration.links);
  LSF_CHECK(decoded_registration.registration.epoch == registration.registration.epoch);

  Message result;
  result.type = MessageType::PublishResult;
  result.result.code = OutcomeCode::StaleTopology;
  result.result.detail = "stale";
  result.result.state = LinkOperationalState::Degraded;
  result.result.state_generation = LinkStateGeneration::from_value(11);
  result.result.global_generation = GlobalGeneration::from_value(12);
  result.result.found = true;
  const std::vector<std::byte> result_payload = encode_message(result, error);
  LSF_REQUIRE(error.empty());
  Message decoded_result;
  LSF_REQUIRE(decode_message(MessageType::PublishResult,
                             std::span<const std::byte>(result_payload), decoded_result, error));
  LSF_CHECK(decoded_result.result.code == OutcomeCode::StaleTopology);
  LSF_CHECK(decoded_result.result.state == LinkOperationalState::Degraded);
  LSF_CHECK(decoded_result.result.state_generation.value() == 11);
}

LSF_TEST(coordinator_serves_real_frames_over_loopback) {
  LinkStateEngine engine;
  CoordinatorConfig config;
  config.port = 0;
  config.coordinator_name = "loopback-coordinator";
  CoordinatorServer server(engine, config);
  LSF_REQUIRE(server.start().code == OutcomeCode::Committed);
  LSF_CHECK(server.running());
  LSF_CHECK(server.port() != 0);
  LSF_CHECK(server.endpoint().find(':') != std::string::npos);

  const std::vector<LinkId> links = bind_links(engine, {"net.one", "net.two"});
  const PublisherClientConfig first_config =
      client_config(config.bind_address, server.port(), "net-agent-a", "net-boot-a",
                    "net-source-a", links);
  PublisherClient first(first_config);
  const Outcome connected = first.connect();
  if (connected.is_error()) {
    std::printf("  connect failed: %s / %s\n", connected.to_string().c_str(),
                first.last_error().c_str());
    std::fflush(stdout);
  }
  LSF_REQUIRE(connected.code == OutcomeCode::Committed);
  LSF_REQUIRE(first.epoch() == engine.coordinator_epoch());
  const Outcome registered = first.register_publisher();
  if (registered.is_error()) {
    std::printf("  register failed: %s / %s\n", registered.to_string().c_str(),
                first.last_error().c_str());
    std::fflush(stdout);
  }
  LSF_REQUIRE(registered.code == OutcomeCode::Committed);

  // A worker learns the structural generation from the coordinator and binds
  // its evidence to it.
  LinkStateView view;
  LSF_REQUIRE(first.query_state(links[0], view));
  LSF_CHECK(view.state() == LinkOperationalState::Unknown);
  LSF_CHECK(view.record.binding.topology_generation.value() == 1);
  LSF_CHECK(view.record.binding.endpoints.local_generation.value() == 1);
  const TopologyGeneration topology = view.record.binding.topology_generation;
  const LinkMutationResult direct = engine.publish_evidence(client_request(
      first_config, first.epoch(), links[0], EvidenceClaim::Up, 1, "net.a", topology));
  LSF_REQUIRE(direct.outcome.code == OutcomeCode::Committed);

  LSF_CHECK(first.query_state(links[0], view));
  LSF_CHECK(view.state() == LinkOperationalState::Up);

  LinkStateView published_view;
  LSF_REQUIRE(first
                  .publish(client_request(first_config, first.epoch(), links[1],
                                          EvidenceClaim::Up, 1, "net.b", topology),
                           &published_view)
                  .code == OutcomeCode::Committed);
  LSF_CHECK(published_view.state() == LinkOperationalState::Up);
  LSF_CHECK(engine.link_state(links[1])->state() == LinkOperationalState::Up);

  // A worker that vanishes without a graceful shutdown is fenced through the
  // real transport: the session closes, the publisher is invalidated and the
  // link that depended only on it requires revalidation.
  const std::vector<EvidenceRecord> records = engine.evidence_for_link(links[1]);
  LSF_REQUIRE(records.size() == 1);
  WithdrawEvidenceRequest withdrawal;
  withdrawal.key = records.front().key;
  withdrawal.observation = records.front().observation;
  withdrawal.publisher = first_config.publisher;
  withdrawal.worker_boot = first_config.worker_boot;
  withdrawal.epoch = first.epoch();
  withdrawal.source_generation = records.front().source_generation;
  withdrawal.sequence = records.front().sequence + 1u;
  LSF_CHECK(first.withdraw(withdrawal).code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links[1])->state() == LinkOperationalState::RevalidationRequired);

  first.abandon();
  LSF_CHECK(server.frames_received() >= 5);
  LSF_CHECK(server.frames_rejected() == 0);
  LSF_REQUIRE(server.stop().code == OutcomeCode::Committed);
  LSF_CHECK(!server.running());
  LSF_CHECK(server.stop().code == OutcomeCode::Idempotent);
  LSF_CHECK(server.endpoint().empty());
}

LSF_TEST(server_start_stop_cycles_leave_no_session_behind) {
  LinkStateEngine engine;
  const std::vector<LinkId> links = bind_links(engine, {"cycle.one"});
  for (int cycle = 0; cycle < 5; ++cycle) {
    CoordinatorConfig config;
    config.port = 0;
    CoordinatorServer server(engine, config);
    LSF_REQUIRE(server.start().code == OutcomeCode::Committed);
    // Every worker restart uses a fresh boot identity, which is what a real
    // process restart produces.
    PublisherClientConfig client =
        client_config(config.bind_address, server.port(), "cycle-agent",
                      "cycle-boot-" + std::to_string(cycle), "cycle-source", links);
    PublisherClient publisher(client);
    LSF_REQUIRE(publisher.connect().code == OutcomeCode::Committed);
    LSF_REQUIRE(publisher.register_publisher().code == OutcomeCode::Committed);
    LSF_REQUIRE(publisher
                    .publish(client_request(client, publisher.epoch(), links[0], EvidenceClaim::Up,
                                            1, "cycle." + std::to_string(cycle)))
                    .code == OutcomeCode::Committed);
    LSF_REQUIRE(publisher.disconnect().code == OutcomeCode::Committed);
    LSF_REQUIRE(server.stop().code == OutcomeCode::Committed);
    LSF_CHECK_EQ(server.session_count(), std::size_t{0});
  }
  LSF_CHECK(engine.link_state(links[0])->state() == LinkOperationalState::RevalidationRequired);
  std::size_t session_fences = 0;
  for (const PublisherFence& fence : engine.fences()) {
    if (fence.reason.find("worker session ended") != std::string::npos) {
      ++session_fences;
    }
  }
  LSF_CHECK_EQ(session_fences, std::size_t{5});

  // A fenced incarnation may re-register but must never regain the ability to
  // publish: boot authority is permanently stale once fenced.
  CoordinatorConfig config;
  config.port = 0;
  CoordinatorServer server(engine, config);
  LSF_REQUIRE(server.start().code == OutcomeCode::Committed);
  const PublisherClientConfig stale =
      client_config(config.bind_address, server.port(), "cycle-agent", "cycle-boot-0",
                    "cycle-source", links);
  PublisherClient stale_client(stale);
  LSF_REQUIRE(stale_client.connect().code == OutcomeCode::Committed);
  // Fencing is permanent: the fenced incarnation may not even re-register, let
  // alone publish again.
  const Outcome replayed_registration = stale_client.register_publisher();
  if (replayed_registration.code != OutcomeCode::StaleAuthority) {
    std::printf("  fenced re-registration produced: %s\n",
                replayed_registration.to_string().c_str());
    std::fflush(stdout);
  }
  LSF_CHECK(replayed_registration.code == OutcomeCode::StaleAuthority);
  const Outcome replayed =
      stale_client.publish(client_request(stale, stale_client.epoch(), links[0], EvidenceClaim::Up,
                                          9, "cycle.replay"));
  LSF_CHECK(replayed.code == OutcomeCode::StaleAuthority);
  stale_client.abandon();
  LSF_REQUIRE(server.stop().code == OutcomeCode::Committed);
}

LSF_TEST(protocol_violation_closes_the_session_without_harming_the_listener) {
  LinkStateEngine engine;
  const std::vector<LinkId> links = bind_links(engine, {"violation.one"});
  CoordinatorConfig config;
  config.port = 0;
  CoordinatorServer server(engine, config);
  LSF_REQUIRE(server.start().code == OutcomeCode::Committed);
  const std::uint16_t port = server.port();

#ifdef _WIN32
  WSADATA data;
  LSF_REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
  const SOCKET raw = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  LSF_REQUIRE(raw != INVALID_SOCKET);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  LSF_REQUIRE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  LSF_REQUIRE(::connect(raw, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

  // A well formed header with a broken integrity field must be rejected.
  std::vector<std::byte> broken =
      encode_frame(MessageType::Hello, 1, std::span<const std::byte>());
  broken[frame_header_bytes] = std::byte{0x00};
  broken[frame_header_bytes + 1] = std::byte{0x00};
  LSF_REQUIRE(::send(raw, reinterpret_cast<const char*>(broken.data()),
                     static_cast<int>(broken.size()), 0) ==
              static_cast<int>(broken.size()));

  // The coordinator answers with an error frame and closes the session; the
  // blocking receive returns only after that response arrives.
  std::vector<std::byte> response(4096);
  const int received = ::recv(raw, reinterpret_cast<char*>(response.data()),
                              static_cast<int>(response.size()), 0);
  LSF_CHECK(received > 0);
  if (received > 0) {
    const FrameDecodeResult decoded = decode_frame(
        std::span<const std::byte>(response.data(), static_cast<std::size_t>(received)));
    LSF_REQUIRE(decoded.status == FrameDecodeStatus::Complete);
    LSF_CHECK(decoded.frame.type == MessageType::Error);
    Message message;
    std::string error;
    LSF_CHECK(decode_message(MessageType::Error, decoded.frame.payload, message, error));
    LSF_CHECK(message.result.code == OutcomeCode::MalformedRequest);
  }
  ::closesocket(raw);
  WSACleanup();
#endif

  LSF_CHECK(server.frames_rejected() >= 1);
  // The listener is unaffected: a well behaved worker can still connect.
  const PublisherClientConfig fresh = client_config(config.bind_address, port, "violation-agent",
                                                    "violation-boot", "violation-source", links);
  PublisherClient publisher(fresh);
  LSF_REQUIRE(publisher.connect().code == OutcomeCode::Committed);
  LSF_REQUIRE(publisher.register_publisher().code == OutcomeCode::Committed);
  LSF_REQUIRE(publisher
                  .publish(client_request(fresh, publisher.epoch(), links[0], EvidenceClaim::Up, 1,
                                          "violation.one"))
                  .code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(links[0])->state() == LinkOperationalState::Up);
  publisher.abandon();
  LSF_REQUIRE(server.stop().code == OutcomeCode::Committed);
}
