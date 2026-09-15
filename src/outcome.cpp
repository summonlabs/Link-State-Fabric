#include "linkstate/outcome.hpp"

namespace linkstate {

std::string_view to_string(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::Committed:
      return "COMMITTED";
    case OutcomeCode::Idempotent:
      return "IDEMPOTENT";
    case OutcomeCode::NoChange:
      return "NO_CHANGE";
    case OutcomeCode::StaleGeneration:
      return "STALE_GENERATION";
    case OutcomeCode::StaleEvidence:
      return "STALE_EVIDENCE";
    case OutcomeCode::StaleTopology:
      return "STALE_TOPOLOGY";
    case OutcomeCode::StaleAuthority:
      return "STALE_AUTHORITY";
    case OutcomeCode::StaleCoordinatorEpoch:
      return "STALE_COORDINATOR_EPOCH";
    case OutcomeCode::UnauthorizedScope:
      return "UNAUTHORIZED_SCOPE";
    case OutcomeCode::UnauthorizedPublisher:
      return "UNAUTHORIZED_PUBLISHER";
    case OutcomeCode::UnknownLink:
      return "UNKNOWN_LINK";
    case OutcomeCode::RetiredLink:
      return "RETIRED_LINK";
    case OutcomeCode::EvidenceConflict:
      return "EVIDENCE_CONFLICT";
    case OutcomeCode::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case OutcomeCode::InvalidTransition:
      return "INVALID_TRANSITION";
    case OutcomeCode::MalformedRequest:
      return "MALFORMED_REQUEST";
    case OutcomeCode::ResourceLimit:
      return "RESOURCE_LIMIT";
    case OutcomeCode::PersistenceCorrupt:
      return "PERSISTENCE_CORRUPT";
    case OutcomeCode::PersistenceIoFailure:
      return "PERSISTENCE_IO_FAILURE";
    case OutcomeCode::TransportFailure:
      return "TRANSPORT_FAILURE";
    case OutcomeCode::UnsupportedCapability:
      return "UNSUPPORTED_CAPABILITY";
    case OutcomeCode::UnsupportedState:
      return "UNSUPPORTED_STATE";
    case OutcomeCode::NotFound:
      return "NOT_FOUND";
    case OutcomeCode::Invalidated:
      return "INVALIDATED";
  }
  return "MALFORMED_REQUEST";
}

Outcome Outcome::make(OutcomeCode code, std::string detail) {
  Outcome outcome;
  outcome.code = code;
  outcome.detail = std::move(detail);
  return outcome;
}

bool Outcome::is_error() const noexcept {
  switch (code) {
    case OutcomeCode::Committed:
    case OutcomeCode::Idempotent:
    case OutcomeCode::NoChange:
      return false;
    default:
      return true;
  }
}

std::string Outcome::to_string() const {
  std::string out(linkstate::to_string(code));
  if (!detail.empty()) {
    out += ": ";
    out += detail;
  }
  if (!state_generation.is_zero()) {
    out += " [state_gen=";
    out += state_generation.to_string();
    out.push_back(']');
  }
  return out;
}

}  // namespace linkstate
