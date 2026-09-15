#include <algorithm>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "linkstate/engine.hpp"
#include "lsf_test.hpp"

using namespace linkstate;
using lsf_test::binding_for;
using lsf_test::link_id;
using lsf_test::TestPublisher;

namespace {

EngineConfig small_config() {
  EngineConfig config;
  config.max_links = 1000;
  config.max_publishers = 64;
  config.max_evidence_per_link = 8;
  config.max_history_per_link = 8;
  config.retain_history = true;
  return config;
}

LinkMutationResult bind_link_for(LinkStateEngine& engine, const std::string& name) {
  return engine.bind_link(BindLinkRequest{binding_for(name)});
}



}  // namespace

LSF_TEST(topology_presence_does_not_imply_up) {
  LinkStateEngine engine(small_config());
  const LinkMutationResult bound = bind_link_for(engine, "a");
  LSF_REQUIRE(bound.outcome.code == OutcomeCode::Committed);
  LSF_REQUIRE(bound.view.has_value());
  LSF_CHECK(bound.view->state() == LinkOperationalState::Unknown);
  LSF_CHECK(!bound.outcome.state_changed);
  LSF_CHECK(engine.link_count() == 1);
  LSF_CHECK(engine.evidence_count() == 0);
  const std::vector<LinkStateView> unknown = engine.links_in_state(LinkOperationalState::Unknown);
  LSF_REQUIRE(unknown.size() == 1);
  LSF_CHECK(unknown.front().link() == link_id("a"));
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).empty());
}

LSF_TEST(bind_is_idempotent_and_rejects_silent_structural_change) {
  LinkStateEngine engine(small_config());
  LSF_CHECK(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  LSF_CHECK(bind_link_for(engine, "a").outcome.code == OutcomeCode::Idempotent);
  TopologyBinding changed = binding_for("a");
  changed.endpoints.local = lsf_test::endpoint_id("a.c");
  const LinkMutationResult result = engine.bind_link(BindLinkRequest{changed});
  LSF_CHECK(result.outcome.code == OutcomeCode::MalformedRequest);
  const LinkMutationResult stale =
      engine.bind_link(BindLinkRequest{binding_for("a", 0)});
  LSF_CHECK(stale.outcome.code == OutcomeCode::MalformedRequest);
}

LSF_TEST(evidence_publication_establishes_state_and_advances_generations) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(publisher.registration_outcome().code == OutcomeCode::Committed);
  LSF_CHECK(engine.publisher_authority(publisher.publisher()).has_value());

  const LinkMutationResult published = publisher.publish("a", EvidenceClaim::Up);
  LSF_REQUIRE(published.outcome.code == OutcomeCode::Committed);
  LSF_REQUIRE(published.view.has_value());
  LSF_CHECK(published.view->state() == LinkOperationalState::Up);
  LSF_CHECK(published.outcome.state_changed);
  LSF_CHECK(published.view->record.state_generation.value() == 1);
  LSF_CHECK(published.view->record.evidence_generation.value() == 1);
  LSF_CHECK(engine.global_state_generation().value() == 1);
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).size() == 1);
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Unknown).empty());
  LSF_CHECK(engine.link_evidence_generation(link_id("a")).value() == 1);
  LSF_CHECK(engine.evidence_count() == 1);
  LSF_CHECK(!engine.links_requiring_revalidation().empty() == false);
}

