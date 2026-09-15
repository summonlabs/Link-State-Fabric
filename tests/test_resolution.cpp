#include <string>
#include <vector>

#include "helpers.hpp"
#include "linkstate/engine.hpp"
#include "lsf_test.hpp"

using namespace linkstate;
using lsf_test::binding_for;
using lsf_test::endpoint_id;
using lsf_test::link_id;
using lsf_test::TestPublisher;

namespace {

EngineConfig resolution_config() {
  EngineConfig config;
  config.max_links = 64;
  config.max_publishers = 16;
  config.max_evidence_per_link = 16;
  config.max_history_per_link = 16;
  return config;
}

LinkMutationResult bind_kind(LinkStateEngine& engine, const std::string& name, LinkClass link_class) {
  return engine.bind_link(BindLinkRequest{binding_for(name, 1, 1, link_class)});
}

PublishEvidenceRequest scoped(TestPublisher& publisher, const std::string& link,
                              EvidenceKind kind, EvidenceClaim claim,
                              DegradationCause cause = DegradationCause::None,
                              EvidenceDirection direction = EvidenceDirection::Bidirectional,
                              const EndpointId& endpoint = EndpointId{}) {
  PublishEvidenceRequest request = publisher.request(link, claim, kind, cause);
  request.observation.key.direction = direction;
  request.observation.key.endpoint = endpoint;
  if (endpoint.valid()) {
    const std::optional<TopologyBinding> binding = publisher.engine_binding(link);
    if (binding.has_value()) {
      request.observation.endpoint_generation = endpoint == binding->endpoints.local
                                                    ? binding->endpoints.local_generation
                                                    : binding->endpoints.remote_generation;
    }
  }
  return request;
}

}  // namespace

