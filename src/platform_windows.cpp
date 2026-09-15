#include "linkstate/platform.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "text.hpp"

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#endif

namespace linkstate {
namespace {

#ifdef _WIN32

std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr) {
    return std::string();
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::string guid_identifier(const GUID& guid) {
  char buffer[64];
  const int written = std::snprintf(
      buffer, sizeof(buffer), "%08lx-%04hx-%04hx-%02x%02x-%02x%02x%02x%02x%02x%02x",
      static_cast<unsigned long>(guid.Data1), static_cast<unsigned short>(guid.Data2),
      static_cast<unsigned short>(guid.Data3), guid.Data4[0], guid.Data4[1], guid.Data4[2],
      guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  if (written <= 0) {
    return std::string();
  }
  std::string out(buffer, static_cast<std::size_t>(written));
  for (char& ch : out) {
    if (ch >= 'A' && ch <= 'F') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return out;
}

LinkClass class_for_if_type(unsigned long if_type) {
  switch (if_type) {
    case IF_TYPE_ETHERNET_CSMACD:
    case IF_TYPE_IEEE80211:
    case IF_TYPE_FASTETHER:
    case IF_TYPE_GIGABITETHERNET:
      return LinkClass::HostNic;
    case IF_TYPE_TUNNEL:
      return LinkClass::TunnelBacked;
    case IF_TYPE_SOFTWARE_LOOPBACK:
      return LinkClass::Logical;
    default:
      return LinkClass::HostNic;
  }
}

/// NDIS_PHYSICAL_MEDIUM values as published by the Windows IP Helper API.
/// The numeric values are part of the interface ABI and are used directly so
/// that the adapter does not depend on which optional SDK header a given
/// toolchain exposes.
enum : unsigned {
  kMediumUnspecified = 0,
  kMediumWirelessLan = 1,
  kMediumCableModem = 2,
  kMediumPhoneLine = 3,
  kMediumPowerLine = 4,
  kMediumDsl = 5,
  kMediumFibreChannel = 6,
  kMediumIeee80211 = 7,
  kMediumIeee8023 = 8,
  kMediumOther = 9,
};

/// NET_IF_ADMIN_STATUS values as published by the Windows IP Helper API.
enum : unsigned {
  kAdminStatusUp = 0,
  kAdminStatusEnabled = 1,
  kAdminStatusDisabled = 2,
  kAdminStatusTesting = 3,
  kAdminStatusUnknown = 4,
  kAdminStatusDormant = 5,
  kAdminStatusNotPresent = 6,
  kAdminStatusLowerLayerDown = 7,
};

const char* medium_name(unsigned medium) {
  switch (medium) {
    case kMediumUnspecified:
      return "UNSPECIFIED";
    case kMediumWirelessLan:
      return "WIRELESS_LAN";
    case kMediumCableModem:
      return "CABLE_MODEM";
    case kMediumPhoneLine:
      return "PHONE_LINE";
    case kMediumPowerLine:
      return "POWER_LINE";
    case kMediumDsl:
      return "DSL";
    case kMediumFibreChannel:
      return "FIBRE_CHANNEL";
    case kMediumIeee80211:
      return "IEEE80211";
    case kMediumIeee8023:
      return "IEEE8023_ETHERNET";
    case kMediumOther:
      return "OTHER";
    default:
      return "UNKNOWN";
  }
}

const char* oper_status_name(IF_OPER_STATUS status) {
  switch (status) {
    case IfOperStatusUp:
      return "UP";
    case IfOperStatusDown:
      return "DOWN";
    case IfOperStatusTesting:
      return "TESTING";
    case IfOperStatusUnknown:
      return "UNKNOWN";
    case IfOperStatusDormant:
      return "DORMANT";
    case IfOperStatusNotPresent:
      return "NOT_PRESENT";
    case IfOperStatusLowerLayerDown:
      return "LOWER_LAYER_DOWN";
    default:
      return "UNKNOWN";
  }
}

#endif  // _WIN32

void finalize_observation(HostLinkObservation& observation) {
  std::string detail;
  detail += "interface=";
  detail += observation.interface_alias.empty() ? observation.interface_guid
                                                : observation.interface_alias;
  detail += " guid=";
  detail += observation.interface_guid;
  detail += " admin_enabled=";
  detail += observation.administratively_enabled ? "true" : "false";
  detail += " media_connected=";
  detail += observation.media_connected ? "true" : "false";
  detail += " transmit_bps=";
  detail += detail::format_u64(observation.transmit_speed_bps);
  detail += " receive_bps=";
  detail += detail::format_u64(observation.receive_speed_bps);
  detail += " medium=";
  detail += observation.media_type;
  detail += " kind=";
  detail += linkstate::to_string(observation.kind);
  detail += " claim=";
  detail += linkstate::to_string(observation.claim);
  detail += " class=";
  detail += to_string(observation.evidence_class);
  if (observation.degradation_cause != DegradationCause::None) {
    detail += " cause=";
    detail += linkstate::to_string(observation.degradation_cause);
  }
  // The detail string is a bounded diagnostic: interface aliases and hardware
  // descriptions are host supplied and can be arbitrarily long. A value that
  // would exceed the evidence detail bound is truncated on a UTF-8 boundary
  // with an explicit marker instead of producing a request the engine rejects.
  if (detail.size() > limits::max_detail_length) {
    std::string truncated = detail.substr(0, limits::max_detail_length - 3);
    while (!truncated.empty() && !detail::is_valid_utf8(truncated)) {
      truncated.pop_back();
    }
    truncated += "...";
    detail = std::move(truncated);
  }
  observation.detail = std::move(detail);
}

}  // namespace

const char* to_string(HostEvidenceAvailability availability) noexcept {
  switch (availability) {
    case HostEvidenceAvailability::Available:
      return "AVAILABLE";
    case HostEvidenceAvailability::Unavailable:
      return "UNAVAILABLE";
    case HostEvidenceAvailability::Unsupported:
      return "UNSUPPORTED";
  }
  return "UNAVAILABLE";
}

std::string render(const HostLinkObservation& observation) {
  std::string out;
  out += "link            ";
  out += observation.link.value();
  out.push_back('\n');
  out += "endpoint        ";
  out += observation.endpoint.value();
  out.push_back('\n');
  out += "device          ";
  out += observation.device.value();
  out.push_back('\n');
  out += "port            ";
  out += observation.port.value();
  out.push_back('\n');
  out += "interface_guid  ";
  out += observation.interface_guid;
  out.push_back('\n');
  out += "alias           ";
  out += observation.interface_alias;
  out.push_back('\n');
  out += "description     ";
  out += observation.description;
  out.push_back('\n');
  out += "media_type      ";
  out += observation.media_type;
  out.push_back('\n');
  out += "link_class      ";
  out += linkstate::to_string(observation.link_class);
  out.push_back('\n');
  out += "evidence_kind   ";
  out += linkstate::to_string(observation.kind);
  out.push_back('\n');
  out += "claim           ";
  out += linkstate::to_string(observation.claim);
  out.push_back('\n');
  out += "evidence_class  ";
  out += linkstate::to_string(observation.evidence_class);
  out.push_back('\n');
  out += "detail          ";
  out += detail::single_line(observation.detail);
  out.push_back('\n');
  return out;
}

HostDiscoveryReport discover_host_links() {
  HostDiscoveryReport report;
#ifdef _WIN32
  PMIB_IF_TABLE2 table = nullptr;
  const NETIO_STATUS status = GetIfTable2(&table);
  if (status != NO_ERROR || table == nullptr) {
    report.availability = HostEvidenceAvailability::Unavailable;
    report.notes.push_back("the operating system interface inventory could not be queried");
    if (table != nullptr) {
      FreeMibTable(table);
    }
    return report;
  }
  report.availability = HostEvidenceAvailability::Available;
  for (ULONG index = 0; index < table->NumEntries; ++index) {
    const MIB_IF_ROW2& row = table->Table[index];
    ++report.interfaces_enumerated;
    if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) {
      ++report.unsupported_interfaces;
      report.notes.push_back("software loopback interface was skipped: it is not a fabric link");
      continue;
    }
    const std::string guid = guid_identifier(row.InterfaceGuid);
    if (guid.empty()) {
      ++report.unsupported_interfaces;
      report.notes.push_back("interface without a usable interface GUID was skipped");
      continue;
    }

    HostLinkObservation base;
    base.link = LinkId::from_validated("link.host." + guid);
    base.endpoint = EndpointId::from_validated("ep.host." + guid);
    base.device = DeviceId::from_validated("dev.host." + guid);
    base.port = PortId::from_validated("port.host." + guid);
    base.link_class = class_for_if_type(static_cast<unsigned long>(row.Type));
    base.interface_guid = guid;
    base.interface_alias = wide_to_utf8(row.Alias);
    base.description = wide_to_utf8(row.Description);
    base.media_type = medium_name(static_cast<unsigned>(row.PhysicalMediumType));
    base.transmit_speed_bps = row.TransmitLinkSpeed;
    base.receive_speed_bps = row.ReceiveLinkSpeed;
    base.interface_index = row.InterfaceIndex;
    base.interface_type = row.Type;
    base.media_connected = row.MediaConnectState == MediaConnectStateConnected;
    const unsigned admin_status = static_cast<unsigned>(row.AdminStatus);
    base.administratively_enabled = admin_status != kAdminStatusDisabled;

    const bool media_known =
        row.MediaConnectState == MediaConnectStateConnected ||
        row.MediaConnectState == MediaConnectStateDisconnected;
    const bool upstream_down = row.OperStatus == IfOperStatusDown ||
                               row.OperStatus == IfOperStatusNotPresent ||
                               row.OperStatus == IfOperStatusLowerLayerDown ||
                               row.OperStatus == IfOperStatusDormant;
    const bool oper_up = row.OperStatus == IfOperStatusUp;

    // Administrative enablement is a constraint, never an operational claim.
    if (admin_status == kAdminStatusDisabled || admin_status == kAdminStatusEnabled) {
      HostLinkObservation admin = base;
      admin.kind = EvidenceKind::AdministrativeState;
      admin.claim = admin.administratively_enabled ? EvidenceClaim::Up : EvidenceClaim::AdminDisabled;
      finalize_observation(admin);
      report.observations.push_back(std::move(admin));
    }

    if (media_known) {
      HostLinkObservation carrier = base;
      carrier.kind = EvidenceKind::CarrierState;
      carrier.claim = carrier.media_connected ? EvidenceClaim::Up : EvidenceClaim::Down;
      finalize_observation(carrier);
      report.observations.push_back(std::move(carrier));
    } else {
      report.notes.push_back("media connect state is not reported for interface " + guid +
                             "; no carrier evidence was produced");
    }

    HostLinkObservation oper = base;
    oper.kind = EvidenceKind::InterfaceOperationalState;
    if (!oper.administratively_enabled) {
      oper.claim = EvidenceClaim::Down;
    } else if (oper_up && base.media_connected) {
      const bool asymmetric = base.transmit_speed_bps != base.receive_speed_bps &&
                              (base.transmit_speed_bps != 0 || base.receive_speed_bps != 0);
      if (asymmetric) {
        oper.claim = EvidenceClaim::Degraded;
        oper.degradation_cause = DegradationCause::ReducedNegotiatedSpeed;
      } else {
        oper.claim = EvidenceClaim::Up;
      }
    } else if (upstream_down) {
      oper.claim = EvidenceClaim::Down;
    } else if (oper_up) {
      // The interface is administratively up but the media is not connected.
      oper.claim = EvidenceClaim::Down;
    } else {
      oper.claim = EvidenceClaim::Unknown;
    }
    if (oper.claim != EvidenceClaim::Unknown) {
      finalize_observation(oper);
      report.observations.push_back(std::move(oper));
    } else {
      report.notes.push_back("operational state is not reported for interface " + guid +
                             "; no operational evidence was produced");
    }
  }
  FreeMibTable(table);
#else
  report.availability = HostEvidenceAvailability::Unsupported;
  report.notes.push_back(
      "host interface discovery is implemented for Windows in this release; the capability is "
      "UNSUPPORTED on this platform");
#endif
  std::sort(report.observations.begin(), report.observations.end(),
            [](const HostLinkObservation& lhs, const HostLinkObservation& rhs) {
              if (lhs.link != rhs.link) {
                return lhs.link < rhs.link;
              }
              if (lhs.kind != rhs.kind) {
                return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
              }
              return static_cast<unsigned>(lhs.claim) < static_cast<unsigned>(rhs.claim);
            });
  return report;
}

HostEvidencePublisher::HostEvidencePublisher(PublisherId publisher, WorkerBootId worker_boot,
                                             SourceId source)
    : publisher_(std::move(publisher)), worker_boot_(std::move(worker_boot)),
      source_(std::move(source)) {}

std::vector<PublishEvidenceRequest> HostEvidencePublisher::requests(
    const HostDiscoveryReport& report,
    const std::unordered_map<LinkId, HostBindingContext>& bindings, CoordinatorEpoch epoch) {
  std::vector<PublishEvidenceRequest> requests;
  for (const HostLinkObservation& observation : report.observations) {
    const auto binding = bindings.find(observation.link);
    if (binding == bindings.end()) {
      continue;
    }
    const std::string key = observation.link.value() + "|" + source_.value() + "|" +
                            observation.endpoint.value() + "|" +
                            std::to_string(static_cast<unsigned>(observation.kind));
    const std::uint64_t sequence = ++sequences_[key];
    ++observation_counter_;

    EvidenceRecord record;
    record.key.link = observation.link;
    record.key.source = source_;
    record.key.endpoint = observation.endpoint;
    record.key.direction = EvidenceDirection::Bidirectional;
    record.key.kind = observation.kind;
    record.observation =
        ObservationId::from_validated("obs.host." + detail::format_u64(observation_counter_));
    record.publisher = publisher_;
    record.worker_boot = worker_boot_;
    record.epoch = epoch;
    record.source_generation = source_generation_;
    record.sequence = sequence;
    record.topology_generation = binding->second.topology_generation;
    record.endpoint_generation = binding->second.endpoint_generation;
    record.evidence_class = EvidenceClass::Real;
    record.claim = observation.claim;
    record.degradation_cause = observation.degradation_cause;
    record.detail = observation.detail;

    PublishEvidenceRequest request;
    request.observation = std::move(record);
    request.publication =
        PublicationId::from_validated("pub.host." + detail::format_u64(observation_counter_));
    requests.push_back(std::move(request));
  }
  ++round_;
  if (const auto next = source_generation_.next(); next.has_value()) {
    source_generation_ = *next;
  }
  return requests;
}

}  // namespace linkstate
