#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/export.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// Whether a platform evidence source exists on this host.
enum class HostEvidenceAvailability : std::uint8_t {
  /// The source was queried and returned host visible interfaces.
  Available = 0,
  /// The host exposes no supported interface inventory.
  Unavailable,
  /// The capability is not present or not accessible on this platform.
  Unsupported,
};

LSF_EXPORT const char* to_string(HostEvidenceAvailability availability) noexcept;

/// One piece of real host visible link evidence.
///
/// A host observation describes exactly one endpoint: the local interface. It
/// never claims to describe the peer, the switch port or the fabric.
struct LSF_EXPORT HostLinkObservation {
  LinkId link;
  EndpointId endpoint;
  DeviceId device;
  PortId port;

  LinkClass link_class = LinkClass::HostNic;
  EvidenceKind kind = EvidenceKind::InterfaceOperationalState;
  EvidenceClaim claim = EvidenceClaim::Unknown;
  DegradationCause degradation_cause = DegradationCause::None;
  EvidenceClass evidence_class = EvidenceClass::Real;

  std::string interface_guid;
  std::string interface_alias;
  std::string description;
  std::string media_type;

  std::uint64_t transmit_speed_bps = 0;
  std::uint64_t receive_speed_bps = 0;
  std::uint32_t interface_index = 0;
  std::uint32_t interface_type = 0;

  bool media_connected = false;
  bool administratively_enabled = true;

  std::string detail;
};

/// Result of one host discovery round.
struct LSF_EXPORT HostDiscoveryReport {
  HostEvidenceAvailability availability = HostEvidenceAvailability::Unavailable;
  std::vector<HostLinkObservation> observations;
  std::vector<std::string> notes;
  std::size_t interfaces_enumerated = 0;
  std::size_t unsupported_interfaces = 0;
};

/// Deterministic rendering of one observation, one field per line.
LSF_EXPORT std::string render(const HostLinkObservation& observation);

/// Enumerates real host visible link evidence.
///
/// Windows: network interface operational status, media connect state,
/// negotiated link speeds, administrative status, interface GUID, hardware
/// description and physical medium type, taken from the operating system's
/// interface inventory. No switch side or fabric wide truth is inferred.
LSF_EXPORT HostDiscoveryReport discover_host_links();

/// Topology context needed to publish one host observation.
struct LSF_EXPORT HostBindingContext {
  TopologyGeneration topology_generation;
  EndpointGeneration endpoint_generation;
};

/// Converts successive discovery rounds into publishable evidence requests.
///
/// Each evidence key receives a strictly increasing observation sequence, and
/// each round advances the source generation, so a later round always
/// supersedes an earlier one.
class LSF_EXPORT HostEvidencePublisher {
 public:
  HostEvidencePublisher(PublisherId publisher, WorkerBootId worker_boot, SourceId source);

  /// Builds one request per observation whose link is present in bindings.
  std::vector<PublishEvidenceRequest> requests(
      const HostDiscoveryReport& report,
      const std::unordered_map<LinkId, HostBindingContext>& bindings, CoordinatorEpoch epoch);

  /// Number of discovery rounds converted so far.
  std::uint64_t round() const noexcept { return round_; }

  SourceGeneration source_generation() const noexcept { return source_generation_; }

 private:
  PublisherId publisher_;
  WorkerBootId worker_boot_;
  SourceId source_;
  SourceGeneration source_generation_ = SourceGeneration::from_value(1);
  std::uint64_t round_ = 0;
  std::unordered_map<std::string, std::uint64_t> sequences_;
  std::uint64_t observation_counter_ = 0;
};

}  // namespace linkstate