LSF_TEST(contradictory_authoritative_evidence_is_conflicted_not_averaged) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "c", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher local(engine, "local", {link_id("c")}, "local-port", "boot-1");
  TestPublisher remote(engine, "remote", {link_id("c")}, "peer-port", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(scoped(local, "c", EvidenceKind::CarrierState,
                                             EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult conflicted = engine.publish_evidence(
      scoped(remote, "c", EvidenceKind::CarrierState, EvidenceClaim::Down));
  LSF_CHECK(conflicted.outcome.code == OutcomeCode::EvidenceConflict);
  LSF_REQUIRE(conflicted.view.has_value());
  LSF_CHECK(conflicted.view->state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(conflicted.view->conflicted);
  LSF_CHECK(conflicted.view->record.result_class == StateResultClass::Conflicted);
  LSF_CHECK(engine.stats().conflicts == 1);

  const Explanation explanation = engine.explain(link_id("c"));
  LSF_CHECK(!explanation.conflicts.empty());
  LSF_CHECK(explanation.render().find("RULE_OPERATIONALITY_CONFLICT") != std::string::npos);
  LSF_CHECK(explanation.evidence.size() == 2);
}

LSF_TEST(impairment_evidence_wins_over_an_unimpaired_claim) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "d", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher first(engine, "one", {link_id("d")}, "source-one", "boot-1");
  TestPublisher second(engine, "two", {link_id("d")}, "source-two", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(scoped(first, "d", EvidenceKind::CarrierState,
                                             EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult degraded = engine.publish_evidence(scoped(
      second, "d", EvidenceKind::LossErrorThreshold, EvidenceClaim::Degraded,
      DegradationCause::ErrorRateThresholdExceeded));
  LSF_REQUIRE(degraded.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(degraded.view->state() == LinkOperationalState::Degraded);
  LSF_CHECK(degraded.view->record.degradation_cause ==
            DegradationCause::ErrorRateThresholdExceeded);
  LSF_CHECK(!degraded.view->conflicted);
}

LSF_TEST(specific_fault_is_reported_over_a_generic_down) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "e", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher first(engine, "one", {link_id("e")}, "source-one", "boot-1");
  TestPublisher second(engine, "two", {link_id("e")}, "source-two", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(scoped(first, "e", EvidenceKind::CarrierState,
                                             EvidenceClaim::Down))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult faulted = engine.publish_evidence(
      scoped(second, "e", EvidenceKind::DeviceReportedState, EvidenceClaim::Faulted));
  LSF_REQUIRE(faulted.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(faulted.view->state() == LinkOperationalState::Faulted);
}

LSF_TEST(hardware_fault_precedence_outranks_local_operational_claims) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "f", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher carrier(engine, "carrier", {link_id("f")}, "carrier-source", "boot-1");
  TestPublisher hardware(engine, "hardware", {link_id("f")}, "hardware-source", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(
                  scoped(carrier, "f", EvidenceKind::CarrierState, EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult faulted = engine.publish_evidence(
      scoped(hardware, "f", EvidenceKind::HardwareErrorIndication, EvidenceClaim::Faulted));
  LSF_REQUIRE(faulted.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(faulted.view->state() == LinkOperationalState::Faulted);
  const Explanation explanation = engine.explain(link_id("f"));
  bool saw_outranked = false;
  for (const EvidenceDisposition& disposition : explanation.evidence) {
    if (disposition.kind == EvidenceKind::CarrierState) {
      LSF_CHECK(!disposition.counted);
      LSF_CHECK(disposition.reason.find("outranked by precedence tier 2") != std::string::npos);
      saw_outranked = true;
    }
  }
  LSF_CHECK(saw_outranked);
}

LSF_TEST(administrative_evidence_gates_operational_claims) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "g", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher operational(engine, "oper", {link_id("g")}, "oper-source", "boot-1");
  TestPublisher admin(engine, "admin", {link_id("g")}, "admin-source", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(
                  scoped(operational, "g", EvidenceKind::InterfaceOperationalState,
                         EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult disabled = engine.publish_evidence(scoped(
      admin, "g", EvidenceKind::AdministrativeState, EvidenceClaim::AdminDisabled));
  LSF_REQUIRE(disabled.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(disabled.view->state() == LinkOperationalState::AdminDisabled);

  const LinkMutationResult later_up = engine.publish_evidence(
      scoped(operational, "g", EvidenceKind::CarrierState, EvidenceClaim::Up));
  LSF_REQUIRE(later_up.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(later_up.view->state() == LinkOperationalState::AdminDisabled);
  const Explanation explanation = engine.explain(link_id("g"));
  LSF_CHECK(explanation.render().find("RULE_ADMIN_DISABLE_GATE") != std::string::npos);

  TestPublisher admin_two(engine, "admin2", {link_id("g")}, "admin-source-2", "boot-3");
  const LinkMutationResult drained = engine.publish_evidence(
      scoped(admin_two, "g", EvidenceKind::AdministrativeState, EvidenceClaim::Draining));
  LSF_REQUIRE(drained.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(drained.view->state() == LinkOperationalState::AdminDisabled);
  LSF_CHECK(!drained.view->conflicted);
  LSF_CHECK(!engine.explain(link_id("g")).conflicts.empty());
}

LSF_TEST(administrative_enablement_is_a_constraint_not_a_claim) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "h", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("h")}, "agent-source", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(
                  scoped(agent, "h", EvidenceKind::AdministrativeState, EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("h"))->state() == LinkOperationalState::Unknown);
  const LinkMutationResult down = engine.publish_evidence(
      scoped(agent, "h", EvidenceKind::InterfaceOperationalState, EvidenceClaim::Down));
  LSF_REQUIRE(down.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(down.view->state() == LinkOperationalState::Down);
}

LSF_TEST(directional_asymmetry_is_degraded_on_optical_links) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "i", LinkClass::Optical).outcome.code == OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("i")}, "optical-agent", "boot-1");
  const std::optional<TopologyBinding> binding = engine.topology_binding(link_id("i"));
  LSF_REQUIRE(binding.has_value());
  LSF_REQUIRE(engine.publish_evidence(scoped(agent, "i", EvidenceKind::CarrierState,
                                             EvidenceClaim::Up, DegradationCause::None,
                                             EvidenceDirection::Transmit,
                                             binding->endpoints.local))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult asymmetric = engine.publish_evidence(
      scoped(agent, "i", EvidenceKind::CarrierState, EvidenceClaim::Down,
             DegradationCause::None, EvidenceDirection::Receive, binding->endpoints.local));
  LSF_REQUIRE(asymmetric.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(asymmetric.view->state() == LinkOperationalState::Degraded);
  LSF_CHECK(asymmetric.view->record.degradation_cause == DegradationCause::Asymmetry);
  LSF_CHECK(asymmetric.view->record.result_class == StateResultClass::AuthoritativeDegraded);
}

LSF_TEST(directional_contradiction_on_a_symmetric_link_is_a_conflict) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "j", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("j")}, "ethernet-agent", "boot-1");
  const std::optional<TopologyBinding> binding = engine.topology_binding(link_id("j"));
  LSF_REQUIRE(binding.has_value());
  LSF_REQUIRE(engine.publish_evidence(scoped(agent, "j", EvidenceKind::CarrierState,
                                             EvidenceClaim::Up, DegradationCause::None,
                                             EvidenceDirection::Transmit,
                                             binding->endpoints.local))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult conflicted = engine.publish_evidence(
      scoped(agent, "j", EvidenceKind::CarrierState, EvidenceClaim::Down,
             DegradationCause::None, EvidenceDirection::Receive, binding->endpoints.local));
  LSF_CHECK(conflicted.outcome.code == OutcomeCode::EvidenceConflict);
  LSF_CHECK(conflicted.view->state() == LinkOperationalState::RevalidationRequired);
}

LSF_TEST(remote_peer_contradiction_is_never_silently_discarded) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "k", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher local(engine, "local", {link_id("k")}, "local-source", "boot-1");
  TestPublisher peer(engine, "peer", {link_id("k")}, "peer-source", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(
                  scoped(local, "k", EvidenceKind::CarrierState, EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult contradicted = engine.publish_evidence(
      scoped(peer, "k", EvidenceKind::RemoteEndpointState, EvidenceClaim::Down));
  LSF_CHECK(contradicted.outcome.code == OutcomeCode::EvidenceConflict);
  LSF_CHECK(contradicted.view->state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(engine.explain(link_id("k")).render().find("RULE_REMOTE_CONFLICT") !=
            std::string::npos);

  LinkStateEngine optical(resolution_config());
  LSF_REQUIRE(bind_kind(optical, "l", LinkClass::Optical).outcome.code == OutcomeCode::Committed);
  TestPublisher optical_local(optical, "local", {link_id("l")}, "local-source", "boot-1");
  TestPublisher optical_peer(optical, "peer", {link_id("l")}, "peer-source", "boot-2");
  LSF_REQUIRE(optical.publish_evidence(
                  scoped(optical_local, "l", EvidenceKind::CarrierState, EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult asymmetric = optical.publish_evidence(
      scoped(optical_peer, "l", EvidenceKind::RemoteEndpointState, EvidenceClaim::Down));
  LSF_REQUIRE(asymmetric.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(asymmetric.view->state() == LinkOperationalState::Degraded);
  LSF_CHECK(asymmetric.view->record.degradation_cause == DegradationCause::Asymmetry);
}

LSF_TEST(remote_corroboration_is_recorded) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "m", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher local(engine, "local", {link_id("m")}, "local-source", "boot-1");
  TestPublisher peer(engine, "peer", {link_id("m")}, "peer-source", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(
                  scoped(local, "m", EvidenceKind::CarrierState, EvidenceClaim::Up))
                  .outcome.code == OutcomeCode::Committed);
  const LinkMutationResult corroborated = engine.publish_evidence(
      scoped(peer, "m", EvidenceKind::RemoteEndpointState, EvidenceClaim::Up));
  LSF_REQUIRE(corroborated.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(corroborated.view->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.explain(link_id("m")).render().find("RULE_REMOTE_CORROBORATION") !=
            std::string::npos);
}

LSF_TEST(synthetic_evidence_never_outranks_real_evidence) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "n", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher real(engine, "real", {link_id("n")}, "real-source", "boot-1");
  TestPublisher synthetic(engine, "synth", {link_id("n")}, "synthetic-source", "boot-2");
  PublishEvidenceRequest synthetic_request = scoped(synthetic, "n", EvidenceKind::CarrierState,
                                                    EvidenceClaim::Up);
  synthetic_request.observation.evidence_class = EvidenceClass::Synthetic;
  LSF_REQUIRE(engine.publish_evidence(synthetic_request).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("n"))->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.link_state(link_id("n"))->synthetic_backed);

  const LinkMutationResult down = engine.publish_evidence(
      scoped(real, "n", EvidenceKind::CarrierState, EvidenceClaim::Down));
  LSF_REQUIRE(down.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(down.view->state() == LinkOperationalState::Down);
  LSF_CHECK(!down.view->synthetic_backed);
}

LSF_TEST(unsupported_capability_evidence_is_rejected) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "o", LinkClass::InfiniBandLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("o")}, "ib-agent", "boot-1");
  PublishEvidenceRequest request = scoped(agent, "o", EvidenceKind::DeviceReportedState,
                                          EvidenceClaim::Up);
  request.observation.evidence_class = EvidenceClass::Unsupported;
  const LinkMutationResult rejected = engine.publish_evidence(request);
  LSF_CHECK(rejected.outcome.code == OutcomeCode::MalformedRequest);
  LSF_CHECK(rejected.outcome.detail.find("unsupported capability") != std::string::npos);
  LSF_CHECK(engine.evidence_count() == 0);
}

LSF_TEST(a_lone_unknown_claim_leaves_the_link_unknown) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "p", LinkClass::Logical).outcome.code == OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("p")}, "logical-agent", "boot-1");
  const LinkMutationResult unknown = engine.publish_evidence(
      scoped(agent, "p", EvidenceKind::DeviceReportedState, EvidenceClaim::Unknown));
  LSF_REQUIRE(unknown.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(unknown.view->state() == LinkOperationalState::Unknown);
  LSF_CHECK(!unknown.outcome.state_changed);
  LSF_CHECK(engine.evidence_count() == 1);

  const LinkMutationResult up = engine.publish_evidence(
      scoped(agent, "p", EvidenceKind::DeviceReportedState, EvidenceClaim::Up,
             DegradationCause::None, EvidenceDirection::Bidirectional));
  LSF_REQUIRE(up.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(up.view->state() == LinkOperationalState::Up);

  TestPublisher other(engine, "other", {link_id("p")}, "other-source", "boot-2");
  const LinkMutationResult conflicted = engine.publish_evidence(
      scoped(other, "p", EvidenceKind::DeviceReportedState, EvidenceClaim::Down));
  LSF_CHECK(conflicted.outcome.code == OutcomeCode::EvidenceConflict);
  LSF_CHECK(conflicted.view->state() == LinkOperationalState::RevalidationRequired);
}

LSF_TEST(degraded_claims_require_a_named_cause) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "q", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("q")}, "agent-source", "boot-1");
  PublishEvidenceRequest missing_cause = scoped(agent, "q", EvidenceKind::CarrierState,
                                                EvidenceClaim::Degraded);
  const LinkMutationResult rejected = engine.publish_evidence(missing_cause);
  LSF_CHECK(rejected.outcome.code == OutcomeCode::MalformedRequest);
  LSF_CHECK(rejected.outcome.detail.find("degradation cause") != std::string::npos);

  PublishEvidenceRequest wrong_cause = scoped(agent, "q", EvidenceKind::CarrierState,
                                              EvidenceClaim::Up,
                                              DegradationCause::UnstableCarrier);
  LSF_CHECK(engine.publish_evidence(wrong_cause).outcome.code == OutcomeCode::MalformedRequest);

  PublishEvidenceRequest degraded = scoped(agent, "q", EvidenceKind::CarrierState,
                                           EvidenceClaim::Degraded,
                                           DegradationCause::UnstableCarrier);
  LSF_REQUIRE(engine.publish_evidence(degraded).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("q"))->state() == LinkOperationalState::Degraded);
  LSF_CHECK(engine.link_state(link_id("q"))->record.degradation_cause ==
            DegradationCause::UnstableCarrier);
}

LSF_TEST(resolution_is_order_independent) {
  const auto run = [](bool first_up) {
    LinkStateEngine engine(resolution_config());
    (void)bind_kind(engine, "r", LinkClass::EthernetLike);
    TestPublisher one(engine, "one", {link_id("r")}, "source-one", "boot-1");
    TestPublisher two(engine, "two", {link_id("r")}, "source-two", "boot-2");
    const PublishEvidenceRequest up = scoped(one, "r", EvidenceKind::CarrierState,
                                             EvidenceClaim::Up);
    const PublishEvidenceRequest degraded = scoped(two, "r", EvidenceKind::LossErrorThreshold,
                                                   EvidenceClaim::Degraded,
                                                   DegradationCause::PartialLaneFailure);
    if (first_up) {
      (void)engine.publish_evidence(up);
      (void)engine.publish_evidence(degraded);
    } else {
      (void)engine.publish_evidence(degraded);
      (void)engine.publish_evidence(up);
    }
    return engine.link_state(link_id("r"))->record;
  };
  const LinkStateRecord forward = run(true);
  const LinkStateRecord reverse = run(false);
  LSF_CHECK(forward.state == reverse.state);
  LSF_CHECK(forward.state == LinkOperationalState::Degraded);
  LSF_CHECK(forward.degradation_cause == reverse.degradation_cause);
  LSF_CHECK(forward.result_class == reverse.result_class);
  // The authoritative commitment is the resolved state bound to its evidence
  // set, not the number of transitions it took to get there.
  LSF_CHECK(forward.provenance_digest == reverse.provenance_digest);
}

LSF_TEST(source_generation_advance_supersedes_older_evidence_of_the_same_source) {
  LinkStateEngine engine(resolution_config());
  LSF_REQUIRE(bind_kind(engine, "s", LinkClass::EthernetLike).outcome.code ==
              OutcomeCode::Committed);
  TestPublisher agent(engine, "agent", {link_id("s")}, "agent-source", "boot-1");
  PublishEvidenceRequest first = scoped(agent, "s", EvidenceKind::CarrierState,
                                        EvidenceClaim::Down);
  LSF_REQUIRE(engine.publish_evidence(first).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("s"))->state() == LinkOperationalState::Down);

  // A newer observation of the same evidence key supersedes the old record.
  PublishEvidenceRequest same_key = scoped(agent, "s", EvidenceKind::CarrierState,
                                           EvidenceClaim::Up);
  same_key.observation.source_generation = SourceGeneration::from_value(1);
  const LinkMutationResult upgraded = engine.publish_evidence(same_key);
  LSF_REQUIRE(upgraded.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(upgraded.view->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.evidence_count() == 1);
  LSF_CHECK(engine.explain(link_id("s")).render().find("SUPERSEDED") != std::string::npos);

  // A newer source incarnation invalidates every remaining record of the older
  // incarnation, even across different evidence kinds.
  PublishEvidenceRequest new_incarnation = scoped(agent, "s", EvidenceKind::DeviceReportedState,
                                                  EvidenceClaim::Up);
  new_incarnation.observation.source_generation = SourceGeneration::from_value(2);
  new_incarnation.observation.key.direction = EvidenceDirection::Bidirectional;
  const LinkMutationResult advanced = engine.publish_evidence(new_incarnation);
  LSF_REQUIRE(advanced.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.evidence_count() == 1);
  const Explanation explanation = engine.explain(link_id("s"));
  LSF_CHECK(explanation.render().find("STALE_SOURCE_GENERATION") != std::string::npos);
  // One current record plus the two retained dispositions that explain why the
  // earlier observations stopped being current.
  LSF_CHECK_EQ(explanation.evidence.size(), std::size_t{3});
}
