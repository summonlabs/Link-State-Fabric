#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine_internal.hpp"
#include "text.hpp"

namespace linkstate::detail {
namespace {

/// Effective precedence tier of a record. Synthetic evidence never outranks
/// real evidence, whatever its kind would otherwise imply.
std::uint8_t effective_precedence(const EvidenceRecord& record) {
  if (record.evidence_class == EvidenceClass::Synthetic) {
    return 5;
  }
  return evidence_precedence(record.key.kind);
}

bool claim_is_operational(EvidenceClaim claim) noexcept {
  return claim == EvidenceClaim::Up || claim == EvidenceClaim::Degraded;
}

bool claim_is_fault_or_down(EvidenceClaim claim) noexcept {
  return claim == EvidenceClaim::Down || claim == EvidenceClaim::Faulted;
}

std::string describe(const EvidenceRecord& record) {
  std::string out = record.key.source.value();
  out.push_back('/');
  out += linkstate::to_string(record.key.kind);
  out.push_back('/');
  out += linkstate::to_string(record.key.direction);
  out += " -> ";
  out += linkstate::to_string(record.claim);
  out += " (observation ";
  out += record.observation.value();
  out.push_back(')');
  return out;
}

EvidenceDisposition disposition_for(const EvidenceRecord& record) {
  EvidenceDisposition disposition;
  disposition.observation = record.observation;
  disposition.source = record.key.source;
  disposition.publisher = record.publisher;
  disposition.worker_boot = record.worker_boot;
  disposition.kind = record.key.kind;
  disposition.direction = record.key.direction;
  disposition.claim = record.claim;
  disposition.evidence_class = record.evidence_class;
  disposition.status = record.status;
  disposition.precedence = effective_precedence(record);
  disposition.counted = false;
  return disposition;
}

StateResultClass class_for(EvidenceClaim claim) {
  switch (claim) {
    case EvidenceClaim::Up:
      return StateResultClass::AuthoritativeUp;
    case EvidenceClaim::Down:
      return StateResultClass::AuthoritativeDown;
    case EvidenceClaim::Degraded:
      return StateResultClass::AuthoritativeDegraded;
    case EvidenceClaim::AdminDisabled:
      return StateResultClass::AuthoritativeAdminDisabled;
    case EvidenceClaim::Draining:
      return StateResultClass::AuthoritativeDraining;
    case EvidenceClaim::Faulted:
      return StateResultClass::AuthoritativeFaulted;
    case EvidenceClaim::Unknown:
      return StateResultClass::AuthoritativeUnknown;
  }
  return StateResultClass::AuthoritativeUnknown;
}

void set_state(Resolution& resolution, LinkOperationalState state, StateResultClass result_class,
               DegradationCause cause) {
  resolution.state = state;
  resolution.result_class = result_class;
  resolution.cause = cause;
}

DegradationCause strongest_cause(const std::vector<const EvidenceRecord*>& records) {
  DegradationCause cause = DegradationCause::None;
  for (const EvidenceRecord* record : records) {
    if (record->claim != EvidenceClaim::Degraded) {
      continue;
    }
    if (cause == DegradationCause::None ||
        static_cast<unsigned>(record->degradation_cause) < static_cast<unsigned>(cause)) {
      cause = record->degradation_cause;
    }
  }
  return cause;
}

void note_conflict(Resolution& resolution, const std::string& text) {
  resolution.conflicts.push_back(text);
}

}  // namespace

bool evidence_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs) {
  if (lhs.key != rhs.key) {
    return lhs.key < rhs.key;
  }
  if (lhs.observation != rhs.observation) {
    return lhs.observation < rhs.observation;
  }
  if (lhs.publisher != rhs.publisher) {
    return lhs.publisher < rhs.publisher;
  }
  return lhs.sequence < rhs.sequence;
}

