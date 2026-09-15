#include "linkstate/evidence.hpp"

#include <algorithm>

#include "text.hpp"

namespace linkstate {

const char* to_string(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::CarrierState:
      return "CARRIER_STATE";
    case EvidenceKind::InterfaceOperationalState:
      return "INTERFACE_OPERATIONAL_STATE";
    case EvidenceKind::DeviceReportedState:
      return "DEVICE_REPORTED_STATE";
    case EvidenceKind::PortReportedState:
      return "PORT_REPORTED_STATE";
    case EvidenceKind::RemoteEndpointState:
      return "REMOTE_ENDPOINT_STATE";
    case EvidenceKind::ControlPlaneHeartbeat:
      return "CONTROL_PLANE_HEARTBEAT";
    case EvidenceKind::HardwareErrorIndication:
      return "HARDWARE_ERROR_INDICATION";
    case EvidenceKind::LossErrorThreshold:
      return "LOSS_ERROR_THRESHOLD";
    case EvidenceKind::AdministrativeState:
      return "ADMINISTRATIVE_STATE";
    case EvidenceKind::SyntheticTest:
      return "SYNTHETIC_TEST";
    case EvidenceKind::PlatformDiscovery:
      return "PLATFORM_DISCOVERY";
  }
  return "CARRIER_STATE";
}

