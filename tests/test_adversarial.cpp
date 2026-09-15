#include <algorithm>
#include <string>
#include <vector>

#include "helpers.hpp"
#include "linkstate/engine.hpp"
#include "linkstate/platform.hpp"
#include "lsf_test.hpp"

using namespace linkstate;
using lsf_test::binding_for;
using lsf_test::link_id;
using lsf_test::TestPublisher;

namespace {

EngineConfig adversarial_config() {
  EngineConfig config;
  config.max_links = 8;
  config.max_publishers = 4;
  config.max_evidence_per_link = 2;
  config.max_history_per_link = 2;
  return config;
}

PublishEvidenceRequest base_request(const PublisherId& publisher, const WorkerBootId& boot,
                                    const SourceId& source, const LinkId& link,
                                    const std::string& tag) {
  PublishEvidenceRequest publication;
  publication.publication = PublicationId::from_validated("pub." + tag);
  EvidenceRecord& observation = publication.observation;
  observation.key.link = link;
  observation.key.source = source;
  observation.key.kind = EvidenceKind::CarrierState;
  observation.observation = ObservationId::from_validated("obs." + tag);
  observation.publisher = publisher;
  observation.worker_boot = boot;
  observation.epoch = CoordinatorEpoch::from_value(0);
  observation.source_generation = SourceGeneration::from_value(1);
  observation.sequence = 1;
  observation.topology_generation = TopologyGeneration::from_value(1);
  observation.claim = EvidenceClaim::Up;
  return publication;
}

void register_agent(LinkStateEngine& engine, const PublisherId& publisher, const WorkerBootId& boot,
                    const SourceId& source, const std::vector<LinkId>& links) {
  PublisherRegistration registration;
  registration.publisher = publisher;
  registration.worker_boot = boot;
  registration.source = source;
  registration.scope.links = links;
  registration.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{registration}).outcome.code ==
            OutcomeCode::Committed);
}

}  // namespace

LSF_TEST(resource_limits_bound_every_externally_influenced_collection) {
  LinkStateEngine engine(adversarial_config());
  for (int index = 0; index < 8; ++index) {
    LSF_CHECK(engine.bind_link(BindLinkRequest{binding_for("limit." + std::to_string(index))})
                  .outcome.code == OutcomeCode::Committed);
  }
  const LinkMutationResult overflow =
      engine.bind_link(BindLinkRequest{binding_for("limit.overflow")});
  LSF_CHECK(overflow.outcome.code == OutcomeCode::ResourceLimit);

  for (int index = 0; index < 4; ++index) {
    register_agent(engine, lsf_test::publisher_id("limit-agent-" + std::to_string(index)),
                   lsf_test::boot_id("limit-boot-" + std::to_string(index)),
                   lsf_test::source_id("limit-source-" + std::to_string(index)),
                   {link_id("limit.0")});
  }
  PublisherRegistration extra;
  extra.publisher = lsf_test::publisher_id("limit-agent-extra");
  extra.worker_boot = lsf_test::boot_id("limit-boot-extra");
  extra.source = lsf_test::source_id("limit-source-extra");
  extra.scope.links = {link_id("limit.0")};
  extra.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.register_publisher(RegisterPublisherRequest{extra}).outcome.code ==
            OutcomeCode::ResourceLimit);

  const PublisherId publisher = lsf_test::publisher_id("limit-agent-0");
  const WorkerBootId boot = lsf_test::boot_id("limit-boot-0");
  const SourceId source = lsf_test::source_id("limit-source-0");
  for (int index = 0; index < 2; ++index) {
    PublishEvidenceRequest request = base_request(publisher, boot, source, link_id("limit.0"),
                                                  "limit." + std::to_string(index));
    request.observation.key.kind = static_cast<EvidenceKind>(index);
    LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  }
  PublishEvidenceRequest third =
      base_request(publisher, boot, source, link_id("limit.0"), "limit.third");
  third.observation.key.kind = EvidenceKind::DeviceReportedState;
  const LinkMutationResult exhausted = engine.publish_evidence(third);
  LSF_CHECK(exhausted.outcome.code == OutcomeCode::ResourceLimit);

  PublishEvidenceRequest oversized = base_request(publisher, boot, source, link_id("limit.1"),
                                                  "limit.oversized");
  oversized.observation.metadata.assign(limits::max_evidence_metadata_bytes + 1, std::byte{0});
  LSF_CHECK(engine.publish_evidence(oversized).outcome.code == OutcomeCode::MalformedRequest);
  oversized.observation.metadata.clear();
  oversized.observation.detail = std::string(limits::max_detail_length + 1, 'x');
  LSF_CHECK(engine.publish_evidence(oversized).outcome.code == OutcomeCode::MalformedRequest);
}

