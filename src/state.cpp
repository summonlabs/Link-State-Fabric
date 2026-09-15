#include "linkstate/state.hpp"

#include <array>

namespace linkstate {
namespace {

constexpr std::uint16_t trigger_bit(TransitionTrigger trigger) noexcept {
  return static_cast<std::uint16_t>(1u << static_cast<unsigned>(trigger));
}

constexpr std::uint16_t kRetireTriggers =
    trigger_bit(TransitionTrigger::Retirement) | trigger_bit(TransitionTrigger::TopologyInvalidation);

constexpr std::uint16_t kRevalidationTriggers =
    trigger_bit(TransitionTrigger::EvidenceResolution) |
    trigger_bit(TransitionTrigger::EvidenceWithdrawal) |
    trigger_bit(TransitionTrigger::PublisherFenced) | trigger_bit(TransitionTrigger::SourceLost) |
    trigger_bit(TransitionTrigger::TopologyInvalidation) |
    trigger_bit(TransitionTrigger::CoordinatorEpochAdvance) |
    trigger_bit(TransitionTrigger::Recovery) | trigger_bit(TransitionTrigger::RevalidationOutcome);

constexpr std::uint16_t kUnknownTriggers =
    trigger_bit(TransitionTrigger::EvidenceResolution) |
    trigger_bit(TransitionTrigger::EvidenceWithdrawal) |
    trigger_bit(TransitionTrigger::RevalidationOutcome) | trigger_bit(TransitionTrigger::Recovery);

constexpr std::uint16_t kLiveTriggers = trigger_bit(TransitionTrigger::EvidenceResolution) |
                                        trigger_bit(TransitionTrigger::RevalidationOutcome) |
                                        trigger_bit(TransitionTrigger::ExplicitAdministrative);

constexpr std::array<std::uint16_t, link_operational_state_count + 1> kInboundTriggerMask = {
    /* UNKNOWN               */ kUnknownTriggers,
    /* UP                    */ kLiveTriggers,
    /* DOWN                  */ kLiveTriggers,
    /* DEGRADED              */ kLiveTriggers,
    /* ADMIN_DISABLED        */ kLiveTriggers,
    /* DRAINING              */ kLiveTriggers,
    /* FAULTED               */ kLiveTriggers,
    /* REVALIDATION_REQUIRED */ kRevalidationTriggers,
    /* RETIRED               */ kRetireTriggers,
};

}  // namespace

const char* to_string(LinkOperationalState state) noexcept {
  switch (state) {
    case LinkOperationalState::Unknown:
      return "UNKNOWN";
    case LinkOperationalState::Up:
      return "UP";
    case LinkOperationalState::Down:
      return "DOWN";
    case LinkOperationalState::Degraded:
      return "DEGRADED";
    case LinkOperationalState::AdminDisabled:
      return "ADMIN_DISABLED";
    case LinkOperationalState::Draining:
      return "DRAINING";
    case LinkOperationalState::Faulted:
      return "FAULTED";
    case LinkOperationalState::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case LinkOperationalState::Retired:
      return "RETIRED";
  }
  return "UNKNOWN";
}

std::optional<LinkOperationalState> parse_state(std::string_view text) noexcept {
  for (std::size_t index = 0; index < link_operational_state_count; ++index) {
    const auto candidate = static_cast<LinkOperationalState>(index);
    if (text == to_string(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

const char* describe(LinkOperationalState state) noexcept {
  switch (state) {
    case LinkOperationalState::Unknown:
      return "no decisive current evidence and no durable prior conclusion";
    case LinkOperationalState::Up:
      return "current evidence supports operational availability";
    case LinkOperationalState::Down:
      return "current evidence supports a non operational link";
    case LinkOperationalState::Degraded:
      return "operational but materially impaired";
    case LinkOperationalState::AdminDisabled:
      return "intentionally disabled by administrative intent";
    case LinkOperationalState::Draining:
      return "administratively quiescing, must not take new work";
    case LinkOperationalState::Faulted:
      return "a hardware or device fault was reported";
    case LinkOperationalState::RevalidationRequired:
      return "a durable prior state exists but no current evidence is authoritative";
    case LinkOperationalState::Retired:
      return "the underlying topology edge or link identity was retired";
  }
  return "unknown state";
}

bool is_live_state(LinkOperationalState state) noexcept {
  return state != LinkOperationalState::Retired;
}

bool is_terminal_state(LinkOperationalState state) noexcept {
  return state == LinkOperationalState::Retired;
}

bool is_operational_state(LinkOperationalState state) noexcept {
  return state == LinkOperationalState::Up || state == LinkOperationalState::Degraded ||
         state == LinkOperationalState::Draining;
}

const char* to_string(TransitionTrigger trigger) noexcept {
  switch (trigger) {
    case TransitionTrigger::EvidenceResolution:
      return "EVIDENCE_RESOLUTION";
    case TransitionTrigger::EvidenceWithdrawal:
      return "EVIDENCE_WITHDRAWAL";
    case TransitionTrigger::PublisherFenced:
      return "PUBLISHER_FENCED";
    case TransitionTrigger::SourceLost:
      return "SOURCE_LOST";
    case TransitionTrigger::TopologyInvalidation:
      return "TOPOLOGY_INVALIDATION";
    case TransitionTrigger::CoordinatorEpochAdvance:
      return "COORDINATOR_EPOCH_ADVANCE";
    case TransitionTrigger::Recovery:
      return "RECOVERY";
    case TransitionTrigger::Retirement:
      return "RETIREMENT";
    case TransitionTrigger::ExplicitAdministrative:
      return "EXPLICIT_ADMINISTRATIVE";
    case TransitionTrigger::RevalidationOutcome:
      return "REVALIDATION_OUTCOME";
  }
  return "EVIDENCE_RESOLUTION";
}

const char* to_string(TransitionLegality legality) noexcept {
  switch (legality) {
    case TransitionLegality::Legal:
      return "LEGAL";
    case TransitionLegality::SameState:
      return "SAME_STATE";
    case TransitionLegality::FromRetired:
      return "FROM_RETIRED";
    case TransitionLegality::RetirementRequired:
      return "RETIREMENT_REQUIRED";
    case TransitionLegality::TriggerNotPermitted:
      return "TRIGGER_NOT_PERMITTED";
    case TransitionLegality::Illegal:
      return "ILLEGAL";
  }
  return "ILLEGAL";
}

TransitionLegality evaluate_transition(LinkOperationalState from, LinkOperationalState to,
                                       TransitionTrigger trigger) noexcept {
  const auto from_index = static_cast<std::size_t>(from);
  const auto to_index = static_cast<std::size_t>(to);
  if (from_index >= link_operational_state_count || to_index >= link_operational_state_count) {
    return TransitionLegality::Illegal;
  }
  if (from == to) {
    return TransitionLegality::SameState;
  }
  if (from == LinkOperationalState::Retired) {
    return TransitionLegality::FromRetired;
  }
  if (to == LinkOperationalState::Retired) {
    return (kInboundTriggerMask[to_index] & trigger_bit(trigger)) != 0
               ? TransitionLegality::Legal
               : TransitionLegality::RetirementRequired;
  }
  return (kInboundTriggerMask[to_index] & trigger_bit(trigger)) != 0 ? TransitionLegality::Legal
                                                                    : TransitionLegality::TriggerNotPermitted;
}

const char* to_string(LinkClass link_class) noexcept {
  switch (link_class) {
    case LinkClass::Unknown:
      return "UNKNOWN";
    case LinkClass::EthernetLike:
      return "ETHERNET_LIKE";
    case LinkClass::InfiniBandLike:
      return "INFINIBAND_LIKE";
    case LinkClass::HostNic:
      return "HOST_NIC";
    case LinkClass::SwitchToSwitch:
      return "SWITCH_TO_SWITCH";
    case LinkClass::Logical:
      return "LOGICAL";
    case LinkClass::TunnelBacked:
      return "TUNNEL_BACKED";
    case LinkClass::Optical:
      return "OPTICAL";
    case LinkClass::FabricInterconnect:
      return "FABRIC_INTERCONNECT";
    case LinkClass::BondedAggregate:
      return "BONDED_AGGREGATE";
  }
  return "UNKNOWN";
}

std::optional<LinkClass> parse_link_class(std::string_view text) noexcept {
  static constexpr std::array<LinkClass, 10> kClasses = {
      LinkClass::Unknown,        LinkClass::EthernetLike,       LinkClass::InfiniBandLike,
      LinkClass::HostNic,        LinkClass::SwitchToSwitch,     LinkClass::Logical,
      LinkClass::TunnelBacked,   LinkClass::Optical,            LinkClass::FabricInterconnect,
      LinkClass::BondedAggregate};
  for (const LinkClass candidate : kClasses) {
    if (text == to_string(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

bool link_class_is_physical(LinkClass link_class) noexcept {
  switch (link_class) {
    case LinkClass::Logical:
    case LinkClass::TunnelBacked:
      return false;
    default:
      return true;
  }
}

const char* to_string(LinkSymmetryGuarantee guarantee) noexcept {
  switch (guarantee) {
    case LinkSymmetryGuarantee::Unspecified:
      return "UNSPECIFIED";
    case LinkSymmetryGuarantee::Symmetric:
      return "SYMMETRIC";
    case LinkSymmetryGuarantee::Directional:
      return "DIRECTIONAL";
  }
  return "UNSPECIFIED";
}

LinkSymmetryGuarantee default_symmetry_for(LinkClass link_class) noexcept {
  switch (link_class) {
    case LinkClass::Optical:
    case LinkClass::TunnelBacked:
    case LinkClass::Logical:
      return LinkSymmetryGuarantee::Directional;
    case LinkClass::EthernetLike:
    case LinkClass::InfiniBandLike:
    case LinkClass::HostNic:
    case LinkClass::SwitchToSwitch:
    case LinkClass::FabricInterconnect:
    case LinkClass::BondedAggregate:
      return LinkSymmetryGuarantee::Symmetric;
    case LinkClass::Unknown:
      return LinkSymmetryGuarantee::Unspecified;
  }
  return LinkSymmetryGuarantee::Unspecified;
}

const char* to_string(StateResultClass result_class) noexcept {
  switch (result_class) {
    case StateResultClass::AuthoritativeUnknown:
      return "AUTHORITATIVE_UNKNOWN";
    case StateResultClass::AuthoritativeUp:
      return "AUTHORITATIVE_UP";
    case StateResultClass::AuthoritativeDown:
      return "AUTHORITATIVE_DOWN";
    case StateResultClass::AuthoritativeDegraded:
      return "AUTHORITATIVE_DEGRADED";
    case StateResultClass::AuthoritativeAdminDisabled:
      return "AUTHORITATIVE_ADMIN_DISABLED";
    case StateResultClass::AuthoritativeDraining:
      return "AUTHORITATIVE_DRAINING";
    case StateResultClass::AuthoritativeFaulted:
      return "AUTHORITATIVE_FAULTED";
    case StateResultClass::Conflicted:
      return "CONFLICTED";
    case StateResultClass::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case StateResultClass::Retired:
      return "RETIRED";
  }
  return "AUTHORITATIVE_UNKNOWN";
}

LinkOperationalState state_of(StateResultClass result_class) noexcept {
  switch (result_class) {
    case StateResultClass::AuthoritativeUnknown:
      return LinkOperationalState::Unknown;
    case StateResultClass::AuthoritativeUp:
      return LinkOperationalState::Up;
    case StateResultClass::AuthoritativeDown:
      return LinkOperationalState::Down;
    case StateResultClass::AuthoritativeDegraded:
      return LinkOperationalState::Degraded;
    case StateResultClass::AuthoritativeAdminDisabled:
      return LinkOperationalState::AdminDisabled;
    case StateResultClass::AuthoritativeDraining:
      return LinkOperationalState::Draining;
    case StateResultClass::AuthoritativeFaulted:
      return LinkOperationalState::Faulted;
    case StateResultClass::Conflicted:
      return LinkOperationalState::RevalidationRequired;
    case StateResultClass::RevalidationRequired:
      return LinkOperationalState::RevalidationRequired;
    case StateResultClass::Retired:
      return LinkOperationalState::Retired;
  }
  return LinkOperationalState::Unknown;
}

}  // namespace linkstate
