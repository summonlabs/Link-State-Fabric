#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "linkstate/engine.hpp"
#include "linkstate/export.hpp"
#include "linkstate/record.hpp"
#include "linkstate/state.hpp"

namespace linkstate {

/// One synthetic link description.
struct LSF_EXPORT SyntheticLinkSpec {
  LinkId link;
  EndpointId local_endpoint;
  EndpointId remote_endpoint;
  FabricId fabric;
  SiteId site;
  LinkClass link_class = LinkClass::SwitchToSwitch;
  LinkSymmetryGuarantee symmetry = LinkSymmetryGuarantee::Unspecified;
  TopologyGeneration topology_generation;
  EndpointGeneration endpoint_generation;
  std::vector<BackingReference> backing_links;
  std::vector<LinkId> member_links;
};

/// Description of a synthetic fabric topology.
struct LSF_EXPORT SyntheticFabricSpec {
  FabricId fabric;
  SiteId site;
  std::string name = "synthetic";
  std::size_t switch_count = 2;
  std::size_t links_per_switch_pair = 2;
  /// Sites to distribute switches across. One means a single site fabric.
  std::size_t site_count = 1;
  LinkClass link_class = LinkClass::SwitchToSwitch;
  LinkSymmetryGuarantee symmetry = LinkSymmetryGuarantee::Unspecified;
  TopologyGeneration topology_generation;
  EndpointGeneration endpoint_generation;
  /// Extra links appended verbatim after the generated ones.
  std::vector<SyntheticLinkSpec> extra_links;
};

/// Builds a deterministic synthetic fabric description.
LSF_EXPORT std::vector<SyntheticLinkSpec> build_synthetic_fabric(const SyntheticFabricSpec& spec);

/// Builds a deterministic synthetic inventory of independent links.
LSF_EXPORT std::vector<SyntheticLinkSpec> build_synthetic_inventory(std::size_t link_count,
                                                                   const FabricId& fabric,
                                                                   const SiteId& site,
                                                                   LinkClass link_class,
                                                                   TopologyGeneration topology,
                                                                   EndpointGeneration endpoints);

/// Converts a synthetic link description into a topology binding.
LSF_EXPORT TopologyBinding to_binding(const SyntheticLinkSpec& spec);

/// Drives a synthetic fabric against an engine using the public API only.
///
/// Every observation produced here is labelled EvidenceClass::Synthetic, and
/// the default evidence kind is EvidenceKind::SyntheticTest, so synthetic truth
/// can never be confused with real host evidence.
class LSF_EXPORT SyntheticFabric {
 public:
  SyntheticFabric(LinkStateEngine& engine, PublisherId publisher, WorkerBootId worker_boot,
                  SourceId source);

  /// Binds every link and registers this publisher for all of them.
  Outcome bind(const std::vector<SyntheticLinkSpec>& links);

  /// Publishes one claim for one link, advancing this fabric's sequence.
  LinkMutationResult publish(const LinkId& link, EvidenceClaim claim,
                             DegradationCause cause = DegradationCause::None,
                             EvidenceKind kind = EvidenceKind::SyntheticTest,
                             std::string detail = {});

  /// Publishes one claim scoped to an endpoint and direction.
  LinkMutationResult publish_endpoint(const LinkId& link, const EndpointId& endpoint,
                                      EvidenceDirection direction, EvidenceClaim claim,
                                      EvidenceKind kind = EvidenceKind::SyntheticTest,
                                      DegradationCause cause = DegradationCause::None,
                                      std::string detail = {});

  /// Publishes a claim from a distinct synthetic source identity, used to model
  /// multi source corroboration and conflict.
  LinkMutationResult publish_from(const SourceId& source, const LinkId& link, EvidenceClaim claim,
                                  EvidenceKind kind = EvidenceKind::SyntheticTest,
                                  DegradationCause cause = DegradationCause::None,
                                  std::string detail = {});

  /// Withdraws the most recent observation of one evidence key.
  LinkMutationResult withdraw(const LinkId& link, const SourceId& source,
                              EvidenceKind kind = EvidenceKind::SyntheticTest,
                              EvidenceDirection direction = EvidenceDirection::Bidirectional,
                              EndpointId endpoint = EndpointId{}, std::string reason = {});

  /// Current observation sequence for an evidence key, for tests that need to
  /// replay or race exact observations.
  std::uint64_t sequence_of(const LinkId& link, const SourceId& source, EvidenceKind kind,
                            EvidenceDirection direction = EvidenceDirection::Bidirectional,
                            EndpointId endpoint = EndpointId{}) const;

  /// The publisher identity used by this fabric.
  const PublisherId& publisher() const noexcept { return publisher_; }
  const WorkerBootId& worker_boot() const noexcept { return worker_boot_; }
  const SourceId& source() const noexcept { return source_; }
  CoordinatorEpoch epoch() const noexcept { return epoch_; }

  /// Rebinds the fabric to a new coordinator epoch after an epoch advance.
  Outcome rebind(CoordinatorEpoch epoch, const std::vector<LinkId>& links);

 private:
  std::string key_of(const SourceId& source, const LinkId& link, EvidenceKind kind,
                     EvidenceDirection direction, const EndpointId& endpoint) const;

  LinkMutationResult publish_endpoint_impl(const SourceId& source, const LinkId& link,
                                           const EndpointId& endpoint, EvidenceDirection direction,
                                           EvidenceKind kind, EvidenceClaim claim,
                                           DegradationCause cause, std::string detail);

  LinkStateEngine& engine_;
  PublisherId publisher_;
  WorkerBootId worker_boot_;
  SourceId source_;
  CoordinatorEpoch epoch_;
  std::unordered_map<std::string, std::uint64_t> sequences_;
  std::uint64_t observation_counter_ = 0;
};

}  // namespace linkstate