LSF_TEST(malformed_requests_are_rejected_with_specific_outcomes) {
  LinkStateEngine engine(adversarial_config());
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{binding_for("mal")}).outcome.code ==
              OutcomeCode::Committed);
  const PublisherId publisher = lsf_test::publisher_id("mal-agent");
  const WorkerBootId boot = lsf_test::boot_id("mal-boot");
  const SourceId source = lsf_test::source_id("mal-source");
  register_agent(engine, publisher, boot, source, {link_id("mal")});

  PublishEvidenceRequest request = base_request(publisher, boot, source, link_id("mal"), "mal.1");
  request.publication = PublicationId{};
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, LinkId{}, "mal.2");
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, link_id("mal"), "mal.3");
  request.observation.observation = ObservationId{};
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, link_id("missing"), "mal.4");
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::UnknownLink);

  request = base_request(publisher, boot, source, link_id("mal"), "mal.5");
  request.observation.key.source = SourceId{};
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, link_id("mal"), "mal.6");
  request.observation.detail = std::string("bad\x01text");
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, link_id("mal"), "mal.7");
  request.observation.detail = std::string("bad\xc3\x28utf8");
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  request = base_request(publisher, boot, source, link_id("mal"), "mal.8");
  request.observation.endpoint_generation = EndpointGeneration::from_value(1);
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::MalformedRequest);

  // The same observation identity may not be reused for a different key.
  request = base_request(publisher, boot, source, link_id("mal"), "mal.9");
  LSF_CHECK(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  PublishEvidenceRequest duplicate = base_request(publisher, boot, source, link_id("mal"), "mal.9");
  duplicate.observation.key.kind = EvidenceKind::PortReportedState;
  duplicate.observation.sequence = 2;
  duplicate.publication = PublicationId::from_validated("pub.mal.10");
  LSF_CHECK(engine.publish_evidence(duplicate).outcome.code == OutcomeCode::MalformedRequest);

  LinkMutationResult unknown = engine.link_state(link_id("nope")).has_value()
                                   ? LinkMutationResult{}
                                   : LinkMutationResult{};
  (void)unknown;
  RetireLinkRequest retirement;
  retirement.link = LinkId{};
  retirement.publication = PublicationId::from_validated("pub.retire");
  retirement.epoch = engine.coordinator_epoch();
  LSF_CHECK(engine.retire_link(retirement).outcome.code == OutcomeCode::MalformedRequest);

  InvalidatePublisherRequest invalidation;
  invalidation.publisher = PublisherId{};
  LSF_CHECK(engine.invalidate_publisher(invalidation).outcome.code ==
            OutcomeCode::MalformedRequest);

  AdvanceEpochRequest advance;
  advance.new_epoch = CoordinatorEpoch::from_value(0);
  LSF_CHECK(engine.advance_coordinator_epoch(advance).outcome.code == OutcomeCode::Idempotent);
}

LSF_TEST(logical_links_never_inherit_state_from_a_replaced_backing_link) {
  LinkStateEngine engine(adversarial_config());
  SyntheticLinkSpec physical;
  physical.fabric = lsf_test::fabric_id("adv");
  physical.site = lsf_test::site_id("adv");
  physical.link = link_id("physical");
  physical.local_endpoint = lsf_test::endpoint_id("physical.a");
  physical.remote_endpoint = lsf_test::endpoint_id("physical.b");
  physical.topology_generation = TopologyGeneration::from_value(1);
  physical.endpoint_generation = EndpointGeneration::from_value(1);
  physical.link_class = LinkClass::Optical;
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{to_binding(physical)}).outcome.code ==
              OutcomeCode::Committed);

  SyntheticLinkSpec logical = physical;
  logical.link = link_id("logical");
  logical.local_endpoint = lsf_test::endpoint_id("logical.a");
  logical.remote_endpoint = lsf_test::endpoint_id("logical.b");
  logical.link_class = LinkClass::Logical;
  logical.backing_links = {BackingReference{link_id("physical"), TopologyGeneration::from_value(1)}};
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{to_binding(logical)}).outcome.code ==
              OutcomeCode::Committed);

  const PublisherId publisher = lsf_test::publisher_id("logical-agent");
  const WorkerBootId boot = lsf_test::boot_id("logical-boot");
  const SourceId source = lsf_test::source_id("logical-source");
  register_agent(engine, publisher, boot, source, {link_id("logical"), link_id("physical")});
  PublishEvidenceRequest request =
      base_request(publisher, boot, source, link_id("logical"), "logical.1");
  request.observation.evidence_class = EvidenceClass::Synthetic;
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("logical"))->state() == LinkOperationalState::Up);

  SupersedeTopologyRequest supersession;
  supersession.link = link_id("physical");
  supersession.new_topology_generation = TopologyGeneration::from_value(2);
  supersession.reason = "physical optics replaced";
  LSF_REQUIRE(engine.supersede_topology(supersession).outcome.code == OutcomeCode::Committed);

  const LinkStateView logical_view = engine.link_state(link_id("logical")).value();
  LSF_CHECK(logical_view.state() == LinkOperationalState::RevalidationRequired);
  LSF_CHECK(engine.evidence_for_link(link_id("logical")).empty());
  LSF_CHECK(logical_view.record.binding.backing_links.front().generation.value() == 1);

  PublishEvidenceRequest retry =
      base_request(publisher, boot, source, link_id("logical"), "logical.2");
  retry.observation.claim = EvidenceClaim::Up;
  retry.observation.sequence = 2;
  const LinkMutationResult rejected = engine.publish_evidence(retry);
  LSF_CHECK(rejected.outcome.code == OutcomeCode::StaleTopology);
  LSF_CHECK(rejected.outcome.detail.find("backing link") != std::string::npos);

  // Rebinding the logical link to the current physical generation restores the
  // ability to publish fresh evidence.
  SyntheticLinkSpec rebound = logical;
  rebound.topology_generation = TopologyGeneration::from_value(2);
  rebound.backing_links = {BackingReference{link_id("physical"), TopologyGeneration::from_value(2)}};
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{to_binding(rebound)}).outcome.code ==
              OutcomeCode::Committed);
  PublishEvidenceRequest fresh =
      base_request(publisher, boot, source, link_id("logical"), "logical.3");
  fresh.observation.sequence = 3;
  fresh.observation.topology_generation = TopologyGeneration::from_value(2);
  LSF_CHECK(engine.publish_evidence(fresh).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("logical"))->state() == LinkOperationalState::Up);
  LSF_CHECK(engine.explain(link_id("logical")).render().find("RULE_BACKING_REFERENCE") !=
            std::string::npos);
}