LSF_TEST(exact_replay_is_idempotent_and_stale_replay_is_rejected) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  const PublishEvidenceRequest request = publisher.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  const LinkStateGeneration state_generation = engine.link_state(link_id("a"))->record.state_generation;
  const EvidenceGeneration evidence_generation =
      engine.link_evidence_generation(link_id("a"));

  const LinkMutationResult replay = engine.publish_evidence(request);
  LSF_CHECK(replay.outcome.code == OutcomeCode::Idempotent);
  LSF_CHECK(!replay.outcome.state_changed);
  LSF_CHECK(engine.link_state(link_id("a"))->record.state_generation == state_generation);
  LSF_CHECK(engine.link_evidence_generation(link_id("a")) == evidence_generation);

  PublishEvidenceRequest conflicting = request;
  conflicting.observation.claim = EvidenceClaim::Down;
  const LinkMutationResult stale = engine.publish_evidence(conflicting);
  LSF_CHECK(stale.outcome.code == OutcomeCode::StaleEvidence);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);

  PublishEvidenceRequest older = publisher.request("a", EvidenceClaim::Down);
  older.observation.sequence = request.observation.sequence - 1u;
  LSF_CHECK(engine.publish_evidence(older).outcome.code == OutcomeCode::StaleEvidence);

  PublishEvidenceRequest superseding = publisher.request("a", EvidenceClaim::Down);
  const LinkMutationResult down = engine.publish_evidence(superseding);
  LSF_REQUIRE(down.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(down.view->state() == LinkOperationalState::Down);
  LSF_CHECK(engine.link_state(link_id("a"))->record.state_generation.value() == 2);
}

LSF_TEST(missing_evidence_never_becomes_down) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  const PublishEvidenceRequest up = publisher.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(up).outcome.code == OutcomeCode::Committed);

  const std::vector<EvidenceRecord> records = engine.evidence_for_link(link_id("a"));
  LSF_REQUIRE(records.size() == 1);
  WithdrawEvidenceRequest withdrawal;
  withdrawal.key = records.front().key;
  withdrawal.observation = records.front().observation;
  withdrawal.publisher = up.observation.publisher;
  withdrawal.worker_boot = up.observation.worker_boot;
  withdrawal.epoch = up.observation.epoch;
  withdrawal.source_generation = records.front().source_generation;
  withdrawal.sequence = records.front().sequence + 1u;
  const LinkMutationResult result = engine.withdraw_evidence(withdrawal);
  LSF_REQUIRE(result.outcome.code == OutcomeCode::Committed);
  LSF_REQUIRE(result.view.has_value());
  LSF_CHECK(result.view->state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(result.view->state() != LinkOperationalState::Down);
  LSF_CHECK(result.view->state() != LinkOperationalState::Up);
  LSF_CHECK(engine.links_requiring_revalidation().size() == 1);

  const LinkMutationResult again = engine.withdraw_evidence(withdrawal);
  LSF_CHECK(again.outcome.code == OutcomeCode::NotFound);
}

LSF_TEST(withdrawal_requires_ownership_and_newer_sequence) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher first(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  TestPublisher second(engine, "agent-b", {link_id("a")}, "peer-agent", "boot-2");
  const PublishEvidenceRequest request = first.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  const EvidenceRecord record = engine.evidence_for_link(link_id("a")).front();

  WithdrawEvidenceRequest withdrawal;
  withdrawal.key = record.key;
  withdrawal.observation = record.observation;
  // A different publisher incarnation cannot withdraw evidence it does not own:
  // its registered source does not match the evidence key source.
  withdrawal.publisher = second.publisher();
  withdrawal.worker_boot = second.boot();
  withdrawal.epoch = engine.coordinator_epoch();
  withdrawal.source_generation = record.source_generation;
  withdrawal.sequence = record.sequence + 1u;
  LSF_CHECK(engine.withdraw_evidence(withdrawal).outcome.code == OutcomeCode::UnauthorizedScope);

  withdrawal.publisher = first.publisher();
  withdrawal.worker_boot = first.boot();
  withdrawal.sequence = record.sequence;
  LSF_CHECK(engine.withdraw_evidence(withdrawal).outcome.code == OutcomeCode::StaleEvidence);

  withdrawal.sequence = record.sequence + 1u;
  withdrawal.source_generation = SourceGeneration::from_value(0);
  LSF_CHECK(engine.withdraw_evidence(withdrawal).outcome.code == OutcomeCode::StaleEvidence);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);

  // Reincarnating the owning publisher fences the old boot and invalidates the
  // evidence it owned, so there is nothing left for the old incarnation to
  // withdraw: the runtime reports that the evidence key no longer exists and the
  // link conservatively requires revalidation.
  TestPublisher reincarnated(engine, "agent-a", {link_id("a")}, "port-agent", "boot-3");
  withdrawal.publisher = reincarnated.publisher();
  withdrawal.worker_boot = reincarnated.boot();
  LSF_CHECK(engine.withdraw_evidence(withdrawal).outcome.code == OutcomeCode::NotFound);
  LSF_CHECK(engine.link_state(link_id("a"))->state() ==
            LinkOperationalState::RevalidationRequired);
  const LinkMutationResult stale_publish = engine.publish_evidence(request);
  LSF_CHECK(stale_publish.outcome.code == OutcomeCode::StaleAuthority);
}

