#include <algorithm>
#include <string>
#include <vector>

#include "engine_internal.hpp"
#include "engine_support.hpp"
#include "text.hpp"

namespace linkstate {

Explanation LinkStateEngine::explain(const LinkId& link) const {
  std::shared_lock lock(impl_->mutex);
  Explanation explanation;
  explanation.link = link;
  const auto it = impl_->store.links.find(link);
  if (it == impl_->store.links.end()) {
    explanation.rules.push_back("RULE_UNKNOWN_LINK: no durable record exists for this link");
    return explanation;
  }
  const detail::LinkEntry& entry = it->second;
  const LinkStateRecord& record = entry.record;

  explanation.state = record.state;
  explanation.result_class = record.result_class;
  explanation.degradation_cause = record.degradation_cause;
  explanation.state_generation = record.state_generation;
  explanation.global_generation = record.global_generation;
  explanation.evidence_generation = record.evidence_generation;
  explanation.topology_generation = record.binding.topology_generation;

  explanation.rules.push_back(std::string("RULE_DURABLE_STATE: ") +
                              linkstate::to_string(record.state) + " - " +
                              describe(record.state));
  explanation.rules.push_back("RULE_TOPOLOGY_BINDING: link " + record.binding.link.value() +
                              " is bound to topology generation " +
                              record.binding.topology_generation.to_string() + " with local endpoint " +
                              (record.binding.endpoints.local.valid()
                                   ? record.binding.endpoints.local.value()
                                   : std::string("<none>")) +
                              " at endpoint generation " +
                              record.binding.endpoints.local_generation.to_string() +
                              (record.binding.endpoints.remote.valid()
                                   ? " and remote endpoint " + record.binding.endpoints.remote.value() +
                                         " at endpoint generation " +
                                         record.binding.endpoints.remote_generation.to_string()
                                   : std::string(" with no remote endpoint")));
  for (const BackingReference& backing : record.binding.backing_links) {
    std::string note = "RULE_BACKING_REFERENCE: depends on ";
    note += backing.link.value();
    note += " pinned at topology generation ";
    note += backing.generation.to_string();
    const auto backing_it = impl_->store.links.find(backing.link);
    if (backing_it == impl_->store.links.end()) {
      note += " which is not bound in this engine";
    } else {
      note += " which is currently at topology generation ";
      note += backing_it->second.record.binding.topology_generation.to_string();
    }
    explanation.rules.push_back(std::move(note));
  }
  if (record.admin_intent.has_value()) {
    explanation.rules.push_back(
        "RULE_ADMIN_INTENT: durable administrative intent " +
        std::string(linkstate::to_string(record.admin_intent->state)) + " recorded by publication " +
        record.admin_intent->publication.value() + " from publisher " +
        record.admin_intent->publisher.value());
  }

  const std::vector<const EvidenceRecord*> pointers = detail::current_pointers(entry);
  const detail::Resolution resolution = detail::resolve_link_state(pointers, record);
  for (const std::string& rule : resolution.rules) {
    explanation.rules.push_back(rule);
  }
  explanation.conflicts = resolution.conflicts;
  explanation.evidence = resolution.dispositions;
  for (const EvidenceDisposition& retained : entry.dispositions) {
    bool known = false;
    for (const EvidenceDisposition& disposition : explanation.evidence) {
      if (disposition.observation == retained.observation) {
        known = true;
        break;
      }
    }
    if (!known) {
      explanation.evidence.push_back(retained);
    }
  }
  std::sort(explanation.evidence.begin(), explanation.evidence.end(),
            [](const EvidenceDisposition& lhs, const EvidenceDisposition& rhs) {
              if (lhs.observation != rhs.observation) {
                return lhs.observation < rhs.observation;
              }
              return lhs.source < rhs.source;
            });

  explanation.authority.push_back(
      "AUTHORITY_SCOPE: " + detail::format_u64(static_cast<std::uint64_t>(impl_->store.publishers.size())) +
      " publisher incarnations are registered; " +
      detail::format_u64(static_cast<std::uint64_t>(impl_->store.fences.size())) +
      " fences have been recorded by this engine");
  for (const EvidenceDisposition& disposition : explanation.evidence) {
    if (disposition.status == EvidenceStatus::StaleAuthority) {
      explanation.authority.push_back("FENCED: observation " + disposition.observation.value() +
                                      " from publisher " + disposition.publisher.value() +
                                      " (worker boot " + disposition.worker_boot.value() +
                                      ") is " + linkstate::to_string(disposition.status) + ": " +
                                      disposition.reason);
    }
  }
  for (const EvidenceDisposition& disposition : entry.dispositions) {
    if (disposition.status == EvidenceStatus::StaleAuthority ||
        disposition.status == EvidenceStatus::StaleTopology) {
      explanation.authority.push_back(std::string("RETIRED EVIDENCE: observation ") +
                                      disposition.observation.value() + " is " +
                                      linkstate::to_string(disposition.status) + ": " +
                                      disposition.reason);
    }
  }
  if (!entry.dispositions.empty()) {
    explanation.rules.push_back(
        "RULE_DISPOSITION_RETENTION: " +
        detail::format_u64(static_cast<std::uint64_t>(entry.dispositions.size())) +
        " recent evidence dispositions are retained for explanation");
  }
  explanation.history = record.history;
  return explanation;
}

std::string Explanation::render() const {
  std::string out;
  out += "link                  ";
  out += link.value();
  out.push_back('\n');
  out += "state                 ";
  out += linkstate::to_string(state);
  out.push_back('\n');
  out += "state_semantics       ";
  out += describe(state);
  out.push_back('\n');
  out += "result_class          ";
  out += linkstate::to_string(result_class);
  out.push_back('\n');
  out += "degradation_cause     ";
  out += linkstate::to_string(degradation_cause);
  out.push_back('\n');
  out += "state_generation      ";
  out += state_generation.to_string();
  out.push_back('\n');
  out += "global_generation     ";
  out += global_generation.to_string();
  out.push_back('\n');
  out += "evidence_generation   ";
  out += evidence_generation.to_string();
  out.push_back('\n');
  out += "topology_generation   ";
  out += topology_generation.to_string();
  out.push_back('\n');
  for (const std::string& rule : rules) {
    out += "rule                  ";
    out += rule;
    out.push_back('\n');
  }
  for (const std::string& conflict : conflicts) {
    out += "conflict              ";
    out += conflict;
    out.push_back('\n');
  }
  for (const std::string& note : authority) {
    out += "authority             ";
    out += note;
    out.push_back('\n');
  }
  for (const EvidenceDisposition& disposition : evidence) {
    out += "evidence              observation=";
    out += disposition.observation.value();
    out += " source=";
    out += disposition.source.value();
    out += " kind=";
    out += linkstate::to_string(disposition.kind);
    out += " direction=";
    out += linkstate::to_string(disposition.direction);
    out += " claim=";
    out += linkstate::to_string(disposition.claim);
    out += " class=";
    out += linkstate::to_string(disposition.evidence_class);
    out += " status=";
    out += linkstate::to_string(disposition.status);
    out += " tier=";
    out += std::to_string(static_cast<unsigned>(disposition.precedence));
    out += " counted=";
    out += disposition.counted ? "true" : "false";
    out += " reason=";
    out += disposition.reason;
    out.push_back('\n');
  }
  for (const StateHistoryEntry& entry : history) {
    out += "history               ";
    out += entry.render();
    out.push_back('\n');
  }
  return out;
}

}  // namespace linkstate