Resolution resolve_link_state(std::span<const EvidenceRecord* const> records,
                              const LinkStateRecord& record) {
  Resolution resolution;

  std::vector<const EvidenceRecord*> ordered(records.begin(), records.end());
  std::sort(ordered.begin(), ordered.end(),
            [](const EvidenceRecord* lhs, const EvidenceRecord* rhs) {
              return evidence_less(*lhs, *rhs);
            });

  std::vector<const EvidenceRecord*> counted;
  for (const EvidenceRecord* record_pointer : ordered) {
    const EvidenceRecord& candidate = *record_pointer;
    EvidenceDisposition disposition = disposition_for(candidate);
    if (candidate.status != EvidenceStatus::Current) {
      disposition.reason = "evidence status is ";
      disposition.reason += linkstate::to_string(candidate.status);
      disposition.reason += ", not current";
    } else if (candidate.evidence_class == EvidenceClass::Unsupported) {
      disposition.reason = "unsupported capability evidence is never decisive";
    } else if (candidate.claim == EvidenceClaim::Unknown) {
      disposition.reason = "source reports no decisive claim";
    } else {
      disposition.counted = true;
      disposition.reason = "counted at precedence tier ";
      disposition.reason += detail::format_u64(disposition.precedence);
      counted.push_back(record_pointer);
    }
    resolution.dispositions.push_back(std::move(disposition));
  }

  // Durable administrative intent is the strongest administrative statement:
  // it was recorded through the explicit transition surface by an authorized
  // publisher and it survives evidence churn.
  if (record.admin_intent.has_value()) {
    const AdministrativeIntent& intent = record.admin_intent.value();
    resolution.rules.push_back(
        "RULE_ADMIN_INTENT: durable administrative intent (" +
        std::string(linkstate::to_string(intent.state)) + ", publication " +
        intent.publication.value() + ", publisher " + intent.publisher.value() +
        ", global generation " + intent.generation.to_string() +
        ") is in force; operational claims are recorded but do not override it");
    const auto claim = claim_from_state(intent.state);
    const EvidenceClaim effective_claim = claim.has_value() ? *claim : EvidenceClaim::AdminDisabled;
    set_state(resolution, state_of(class_for(effective_claim)), class_for(effective_claim),
              intent.cause);
    for (EvidenceDisposition& disposition : resolution.dispositions) {
      if (disposition.counted) {
        disposition.reason += "; overridden by durable administrative intent";
      }
    }
    return resolution;
  }

  // Administrative dimension: an administrative restriction outranks every
  // operational observation. Restrictiveness wins over a permissive claim from
  // another administrative source and the disagreement is reported.
  std::vector<const EvidenceRecord*> administrative;
  std::vector<const EvidenceRecord*> decisive;
  for (const EvidenceRecord* candidate : counted) {
    if (candidate->key.kind == EvidenceKind::AdministrativeState) {
      administrative.push_back(candidate);
    } else {
      decisive.push_back(candidate);
    }
  }

  if (!administrative.empty()) {
    bool disabled = false;
    bool draining = false;
    for (const EvidenceRecord* candidate : administrative) {
      if (candidate->claim == EvidenceClaim::AdminDisabled) {
        disabled = true;
      } else if (candidate->claim == EvidenceClaim::Draining) {
        draining = true;
      }
    }
    const EvidenceClaim winner = disabled ? EvidenceClaim::AdminDisabled : EvidenceClaim::Draining;
    if (disabled && draining) {
      note_conflict(resolution,
                    "RULE_ADMIN_RESTRICTIVENESS: administrative sources disagree "
                    "(ADMIN_DISABLED and DRAINING); the more restrictive intent wins");
    }
    if (disabled || draining) {
      resolution.rules.push_back(
          disabled
              ? "RULE_ADMIN_DISABLE_GATE: current administrative evidence disables the link; "
                "operational claims are recorded but do not override administrative intent"
              : "RULE_ADMIN_DRAIN_GATE: current administrative evidence drains the link; "
                "operational claims are recorded but do not override administrative intent");
      set_state(resolution, state_of(class_for(winner)), class_for(winner), DegradationCause::None);
      for (EvidenceDisposition& disposition : resolution.dispositions) {
        if (disposition.counted && disposition.kind != EvidenceKind::AdministrativeState) {
          disposition.reason += "; overridden by administrative intent";
        }
      }
      return resolution;
    }
    resolution.rules.push_back(
        "RULE_ADMIN_ENABLED: administrative evidence reports the link as administratively enabled; "
        "enablement is not an operational assertion and does not establish UP");
    for (EvidenceDisposition& disposition : resolution.dispositions) {
      if (disposition.counted && disposition.kind == EvidenceKind::AdministrativeState) {
        disposition.counted = false;
        disposition.reason += "; administrative enablement is a constraint, not a state claim";
      }
    }
  }

  if (decisive.empty()) {
    if (record.ever_established) {
      resolution.rules.push_back(
          "RULE_NO_DECISIVE_EVIDENCE_PRIOR_DURABLE: the link carries a durable prior conclusion but "
          "no current decisive evidence; the stale conclusion is not presented as current");
      set_state(resolution, LinkOperationalState::RevalidationRequired,
                StateResultClass::RevalidationRequired, DegradationCause::None);
    } else {
      resolution.rules.push_back(
          "RULE_NO_DECISIVE_EVIDENCE: no current decisive evidence and no durable prior conclusion");
      set_state(resolution, LinkOperationalState::Unknown, StateResultClass::AuthoritativeUnknown,
                DegradationCause::None);
    }
    return resolution;
  }

  std::uint8_t minimum_tier = 6;
  for (const EvidenceRecord* candidate : decisive) {
    minimum_tier = (std::min)(minimum_tier, effective_precedence(*candidate));
  }
  std::vector<const EvidenceRecord*> tier;
  for (const EvidenceRecord* candidate : decisive) {
    if (effective_precedence(*candidate) == minimum_tier) {
      tier.push_back(candidate);
    }
    if (effective_precedence(*candidate) > minimum_tier) {
      for (EvidenceDisposition& disposition : resolution.dispositions) {
        if (disposition.observation == candidate->observation) {
          disposition.counted = false;
          disposition.reason += "; outranked by precedence tier ";
          disposition.reason += detail::format_u64(minimum_tier);
        }
      }
    }
  }

  for (const EvidenceRecord* candidate : tier) {
    if (candidate->evidence_class == EvidenceClass::Synthetic) {
      resolution.synthetic_backed = true;
    }
  }
  if (!administrative.empty()) {
    for (const EvidenceRecord* candidate : administrative) {
      if (candidate->evidence_class == EvidenceClass::Synthetic) {
        resolution.synthetic_backed = true;
      }
    }
  }

  resolution.rules.push_back("RULE_PRECEDENCE_TIER: resolution used precedence tier " +
                             detail::format_u64(minimum_tier) + " with " +
                             detail::format_u64(static_cast<std::uint64_t>(tier.size())) +
                             " current evidence records");

  // Directional asymmetry: only meaningful between transmit and receive
  // evidence of the same source observing the same endpoint.
  std::vector<const EvidenceRecord*> asymmetry;
  for (const EvidenceRecord* candidate : tier) {
    if (candidate->key.direction == EvidenceDirection::Bidirectional) {
      continue;
    }
    for (const EvidenceRecord* peer : tier) {
      if (peer == candidate || peer->key.source != candidate->key.source ||
          peer->key.endpoint != candidate->key.endpoint) {
        continue;
      }
      if (peer->key.direction == candidate->key.direction) {
        continue;
      }
      if (claim_is_operational(candidate->claim) == claim_is_operational(peer->claim)) {
        continue;
      }
      asymmetry.push_back(candidate);
      break;
    }
  }

  if (!asymmetry.empty()) {
    std::string note = "RULE_DIRECTIONAL_ASYMMETRY: ";
    note += describe(*asymmetry.front());
    note += " disagrees with its peer direction";
    if (record.binding.symmetry == LinkSymmetryGuarantee::Directional) {
      resolution.rules.push_back(note +
                                 "; the link class supports asymmetric evidence, so the link is "
                                 "DEGRADED with cause ASYMMETRY");
      set_state(resolution, LinkOperationalState::Degraded, StateResultClass::AuthoritativeDegraded,
                DegradationCause::Asymmetry);
      return resolution;
    }
    note += record.binding.symmetry == LinkSymmetryGuarantee::Symmetric
                ? "; the link class guarantees symmetric behaviour, so the contradiction cannot be "
                  "resolved"
                : "; the link class does not declare a symmetry guarantee, so the contradiction "
                  "cannot be resolved";
    note_conflict(resolution, note);
    resolution.rules.push_back("RULE_DIRECTIONAL_CONFLICT: directional evidence contradicts itself");
    set_state(resolution, LinkOperationalState::RevalidationRequired, StateResultClass::Conflicted,
              DegradationCause::None);
    resolution.conflicted = true;
    return resolution;
  }

  std::set<EvidenceClaim> claims;
  for (const EvidenceRecord* candidate : tier) {
    claims.insert(candidate->claim);
  }
  if (claims.size() == 1) {
    const EvidenceClaim claim = *claims.begin();
    resolution.rules.push_back("RULE_UNANIMOUS_TIER: every current evidence record at the winning "
                               "precedence tier agrees");
    set_state(resolution, state_of(class_for(claim)), class_for(claim),
              claim == EvidenceClaim::Degraded ? strongest_cause(tier) : DegradationCause::None);
  } else {
    bool only_operational = true;
    bool only_non_operational = true;
    for (const EvidenceClaim claim : claims) {
      if (!claim_is_operational(claim)) {
        only_operational = false;
      }
      if (!claim_is_fault_or_down(claim)) {
        only_non_operational = false;
      }
    }
    if (only_operational) {
      resolution.rules.push_back(
          "RULE_IMPAIRMENT_WINS: current evidence agrees the link is operational and disagrees only "
          "about impairment; impairment evidence is stronger than an unimpaired claim");
      set_state(resolution, LinkOperationalState::Degraded, StateResultClass::AuthoritativeDegraded,
                strongest_cause(tier));
    } else if (only_non_operational) {
      resolution.rules.push_back(
          "RULE_FAULT_SPECIFICITY: current evidence agrees the link is not operational and names a "
          "specific fault; the specific fault is reported");
      set_state(resolution, LinkOperationalState::Faulted, StateResultClass::AuthoritativeFaulted,
                strongest_cause(tier));
    } else {
      for (const EvidenceRecord* candidate : tier) {
        note_conflict(resolution, "RULE_OPERATIONALITY_CONFLICT: " + describe(*candidate));
      }
      resolution.rules.push_back(
          "RULE_OPERATIONALITY_CONFLICT: current evidence at the winning precedence tier disagrees "
          "about whether the link is operational; the runtime refuses to force a binary answer");
      set_state(resolution, LinkOperationalState::RevalidationRequired,
                StateResultClass::Conflicted, DegradationCause::None);
      resolution.conflicted = true;
      return resolution;
    }
  }

  // Remote endpoint cross-check: the peer's view of the link is weaker than a
  // local observation but is never silently discarded.
  std::vector<const EvidenceRecord*> remote;
  for (const EvidenceRecord* candidate : decisive) {
    if (effective_precedence(*candidate) == 4) {
      remote.push_back(candidate);
    }
  }
  if (minimum_tier < 4 && !remote.empty()) {
    const bool resolved_operational = is_operational_state(resolution.state);
    bool contradiction = false;
    for (const EvidenceRecord* candidate : remote) {
      if (claim_is_operational(candidate->claim) != resolved_operational) {
        contradiction = true;
      }
    }
    if (!contradiction) {
      resolution.rules.push_back(
          "RULE_REMOTE_CORROBORATION: remote endpoint evidence agrees with the local resolution");
    } else if (record.binding.symmetry == LinkSymmetryGuarantee::Directional) {
      resolution.rules.push_back(
          "RULE_REMOTE_ASYMMETRY: the remote endpoint disagrees with the local observation and the "
          "link class supports asymmetry; the link is DEGRADED with cause ASYMMETRY");
      for (const EvidenceRecord* candidate : remote) {
        note_conflict(resolution, "RULE_REMOTE_ASYMMETRY: " + describe(*candidate));
      }
      set_state(resolution, LinkOperationalState::Degraded, StateResultClass::AuthoritativeDegraded,
                DegradationCause::Asymmetry);
    } else {
      for (const EvidenceRecord* candidate : remote) {
        note_conflict(resolution, "RULE_REMOTE_CONFLICT: " + describe(*candidate));
      }
      resolution.rules.push_back(
          "RULE_REMOTE_CONFLICT: the remote endpoint contradicts the local observation on a link "
          "class that does not support asymmetry; neither side is authoritative");
      set_state(resolution, LinkOperationalState::RevalidationRequired,
                StateResultClass::Conflicted, DegradationCause::None);
      resolution.conflicted = true;
    }
  }

  return resolution;
}

}  // namespace linkstate::detail