LSF_TEST(publisher_fencing_preserves_other_publisher_evidence) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher first(engine, "agent-a", {link_id("a")}, "local-port-agent", "boot-1");
  TestPublisher second(engine, "agent-b", {link_id("a")}, "peer-port-agent", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(first.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.publish_evidence(second.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);

  InvalidatePublisherRequest invalidation;
  invalidation.publisher = first.publisher();
  invalidation.worker_boot = first.boot();
  invalidation.epoch = engine.coordinator_epoch();
  invalidation.reason = "worker process died";
  const LinkMutationResult fenced = engine.invalidate_publisher(invalidation);
  LSF_REQUIRE(fenced.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.evidence_for_publisher(first.publisher()).empty());
  LSF_CHECK(engine.evidence_for_publisher(second.publisher()).size() == 1);
  LSF_CHECK(engine.publisher_authority(first.publisher())->status == AuthorityStatus::Fenced);
  LSF_CHECK(engine.fences().size() == 1);

  const LinkMutationResult replay = engine.publish_evidence(first.request("a", EvidenceClaim::Up));
  LSF_CHECK(replay.outcome.code == OutcomeCode::StaleAuthority);
  LSF_CHECK(engine.invalidate_publisher(invalidation).outcome.code == OutcomeCode::Idempotent);

  InvalidatePublisherRequest unknown;
  unknown.publisher = lsf_test::publisher_id("missing");
  unknown.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.invalidate_publisher(unknown).outcome.code ==
            OutcomeCode::UnauthorizedPublisher);
}

LSF_TEST(sole_publisher_loss_requires_revalidation) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher only(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(only.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  InvalidatePublisherRequest invalidation;
  invalidation.publisher = only.publisher();
  invalidation.worker_boot = only.boot();
  invalidation.epoch = engine.coordinator_epoch();
  const LinkMutationResult fenced = engine.invalidate_publisher(invalidation);
  LSF_REQUIRE(fenced.outcome.code == OutcomeCode::Committed);
  const std::optional<LinkStateView> view = engine.link_state(link_id("a"));
  LSF_REQUIRE(view.has_value());
  LSF_CHECK(view->state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(view->record.ever_established);
  LSF_CHECK(engine.links_requiring_revalidation().size() == 1);
}

LSF_TEST(worker_reincarnation_fences_the_previous_boot) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher first(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  const PublishEvidenceRequest old_request = first.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(old_request).outcome.code == OutcomeCode::Committed);

  TestPublisher second(engine, "agent-a", {link_id("a")}, "port-agent", "boot-2");
  LSF_CHECK(engine.link_state(link_id("a"))->state() ==
            LinkOperationalState::RevalidationRequired);
  LSF_CHECK(engine.publish_evidence(old_request).outcome.code == OutcomeCode::StaleAuthority);
  LSF_CHECK(engine.publish_evidence(second.request("a", EvidenceClaim::Up)).outcome.code ==
            OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.publisher_authority(first.publisher())->generation.value() == 2);
}

LSF_TEST(coordinator_epoch_advance_invalidates_dynamic_evidence) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  const PublishEvidenceRequest request = publisher.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);

  AdvanceEpochRequest advance;
  advance.new_epoch = CoordinatorEpoch::from_value(2);
  advance.reason = "coordinator restart";
  LSF_CHECK(engine.advance_coordinator_epoch(advance).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.coordinator_epoch().value() == 2);
  LSF_CHECK(engine.link_state(link_id("a"))->state() ==
            LinkOperationalState::RevalidationRequired);
  LSF_CHECK(engine.advance_coordinator_epoch(advance).outcome.code == OutcomeCode::Idempotent);

  AdvanceEpochRequest backwards;
  backwards.new_epoch = CoordinatorEpoch::from_value(1);
  LSF_CHECK(engine.advance_coordinator_epoch(backwards).outcome.code ==
            OutcomeCode::StaleGeneration);

  const LinkMutationResult stale = engine.publish_evidence(request);
  LSF_CHECK(stale.outcome.code == OutcomeCode::StaleCoordinatorEpoch);
  LSF_CHECK(engine.evidence_count() == 0);

  // The incarnation of the superseded epoch is permanently fenced: it may not
  // re-register, even with the current epoch.
  PublisherRegistration registration;
  registration.publisher = publisher.publisher();
  registration.worker_boot = publisher.boot();
  registration.source = publisher.source();
  registration.scope.links = {link_id("a")};
  registration.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
            OutcomeCode::StaleAuthority);

  // A fresh incarnation presenting the current epoch is accepted.
  registration.worker_boot = lsf_test::boot_id("boot-2");
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
            OutcomeCode::Committed);

  // Traffic from the previous epoch is rejected.
  registration.worker_boot = lsf_test::boot_id("boot-3");
  registration.epoch = CoordinatorEpoch::from_value(1);
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
            OutcomeCode::StaleCoordinatorEpoch);
}