LSF_TEST(retirement_of_a_backing_link_invalidates_dependents) {
  LinkStateEngine engine(adversarial_config());
  SyntheticLinkSpec physical;
  physical.fabric = lsf_test::fabric_id("adv");
  physical.site = lsf_test::site_id("adv");
  physical.link = link_id("backing");
  physical.local_endpoint = lsf_test::endpoint_id("backing.a");
  physical.remote_endpoint = lsf_test::endpoint_id("backing.b");
  physical.topology_generation = TopologyGeneration::from_value(1);
  physical.endpoint_generation = EndpointGeneration::from_value(1);
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{to_binding(physical)}).outcome.code ==
              OutcomeCode::Committed);
  SyntheticLinkSpec overlay = physical;
  overlay.link = link_id("overlay");
  overlay.local_endpoint = lsf_test::endpoint_id("overlay.a");
  overlay.remote_endpoint = lsf_test::endpoint_id("overlay.b");
  overlay.link_class = LinkClass::TunnelBacked;
  overlay.backing_links = {BackingReference{link_id("backing"), TopologyGeneration::from_value(1)}};
  LSF_REQUIRE(engine.bind_link(BindLinkRequest{to_binding(overlay)}).outcome.code ==
              OutcomeCode::Committed);

  const PublisherId publisher = lsf_test::publisher_id("overlay-agent");
  const WorkerBootId boot = lsf_test::boot_id("overlay-boot");
  const SourceId source = lsf_test::source_id("overlay-source");
  register_agent(engine, publisher, boot, source, {link_id("overlay"), link_id("backing")});
  PublishEvidenceRequest request =
      base_request(publisher, boot, source, link_id("overlay"), "overlay.1");
  request.observation.evidence_class = EvidenceClass::Synthetic;
  LSF_REQUIRE(engine.publish_evidence(request).outcome.code == OutcomeCode::Committed);

  RetireLinkRequest retirement;
  retirement.link = link_id("backing");
  retirement.topology_generation = TopologyGeneration::from_value(1);
  retirement.publication = PublicationId::from_validated("pub.retire.backing");
  retirement.publisher = publisher;
  retirement.worker_boot = boot;
  retirement.epoch = engine.coordinator_epoch();
  retirement.reason = "physical link removed";
  LSF_REQUIRE(engine.retire_link(retirement).outcome.code == OutcomeCode::Committed);
  LSF_CHECK(engine.link_state(link_id("backing"))->state() == LinkOperationalState::Retired);
  LSF_CHECK(engine.link_state(link_id("overlay"))->state() ==
            LinkOperationalState::RevalidationRequired);
}