std::optional<EvidenceKind> parse_evidence_kind(std::string_view text) noexcept {
  for (std::size_t index = 0; index < 11; ++index) {
    const auto candidate = static_cast<EvidenceKind>(index);
    if (text == to_string(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::uint8_t evidence_precedence(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::AdministrativeState:
      return 1;
    case EvidenceKind::HardwareErrorIndication:
      return 2;
    case EvidenceKind::CarrierState:
    case EvidenceKind::InterfaceOperationalState:
    case EvidenceKind::DeviceReportedState:
    case EvidenceKind::PortReportedState:
    case EvidenceKind::ControlPlaneHeartbeat:
    case EvidenceKind::LossErrorThreshold:
    case EvidenceKind::PlatformDiscovery:
      return 3;
    case EvidenceKind::RemoteEndpointState:
      return 4;
    case EvidenceKind::SyntheticTest:
      return 5;
  }
  return 5;
}

const char* to_string(EvidenceClass evidence_class) noexcept {
  switch (evidence_class) {
    case EvidenceClass::Real:
      return "REAL";
    case EvidenceClass::Synthetic:
      return "SYNTHETIC";
    case EvidenceClass::Unsupported:
      return "UNSUPPORTED";
  }
  return "REAL";
}

const char* to_string(EvidenceDirection direction) noexcept {
  switch (direction) {
    case EvidenceDirection::Bidirectional:
      return "BIDIRECTIONAL";
    case EvidenceDirection::Transmit:
      return "TRANSMIT";
    case EvidenceDirection::Receive:
      return "RECEIVE";
  }
  return "BIDIRECTIONAL";
}

const char* to_string(EvidenceClaim claim) noexcept {
  switch (claim) {
    case EvidenceClaim::Unknown:
      return "UNKNOWN";
    case EvidenceClaim::Up:
      return "UP";
    case EvidenceClaim::Down:
      return "DOWN";
    case EvidenceClaim::Degraded:
      return "DEGRADED";
    case EvidenceClaim::AdminDisabled:
      return "ADMIN_DISABLED";
    case EvidenceClaim::Draining:
      return "DRAINING";
    case EvidenceClaim::Faulted:
      return "FAULTED";
  }
  return "UNKNOWN";
}

std::optional<LinkOperationalState> state_from_claim(EvidenceClaim claim) noexcept {
  switch (claim) {
    case EvidenceClaim::Unknown:
      return LinkOperationalState::Unknown;
    case EvidenceClaim::Up:
      return LinkOperationalState::Up;
    case EvidenceClaim::Down:
      return LinkOperationalState::Down;
    case EvidenceClaim::Degraded:
      return LinkOperationalState::Degraded;
    case EvidenceClaim::AdminDisabled:
      return LinkOperationalState::AdminDisabled;
    case EvidenceClaim::Draining:
      return LinkOperationalState::Draining;
    case EvidenceClaim::Faulted:
      return LinkOperationalState::Faulted;
  }
  return std::nullopt;
}

std::optional<EvidenceClaim> claim_from_state(LinkOperationalState state) noexcept {
  switch (state) {
    case LinkOperationalState::Unknown:
      return EvidenceClaim::Unknown;
    case LinkOperationalState::Up:
      return EvidenceClaim::Up;
    case LinkOperationalState::Down:
      return EvidenceClaim::Down;
    case LinkOperationalState::Degraded:
      return EvidenceClaim::Degraded;
    case LinkOperationalState::AdminDisabled:
      return EvidenceClaim::AdminDisabled;
    case LinkOperationalState::Draining:
      return EvidenceClaim::Draining;
    case LinkOperationalState::Faulted:
      return EvidenceClaim::Faulted;
    case LinkOperationalState::RevalidationRequired:
    case LinkOperationalState::Retired:
      return std::nullopt;
  }
  return std::nullopt;
}

const char* to_string(DegradationCause cause) noexcept {
  switch (cause) {
    case DegradationCause::None:
      return "NONE";
    case DegradationCause::ErrorRateThresholdExceeded:
      return "ERROR_RATE_THRESHOLD_EXCEEDED";
    case DegradationCause::ReducedNegotiatedSpeed:
      return "REDUCED_NEGOTIATED_SPEED";
    case DegradationCause::PartialLaneFailure:
      return "PARTIAL_LANE_FAILURE";
    case DegradationCause::UnstableCarrier:
      return "UNSTABLE_CARRIER";
    case DegradationCause::Asymmetry:
      return "ASYMMETRY";
    case DegradationCause::DegradedOpticalSignal:
      return "DEGRADED_OPTICAL_SIGNAL";
    case DegradationCause::RestrictedForwarding:
      return "RESTRICTED_FORWARDING";
    case DegradationCause::HardwareReportedImpairment:
      return "HARDWARE_REPORTED_IMPAIRMENT";
  }
  return "NONE";
}

const char* to_string(EvidenceStatus status) noexcept {
  switch (status) {
    case EvidenceStatus::Current:
      return "CURRENT";
    case EvidenceStatus::Superseded:
      return "SUPERSEDED";
    case EvidenceStatus::Withdrawn:
      return "WITHDRAWN";
    case EvidenceStatus::StaleAuthority:
      return "STALE_AUTHORITY";
    case EvidenceStatus::StaleTopology:
      return "STALE_TOPOLOGY";
    case EvidenceStatus::StaleSourceGeneration:
      return "STALE_SOURCE_GENERATION";
  }
  return "CURRENT";
}

bool operator<(const EvidenceKey& lhs, const EvidenceKey& rhs) noexcept {
  if (lhs.link != rhs.link) {
    return lhs.link < rhs.link;
  }
  if (lhs.source != rhs.source) {
    return lhs.source < rhs.source;
  }
  if (lhs.endpoint != rhs.endpoint) {
    return lhs.endpoint < rhs.endpoint;
  }
  if (lhs.direction != rhs.direction) {
    return static_cast<unsigned>(lhs.direction) < static_cast<unsigned>(rhs.direction);
  }
  return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
}

std::string EvidenceKey::to_string() const {
  std::string out;
  out += "link=";
  out += link.to_string();
  out += " source=";
  out += source.to_string();
  out += " endpoint=";
  out += endpoint.valid() ? endpoint.to_string() : std::string("<link>");
  out += " direction=";
  out += linkstate::to_string(direction);
  out += " kind=";
  out += linkstate::to_string(kind);
  return out;
}

bool operator==(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
  return lhs.key == rhs.key && lhs.observation == rhs.observation &&
         lhs.publisher == rhs.publisher && lhs.worker_boot == rhs.worker_boot &&
         lhs.epoch == rhs.epoch && lhs.source_generation == rhs.source_generation &&
         lhs.sequence == rhs.sequence && lhs.topology_generation == rhs.topology_generation &&
         lhs.endpoint_generation == rhs.endpoint_generation &&
         lhs.evidence_class == rhs.evidence_class && lhs.claim == rhs.claim &&
         lhs.degradation_cause == rhs.degradation_cause && lhs.detail == rhs.detail &&
         lhs.metadata == rhs.metadata;
}

Digest EvidenceRecord::compute_digest() const {
  Hasher hasher;
  hasher.update_text(key.link.value());
  hasher.update_text(key.source.value());
  hasher.update_text(key.endpoint.value());
  hasher.update_u8(static_cast<std::uint8_t>(key.direction));
  hasher.update_u8(static_cast<std::uint8_t>(key.kind));
  hasher.update_text(observation.value());
  hasher.update_text(publisher.value());
  hasher.update_text(worker_boot.value());
  hasher.update_u64(epoch.value());
  hasher.update_u64(source_generation.value());
  hasher.update_u64(sequence);
  hasher.update_u64(topology_generation.value());
  hasher.update_u64(endpoint_generation.value());
  hasher.update_u8(static_cast<std::uint8_t>(evidence_class));
  hasher.update_u8(static_cast<std::uint8_t>(claim));
  hasher.update_u8(static_cast<std::uint8_t>(degradation_cause));
  hasher.update_text(detail);
  hasher.update_u32(static_cast<std::uint32_t>(metadata.size()));
  hasher.update(std::span<const std::byte>(metadata.data(), metadata.size()));
  return hasher.finalize();
}

Digest digest_evidence_set(const std::vector<EvidenceRecord>& records) {
  std::vector<const EvidenceRecord*> ordered;
  ordered.reserve(records.size());
  for (const EvidenceRecord& record : records) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(), [](const EvidenceRecord* lhs, const EvidenceRecord* rhs) {
    if (lhs->key != rhs->key) {
      return lhs->key < rhs->key;
    }
    if (lhs->observation != rhs->observation) {
      return lhs->observation < rhs->observation;
    }
    return lhs->sequence < rhs->sequence;
  });
  Hasher hasher;
  hasher.update_u32(static_cast<std::uint32_t>(ordered.size()));
  for (const EvidenceRecord* record : ordered) {
    hasher.update(record->compute_digest().bytes());
  }
  return hasher.finalize();
}

std::string render(const EvidenceRecord& record) {
  std::string out;
  out += "evidence ";
  out += record.observation.to_string();
  out.push_back('\n');
  out += "  key              ";
  out += record.key.to_string();
  out.push_back('\n');
  out += "  publisher        ";
  out += record.publisher.to_string();
  out.push_back('\n');
  out += "  worker_boot      ";
  out += record.worker_boot.to_string();
  out.push_back('\n');
  out += "  epoch            ";
  out += record.epoch.to_string();
  out.push_back('\n');
  out += "  source_generation ";
  out += record.source_generation.to_string();
  out.push_back('\n');
  out += "  sequence         ";
  out += detail::format_u64(record.sequence);
  out.push_back('\n');
  out += "  topology_gen     ";
  out += record.topology_generation.to_string();
  out.push_back('\n');
  out += "  endpoint_gen     ";
  out += record.endpoint_generation.to_string();
  out.push_back('\n');
  out += "  class            ";
  out += linkstate::to_string(record.evidence_class);
  out.push_back('\n');
  out += "  claim            ";
  out += linkstate::to_string(record.claim);
  out.push_back('\n');
  out += "  degradation      ";
  out += linkstate::to_string(record.degradation_cause);
  out.push_back('\n');
  out += "  status           ";
  out += linkstate::to_string(record.status);
  out.push_back('\n');
  out += "  digest           ";
  out += record.compute_digest().to_hex();
  out.push_back('\n');
  if (!record.detail.empty()) {
    out += "  detail           ";
    out += detail::single_line(record.detail);
    out.push_back('\n');
  }
  return out;
}

}  // namespace linkstate