LSF_TEST(authority_scope_is_enforced) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  LSF_REQUIRE(bind_link_for(engine, "b").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_CHECK(engine.publish_evidence(publisher.request("b", EvidenceClaim::Up)).outcome.code ==
            OutcomeCode::UnauthorizedScope);

  PublishEvidenceRequest wrong_source = publisher.request("a", EvidenceClaim::Up);
  wrong_source.observation.key.source = lsf_test::source_id("other-agent");
  LSF_CHECK(engine.publish_evidence(wrong_source).outcome.code == OutcomeCode::UnauthorizedScope);

  PublishEvidenceRequest unknown_publisher = publisher.request("a", EvidenceClaim::Up);
  unknown_publisher.observation.publisher = lsf_test::publisher_id("stranger");
  LSF_CHECK(engine.publish_evidence(unknown_publisher).outcome.code ==
            OutcomeCode::UnauthorizedPublisher);

  PublisherRegistration empty_scope;
  empty_scope.publisher = lsf_test::publisher_id("empty");
  empty_scope.worker_boot = lsf_test::boot_id("boot-empty");
  empty_scope.source = lsf_test::source_id("empty-source");
  empty_scope.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{empty_scope}).outcome.code ==
            OutcomeCode::ResourceLimit);
}

LSF_TEST(compare_and_swap_generations_reject_before_mutation) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  PublishEvidenceRequest request = publisher.request("a", EvidenceClaim::Up);
  request.expected_evidence_generation = EvidenceGeneration::from_value(4);
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::StaleGeneration);
  LSF_CHECK(engine.evidence_count() == 0);

  PublishEvidenceRequest second = publisher.request("a", EvidenceClaim::Up);
  second.expected_evidence_generation = EvidenceGeneration::from_value(0);
  second.expected_state_generation = LinkStateGeneration::from_value(0);
  LSF_CHECK(engine.publish_evidence(second).outcome.code == OutcomeCode::Committed);
  PublishEvidenceRequest third = publisher.request("a", EvidenceClaim::Down);
  third.expected_state_generation = LinkStateGeneration::from_value(0);
  LSF_CHECK(engine.publish_evidence(third).outcome.code == OutcomeCode::StaleGeneration);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);
}