LSF_TEST(real_host_link_discovery_produces_usable_evidence) {
  const HostDiscoveryReport report = discover_host_links();
  std::printf("  host discovery availability=%s interfaces=%zu observations=%zu unsupported=%zu\n",
              to_string(report.availability), report.interfaces_enumerated,
              report.observations.size(), report.unsupported_interfaces);
  for (const std::string& note : report.notes) {
    std::printf("  note: %s\n", note.c_str());
  }
  std::fflush(stdout);
  LSF_CHECK(report.availability != HostEvidenceAvailability::Unavailable);
  if (report.availability == HostEvidenceAvailability::Unsupported) {
    std::printf("  host discovery is UNSUPPORTED on this platform; no real evidence was produced\n");
    std::fflush(stdout);
    return;
  }
  LSF_REQUIRE(!report.observations.empty());
  for (const HostLinkObservation& observation : report.observations) {
    LSF_CHECK(observation.link.valid());
    LSF_CHECK(observation.endpoint.valid());
    LSF_CHECK(observation.device.valid());
    LSF_CHECK(observation.port.valid());
    LSF_CHECK(observation.evidence_class == EvidenceClass::Real);
    LSF_CHECK(!observation.interface_guid.empty());
    LSF_CHECK(!observation.detail.empty());
    LSF_CHECK(observation.claim != EvidenceClaim::Unknown);
    if (observation.kind == EvidenceKind::AdministrativeState) {
      LSF_CHECK(observation.claim == EvidenceClaim::Up ||
                observation.claim == EvidenceClaim::AdminDisabled);
    }
    if (observation.degradation_cause != DegradationCause::None) {
      LSF_CHECK(observation.claim == EvidenceClaim::Degraded);
    }
  }

  // Bind the discovered interfaces and publish the real evidence through the
  // public API; this is REAL host evidence, not a synthetic stand-in.
  EngineConfig host_config;
  host_config.max_links = 512;
  host_config.max_publishers = 16;
  host_config.max_evidence_per_link = 8;
  host_config.max_history_per_link = 4;
  LinkStateEngine engine(host_config);
  std::vector<LinkId> links;
  for (const HostLinkObservation& observation : report.observations) {
    if (std::find(links.begin(), links.end(), observation.link) != links.end()) {
      continue;
    }
    SyntheticLinkSpec spec;
    spec.link = observation.link;
    spec.local_endpoint = observation.endpoint;
    spec.remote_endpoint = EndpointId::from_validated(
        observation.endpoint.value() + ".remote");
    spec.topology_generation = TopologyGeneration::from_value(1);
    spec.endpoint_generation = EndpointGeneration::from_value(1);
    spec.link_class = observation.link_class;
    const LinkMutationResult bound = engine.bind_link(BindLinkRequest{to_binding(spec)});
    LSF_CHECK(bound.outcome.code == OutcomeCode::Committed ||
              bound.outcome.code == OutcomeCode::Idempotent);
    links.push_back(observation.link);
  }
  LSF_REQUIRE(!links.empty());

  const PublisherId publisher = lsf_test::publisher_id("host-platform-agent");
  const WorkerBootId boot = lsf_test::boot_id("host-platform-boot");
  const SourceId source = lsf_test::source_id("host-platform-source");
  register_agent(engine, publisher, boot, source, links);

  HostEvidencePublisher host_publisher(publisher, boot, source);
  std::size_t published = 0;
  for (int round = 0; round < 2; ++round) {
    const HostDiscoveryReport fresh = discover_host_links();
    const std::unordered_map<LinkId, HostBindingContext> contexts =
        lsf_test::binding_contexts(engine, links);
    for (const PublishEvidenceRequest& request : host_publisher.requests(fresh, contexts,
                                                                        engine.coordinator_epoch())) {
      const LinkMutationResult result = engine.publish_evidence(request);
      if (result.outcome.code != OutcomeCode::Committed &&
          result.outcome.code != OutcomeCode::Idempotent &&
          result.outcome.code != OutcomeCode::EvidenceConflict) {
        std::printf("  rejected %s (%s)\n", request.observation.key.to_string().c_str(),
                    result.outcome.to_string().c_str());
        std::fflush(stdout);
      }
      LSF_CHECK(result.outcome.code == OutcomeCode::Committed ||
                result.outcome.code == OutcomeCode::Idempotent ||
                result.outcome.code == OutcomeCode::EvidenceConflict);
      ++published;
    }
  }
  LSF_CHECK(published > 0);
  LSF_CHECK(host_publisher.round() == 2);
  LSF_CHECK(host_publisher.source_generation().value() == 3);
  LSF_CHECK(engine.evidence_count() > 0);
  std::printf("  published %zu real host evidence records across %zu interfaces\n", published,
              links.size());
  std::fflush(stdout);
  for (const LinkId& link : links) {
    const LinkStateView view = engine.link_state(link).value();
    std::printf("  %s -> %s (evidence=%zu)\n", link.value().c_str(), to_string(view.state()),
                view.current_evidence);
    LSF_CHECK(view.record.state != LinkOperationalState::Retired);
  }
  std::fflush(stdout);
}