LSF_TEST(administrative_intent_outranks_operational_evidence) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);

  TransitionStateRequest disable;
  disable.link = link_id("a");
  disable.target = LinkOperationalState::AdminDisabled;
  disable.publication = PublicationId::from_validated("pub.disable");
  disable.publisher = publisher.publisher();
  disable.worker_boot = publisher.boot();
  disable.epoch = engine.coordinator_epoch();
  disable.reason = "operator disabled the link";
  const LinkMutationResult disabled = engine.transition_state(disable);
  LSF_REQUIRE(disabled.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(disabled.view->state() == LinkOperationalState::AdminDisabled);
  LSF_CHECK(disabled.view->record.admin_intent.has_value());
  LSF_CHECK(disabled.view->record.admin_intent->state == LinkOperationalState::AdminDisabled);

  const LinkMutationResult stale_up = engine.publish_evidence(publisher.request("a", EvidenceClaim::Up));
  LSF_REQUIRE(stale_up.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(stale_up.view->state() == LinkOperationalState::AdminDisabled);

  TransitionStateRequest enable;
  enable.link = link_id("a");
  enable.target = LinkOperationalState::Up;
  enable.publication = PublicationId::from_validated("pub.enable");
  enable.publisher = publisher.publisher();
  enable.worker_boot = publisher.boot();
  enable.epoch = engine.coordinator_epoch();
  const LinkMutationResult enabled = engine.transition_state(enable);
  LSF_REQUIRE(enabled.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(enabled.view->state() == LinkOperationalState::Up);
  LSF_CHECK(!enabled.view->record.admin_intent.has_value());

  TransitionStateRequest illegal;
  illegal.link = link_id("a");
  illegal.target = LinkOperationalState::Unknown;
  illegal.publication = PublicationId::from_validated("pub.unknown");
  illegal.publisher = publisher.publisher();
  illegal.worker_boot = publisher.boot();
  illegal.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.transition_state(illegal).outcome.code == OutcomeCode::InvalidTransition);

  illegal.target = LinkOperationalState::RevalidationRequired;
  LSF_CHECK(engine.transition_state(illegal).outcome.code == OutcomeCode::InvalidTransition);
}

LSF_TEST(topology_supersession_invalidates_operational_state) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  const PublishEvidenceRequest request = publisher.request("a", EvidenceClaim::Up);
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);

  SupersedeTopologyRequest supersession;
  supersession.link = link_id("a");
  supersession.new_topology_generation = TopologyGeneration::from_value(2);
  supersession.reason = "endpoint replaced";
  TopologyBinding replacement = binding_for("a", 2, 2);
  replacement.endpoints.local = lsf_test::endpoint_id("a.a2");
  supersession.replacement = replacement;
  const LinkMutationResult superseded = engine.supersede_topology(supersession);
  LSF_REQUIRE(superseded.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(superseded.view->state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(superseded.view->record.binding.topology_generation.value() == 2);
  LSF_CHECK(superseded.view->record.binding.endpoints.local ==
            lsf_test::endpoint_id("a.a2"));

  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::StaleTopology);

  PublishEvidenceRequest fresh = publisher.request("a", EvidenceClaim::Up);
  LSF_CHECK(engine.publish_evidence(fresh).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("a"))->state() == LinkOperationalState::Up);

  SupersedeTopologyRequest backwards;
  backwards.link = link_id("a");
  backwards.new_topology_generation = TopologyGeneration::from_value(1);
  LSF_CHECK(engine.supersede_topology(backwards).outcome.code == OutcomeCode::StaleTopology);

  SupersedeTopologyRequest unknown;
  unknown.link = link_id("missing");
  unknown.new_topology_generation = TopologyGeneration::from_value(2);
  LSF_CHECK(engine.supersede_topology(unknown).outcome.code == OutcomeCode::UnknownLink);
}

LSF_TEST(endpoint_generation_mismatch_is_stale_topology) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  PublishEvidenceRequest request = publisher.request("a", EvidenceClaim::Up);
  request.observation.key.endpoint = lsf_test::endpoint_id("a.a");
  request.observation.endpoint_generation = EndpointGeneration::from_value(9);
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::StaleTopology);

  request.observation.key.endpoint = lsf_test::endpoint_id("a.unknown");
  request.observation.endpoint_generation = EndpointGeneration::from_value(1);
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request.observation.key.endpoint = lsf_test::endpoint_id("a.a");
  request.observation.endpoint_generation = EndpointGeneration::from_value(1);
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
}

LSF_TEST(retirement_is_terminal) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);

  RetireLinkRequest retirement;
  retirement.link = link_id("a");
  retirement.topology_generation = TopologyGeneration::from_value(1);
  retirement.publication = PublicationId::from_validated("pub.retire");
  retirement.publisher = publisher.publisher();
  retirement.worker_boot = publisher.boot();
  retirement.epoch = engine.coordinator_epoch();
  retirement.reason = "edge removed from Fabric Topology";
  const LinkMutationResult retired = engine.retire_link(retirement);
  LSF_REQUIRE(retired.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(retired.view->state() == LinkOperationalState::Retired);
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Retired).size() == 1);
  LSF_CHECK(engine.links_in_state(LinkOperationalState::Up).empty());
  LSF_CHECK(engine.retire_link(retirement).outcome.code == OutcomeCode::Idempotent);
  LSF_CHECK(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
            OutcomeCode::RetiredLink);
  LSF_CHECK(bind_link_for(engine, "a").outcome.code == OutcomeCode::RetiredLink);

  RetireLinkRequest unauthorized = retirement;
  unauthorized.worker_boot = lsf_test::boot_id("boot-other");
  LSF_CHECK(engine.retire_link(unauthorized).outcome.code == OutcomeCode::StaleAuthority);
}

LSF_TEST(marking_revalidation_invalidates_only_the_callers_evidence) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher first(engine, "agent-a", {link_id("a")}, "local", "boot-1");
  TestPublisher second(engine, "agent-b", {link_id("a")}, "peer", "boot-2");
  LSF_REQUIRE(engine.publish_evidence(first.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.publish_evidence(second.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);

  MarkRevalidationRequest mark;
  mark.link = link_id("a");
  mark.publication = PublicationId::from_validated("pub.revalidate");
  mark.publisher = first.publisher();
  mark.worker_boot = first.boot();
  mark.epoch = engine.coordinator_epoch();
  mark.reason = "local agent lost its view";
  const LinkMutationResult marked = engine.mark_revalidation_required(mark);
  LSF_REQUIRE(marked.outcome.code == OutcomeCode::Committed);
  LSF_CHECK(marked.view->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.evidence_for_publisher(second.publisher()).size() == 1);

  MarkRevalidationRequest again = mark;
  LSF_CHECK(engine.mark_revalidation_required(again).outcome.code == OutcomeCode::NoChange);
}

LSF_TEST(queries_and_indexes_stay_consistent) {
  LinkStateEngine engine(small_config());
  for (int index = 0; index < 6; ++index) {
    LSF_REQUIRE(bind_link_for(engine, "q" + std::to_string(index)).outcome.code == OutcomeCode::Committed);
  }
  std::vector<LinkId> scope;
  for (int index = 0; index < 6; ++index) {
    scope.push_back(link_id("q" + std::to_string(index)));
  }
  TestPublisher publisher(engine, "agent-q", scope, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("q0", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.publish_evidence(publisher.request("q1", EvidenceClaim::Down)).outcome.code ==
              OutcomeCode::Committed);
  LSF_REQUIRE(engine.publish_evidence(publisher.request("q2", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);

  std::size_t total = 0;
  for (std::size_t index = 0; index < link_operational_state_count; ++index) {
    total += engine.links_in_state(static_cast<LinkOperationalState>(index)).size();
  }
  LSF_CHECK_EQ(total, std::size_t{6});
  LSF_CHECK_EQ(engine.links_in_state(LinkOperationalState::Up).size(), std::size_t{2});
  LSF_CHECK_EQ(engine.links_in_state(LinkOperationalState::Down).size(), std::size_t{1});
  LSF_CHECK_EQ(engine.links_in_state(LinkOperationalState::Unknown).size(), std::size_t{3});

  const std::optional<LinkStateView> by_endpoints =
      engine.link_by_endpoints(lsf_test::endpoint_id("q0.a"), lsf_test::endpoint_id("q0.b"));
  LSF_REQUIRE(by_endpoints.has_value());
  LSF_CHECK(by_endpoints->link() == link_id("q0"));
  LSF_CHECK(!engine.link_by_endpoints(lsf_test::endpoint_id("q0.a"),
                                      lsf_test::endpoint_id("missing"))
                 .has_value());
  LSF_CHECK_EQ(engine.links_for_endpoint(lsf_test::endpoint_id("q0.a")).size(), std::size_t{1});
  LSF_CHECK_EQ(engine.evidence_for_publisher(publisher.publisher()).size(), std::size_t{3});
  LSF_CHECK_EQ(engine.publishers().size(), std::size_t{1});
  LSF_CHECK(engine.publishers().front().live_evidence == 3);
  LSF_CHECK(!engine.record(link_id("missing")).has_value());
  LSF_CHECK(!engine.link_state(link_id("missing")).has_value());

  const Digest before = engine.state_digest();
  LSF_CHECK(engine.state_digest() == before);
  const EngineStats stats = engine.stats();
  LSF_CHECK_EQ(stats.links, std::size_t{6});
  LSF_CHECK_EQ(stats.current_evidence, std::size_t{3});
  LSF_CHECK_EQ(stats.active_publishers, std::size_t{1});
  LSF_CHECK(stats.rejected_publications == 0);

  const Outcome reconciled = engine.reconcile();
  LSF_CHECK(reconciled.code == OutcomeCode::Committed);
  LSF_CHECK(engine.state_digest() == before);
}

LSF_TEST(explanations_are_deterministic_and_complete) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  const Explanation first = engine.explain(link_id("a"));
  const Explanation second = engine.explain(link_id("a"));
  LSF_CHECK(first.render() == second.render());
  LSF_CHECK(first.state == LinkOperationalState::Up);
  LSF_CHECK(!first.rules.empty());
  LSF_CHECK_EQ(first.evidence.size(), std::size_t{1});
  LSF_CHECK(first.evidence.front().counted);
  LSF_CHECK(first.evidence.front().reason.find("counted at precedence tier") != std::string::npos);
  LSF_CHECK(!first.history.empty());

  const Explanation missing = engine.explain(link_id("missing"));
  LSF_CHECK(missing.render().find("RULE_UNKNOWN_LINK") != std::string::npos);
}

LSF_TEST(batch_operations_are_bounded_and_ordered) {
  LinkStateEngine engine(small_config());
  std::vector<BindLinkRequest> requests;
  for (int index = 0; index < 4; ++index) {
    requests.push_back(BindLinkRequest{binding_for("batch" + std::to_string(index))});
  }
  const std::vector<LinkMutationResult> results = engine.bind_links(requests);
  LSF_REQUIRE(results.size() == 4);
  for (const LinkMutationResult& result : results) {
    LSF_CHECK(result.outcome.code == OutcomeCode::Committed);
  }
  std::vector<BindLinkRequest> oversized(engine.config().max_batch_size + 1,
                                        BindLinkRequest{binding_for("oversized")});
  const std::vector<LinkMutationResult> rejected = engine.bind_links(oversized);
  LSF_REQUIRE(rejected.size() == 1);
  LSF_CHECK(rejected.front().outcome.code == OutcomeCode::ResourceLimit);
}

LSF_TEST(move_construction_preserves_engine_state) {
  LinkStateEngine engine(small_config());
  LSF_REQUIRE(bind_link_for(engine, "a").outcome.code == OutcomeCode::Committed);
  TestPublisher publisher(engine, "agent-a", {link_id("a")}, "port-agent", "boot-1");
  LSF_REQUIRE(engine.publish_evidence(publisher.request("a", EvidenceClaim::Up)).outcome.code ==
              OutcomeCode::Committed);
  const Digest digest = engine.state_digest();
  LinkStateEngine moved(std::move(engine));
  LSF_CHECK(moved.link_count() == 1);
  LSF_CHECK(moved.state_digest() == digest);
  LinkStateEngine assigned(small_config());
  assigned = std::move(moved);
  LSF_CHECK(assigned.link_state(link_id("a"))->state() == LinkOperationalState::Up);
}
