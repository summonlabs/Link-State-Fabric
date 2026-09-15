#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "linkstate/digest.hpp"
#include "linkstate/evidence.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/record.hpp"
#include "linkstate/state.hpp"
#include "lsf_test.hpp"

using namespace linkstate;

namespace {

bool transition_expected(LinkOperationalState from, LinkOperationalState to,
                         TransitionTrigger trigger) {
  if (from == to) {
    return false;
  }
  if (from == LinkOperationalState::Retired) {
    return false;
  }
  if (to == LinkOperationalState::Retired) {
    return trigger == TransitionTrigger::Retirement ||
           trigger == TransitionTrigger::TopologyInvalidation;
  }
  if (to == LinkOperationalState::RevalidationRequired) {
    switch (trigger) {
      case TransitionTrigger::EvidenceResolution:
      case TransitionTrigger::EvidenceWithdrawal:
      case TransitionTrigger::PublisherFenced:
      case TransitionTrigger::SourceLost:
      case TransitionTrigger::TopologyInvalidation:
      case TransitionTrigger::CoordinatorEpochAdvance:
      case TransitionTrigger::Recovery:
      case TransitionTrigger::RevalidationOutcome:
        return true;
      default:
        return false;
    }
  }
  if (to == LinkOperationalState::Unknown) {
    switch (trigger) {
      case TransitionTrigger::EvidenceResolution:
      case TransitionTrigger::EvidenceWithdrawal:
      case TransitionTrigger::RevalidationOutcome:
      case TransitionTrigger::Recovery:
        return true;
      default:
        return false;
    }
  }
  switch (trigger) {
    case TransitionTrigger::EvidenceResolution:
    case TransitionTrigger::RevalidationOutcome:
    case TransitionTrigger::ExplicitAdministrative:
      return true;
    default:
      return false;
  }
}

}  // namespace

LSF_TEST(identifier_validation_accepts_documented_forms) {
  LSF_CHECK(validate_identifier("link.a1") == IdValidation::Ok);
  LSF_CHECK(validate_identifier("A") == IdValidation::Ok);
  LSF_CHECK(validate_identifier("fabric:site/link-1.2_3") == IdValidation::Ok);
  LSF_CHECK(validate_identifier(std::string(limits::max_identifier_length, 'a')) ==
            IdValidation::Ok);
}

LSF_TEST(identifier_validation_rejects_malformed_forms) {
  LSF_CHECK(validate_identifier("") == IdValidation::Empty);
  LSF_CHECK(validate_identifier(std::string(limits::max_identifier_length + 1, 'a')) ==
            IdValidation::TooLong);
  LSF_CHECK(validate_identifier("link a") == IdValidation::InvalidCharacter);
  LSF_CHECK(validate_identifier("link\ta") == IdValidation::InvalidCharacter);
  LSF_CHECK(validate_identifier("link;a") == IdValidation::InvalidCharacter);
  LSF_CHECK(validate_identifier("\\server") == IdValidation::InvalidCharacter);
  LSF_CHECK(validate_identifier("../etc/passwd") == IdValidation::DotDot);
  LSF_CHECK(validate_identifier("/etc/passwd") == IdValidation::LeadingSeparator);
  LSF_CHECK(validate_identifier("link..a") == IdValidation::DotDot);
  LSF_CHECK(validate_identifier("link.a.") == IdValidation::TrailingSeparator);
  LSF_CHECK(validate_identifier("-link") == IdValidation::LeadingSeparator);
  LSF_CHECK(validate_identifier(std::string("link.") + "\xc3\xa9") ==
            IdValidation::InvalidCharacter);
}

LSF_TEST(strong_identifiers_do_not_cross_domains_implicitly) {
  const LinkId link = LinkId::from_validated("link.alpha");
  const EndpointId endpoint = EndpointId::from_validated("link.alpha");
  static_assert(!std::is_convertible_v<LinkId, EndpointId>);
  static_assert(!std::is_constructible_v<EndpointId, LinkId>);
  LSF_CHECK(link.valid());
  LSF_CHECK(endpoint.valid());
  LSF_CHECK(link.value() == endpoint.value());
  LSF_CHECK(link.to_string() == std::string("link.alpha"));

  const LinkId invalid = LinkId::from_validated("bad id");
  LSF_CHECK(!invalid.valid());
  LSF_CHECK(!invalid);
  LSF_CHECK(!LinkId::parse("bad id").has_value());
  IdValidation reason = IdValidation::Ok;
  LSF_CHECK(!LinkId::parse("", reason).has_value());
  LSF_CHECK(reason == IdValidation::Empty);
}

LSF_TEST(strong_identifiers_order_and_hash_deterministically) {
  const LinkId a = LinkId::from_validated("link.a");
  const LinkId b = LinkId::from_validated("link.b");
  LSF_CHECK(a < b);
  LSF_CHECK(a != b);
  LSF_CHECK(a == LinkId::from_validated("link.a"));
  LSF_CHECK(a.hash() == LinkId::from_validated("link.a").hash());
  LSF_CHECK(std::hash<LinkId>{}(a) == std::hash<LinkId>{}(LinkId::from_validated("link.a")));
  LSF_CHECK(LinkId{}.hash() == 0);
  LSF_CHECK(!(LinkId{} == a));
}

LSF_TEST(generations_are_monotonic_and_saturating) {
  LinkStateGeneration generation = LinkStateGeneration::from_value(0);
  LSF_CHECK(generation.is_zero());
  generation = *generation.next();
  LSF_CHECK_EQ(generation.value(), std::uint64_t{1});
  LSF_CHECK(generation.to_string() == "1");
  const LinkStateGeneration saturated =
      LinkStateGeneration::from_value((std::numeric_limits<std::uint64_t>::max)());
  LSF_CHECK(!saturated.next().has_value());
  LSF_CHECK(LinkStateGeneration::from_value(4) < LinkStateGeneration::from_value(5));
  LSF_CHECK(LinkStateGeneration::from_value(5) > LinkStateGeneration::from_value(4));
}

LSF_TEST(sha256_matches_published_vectors) {
  LSF_CHECK(sha256(std::string("")).to_hex() ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  LSF_CHECK(sha256(std::string("abc")).to_hex() ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string long_input(1000, 'a');
  LSF_CHECK(sha256(long_input).to_hex() ==
            "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

LSF_TEST(digest_round_trips_and_rejects_malformed_hex) {
  const Digest digest = sha256(std::string("link-state-fabric"));
  const std::optional<Digest> parsed = Digest::from_hex(digest.to_hex());
  LSF_REQUIRE(parsed.has_value());
  LSF_CHECK(parsed.value() == digest);
  LSF_CHECK(!Digest::from_hex("").has_value());
  LSF_CHECK(!Digest::from_hex("zz").has_value());
  LSF_CHECK(!Digest::from_hex(std::string(64, 'z')).has_value());
  LSF_CHECK(Digest{}.is_zero());
  LSF_CHECK(!digest.is_zero());
}

LSF_TEST(hasher_streaming_matches_one_shot) {
  Hasher hasher;
  hasher.update(std::string("link"));
  hasher.update_u8(7);
  hasher.update_u16(9);
  hasher.update_u32(11);
  hasher.update_u64(13);
  hasher.update_text("state");
  const Digest streamed = hasher.finalize();

  Hasher reference;
  reference.update(std::string("link"));
  reference.update_u8(7);
  reference.update_u16(9);
  reference.update_u32(11);
  reference.update_u64(13);
  reference.update_text("state");
  LSF_CHECK(streamed == reference.finalize());

  Hasher different;
  different.update(std::string("link"));
  different.update_u8(7);
  different.update_u16(9);
  different.update_u32(11);
  different.update_u64(14);
  different.update_text("state");
  LSF_CHECK(streamed != different.finalize());
}

LSF_TEST(state_model_round_trips_through_canonical_names) {
  for (std::size_t index = 0; index < link_operational_state_count; ++index) {
    const auto state = static_cast<LinkOperationalState>(index);
    const char* name = to_string(state);
    LSF_CHECK(name != nullptr);
    LSF_CHECK(std::string(describe(state)).size() > 0);
    const std::optional<LinkOperationalState> parsed = parse_state(name);
    LSF_REQUIRE(parsed.has_value());
    LSF_CHECK(parsed.value() == state);
  }
  LSF_CHECK(!parse_state("up").has_value());
  LSF_CHECK(!parse_state("HEALTHY").has_value());
  LSF_CHECK(std::string(to_string(LinkOperationalState::RevalidationRequired)) ==
            "REVALIDATION_REQUIRED");
  LSF_CHECK(is_live_state(LinkOperationalState::Unknown));
  LSF_CHECK(!is_live_state(LinkOperationalState::Retired));
  LSF_CHECK(is_terminal_state(LinkOperationalState::Retired));
  LSF_CHECK(is_operational_state(LinkOperationalState::Up));
  LSF_CHECK(is_operational_state(LinkOperationalState::Degraded));
  LSF_CHECK(is_operational_state(LinkOperationalState::Draining));
  LSF_CHECK(!is_operational_state(LinkOperationalState::Down));
  LSF_CHECK(!is_operational_state(LinkOperationalState::Unknown));
}

LSF_TEST(transition_matrix_matches_the_documented_contract) {
  for (std::size_t from_index = 0; from_index < link_operational_state_count; ++from_index) {
    for (std::size_t to_index = 0; to_index < link_operational_state_count; ++to_index) {
      const auto from = static_cast<LinkOperationalState>(from_index);
      const auto to = static_cast<LinkOperationalState>(to_index);
      for (unsigned trigger_index = 0; trigger_index < 10; ++trigger_index) {
        const auto trigger = static_cast<TransitionTrigger>(trigger_index);
        const TransitionLegality legality = evaluate_transition(from, to, trigger);
        if (from == to) {
          LSF_CHECK(legality == TransitionLegality::SameState);
          continue;
        }
        if (from == LinkOperationalState::Retired) {
          LSF_CHECK(legality == TransitionLegality::FromRetired);
          continue;
        }
        const bool expected = transition_expected(from, to, trigger);
        if (expected) {
          LSF_CHECK(legality == TransitionLegality::Legal);
        } else if (to == LinkOperationalState::Retired) {
          LSF_CHECK(legality == TransitionLegality::RetirementRequired);
        } else {
          LSF_CHECK(legality == TransitionLegality::TriggerNotPermitted);
        }
      }
    }
  }
}

LSF_TEST(retired_is_a_sink_and_unknown_is_never_silently_reached) {
  for (unsigned trigger_index = 0; trigger_index < 10; ++trigger_index) {
    const auto trigger = static_cast<TransitionTrigger>(trigger_index);
    for (std::size_t to_index = 0; to_index < link_operational_state_count; ++to_index) {
      const auto to = static_cast<LinkOperationalState>(to_index);
      if (to == LinkOperationalState::Retired) {
        LSF_CHECK(evaluate_transition(LinkOperationalState::Retired, to, trigger) ==
                  TransitionLegality::SameState);
        continue;
      }
      LSF_CHECK(evaluate_transition(LinkOperationalState::Retired, to, trigger) ==
                TransitionLegality::FromRetired);
    }
    LSF_CHECK(evaluate_transition(LinkOperationalState::Up, LinkOperationalState::Unknown, trigger) ==
              (trigger == TransitionTrigger::EvidenceResolution ||
                       trigger == TransitionTrigger::EvidenceWithdrawal ||
                       trigger == TransitionTrigger::RevalidationOutcome ||
                       trigger == TransitionTrigger::Recovery
                   ? TransitionLegality::Legal
                   : TransitionLegality::TriggerNotPermitted));
  }
  LSF_CHECK(evaluate_transition(LinkOperationalState::Up, LinkOperationalState::Retired,
                                TransitionTrigger::EvidenceResolution) ==
            TransitionLegality::RetirementRequired);
  LSF_CHECK(evaluate_transition(LinkOperationalState::Up, LinkOperationalState::Retired,
                                TransitionTrigger::Retirement) == TransitionLegality::Legal);
}

LSF_TEST(claims_and_states_convert_without_overloading) {
  LSF_CHECK(state_from_claim(EvidenceClaim::Up).value() == LinkOperationalState::Up);
  LSF_CHECK(state_from_claim(EvidenceClaim::AdminDisabled).value() ==
            LinkOperationalState::AdminDisabled);
  LSF_CHECK(claim_from_state(LinkOperationalState::Retired) == std::nullopt);
  LSF_CHECK(claim_from_state(LinkOperationalState::RevalidationRequired) == std::nullopt);
  for (std::size_t index = 0; index < evidence_claim_count; ++index) {
    const auto claim = static_cast<EvidenceClaim>(index);
    LSF_CHECK(std::string(to_string(claim)).size() > 0);
    const std::optional<LinkOperationalState> state = state_from_claim(claim);
    LSF_REQUIRE(state.has_value());
    const std::optional<EvidenceClaim> back = claim_from_state(state.value());
    LSF_REQUIRE(back.has_value());
    LSF_CHECK(back.value() == claim);
  }
}

LSF_TEST(evidence_precedence_is_a_stated_policy) {
  LSF_CHECK_EQ(evidence_precedence(EvidenceKind::AdministrativeState), std::uint8_t{1});
  LSF_CHECK_EQ(evidence_precedence(EvidenceKind::HardwareErrorIndication), std::uint8_t{2});
  LSF_CHECK_EQ(evidence_precedence(EvidenceKind::CarrierState), std::uint8_t{3});
  LSF_CHECK_EQ(evidence_precedence(EvidenceKind::RemoteEndpointState), std::uint8_t{4});
  LSF_CHECK_EQ(evidence_precedence(EvidenceKind::SyntheticTest), std::uint8_t{5});
}

LSF_TEST(evidence_set_digest_is_insertion_order_independent) {
  std::vector<EvidenceRecord> first;
  std::vector<EvidenceRecord> second;
  for (int index = 0; index < 8; ++index) {
    EvidenceRecord record;
    record.key.link = LinkId::from_validated("link.digest" + std::to_string(index));
    record.key.source = SourceId::from_validated("source.digest");
    record.key.kind = EvidenceKind::CarrierState;
    record.observation = ObservationId::from_validated("obs." + std::to_string(index));
    record.publisher = PublisherId::from_validated("publisher.digest");
    record.worker_boot = WorkerBootId::from_validated("boot.digest");
    record.sequence = static_cast<std::uint64_t>(index);
    record.claim = EvidenceClaim::Up;
    first.push_back(record);
    second.insert(second.begin(), record);
  }
  LSF_CHECK(digest_evidence_set(first) == digest_evidence_set(second));
  second[0].claim = EvidenceClaim::Down;
  LSF_CHECK(digest_evidence_set(first) != digest_evidence_set(second));
}

LSF_TEST(record_digest_covers_every_authoritative_field) {
  LinkStateRecord record;
  record.binding = [] {
    TopologyBinding binding;
    binding.link = LinkId::from_validated("link.record");
    binding.topology_generation = TopologyGeneration::from_value(3);
    binding.endpoints.local = EndpointId::from_validated("ep.record.a");
    binding.endpoints.remote = EndpointId::from_validated("ep.record.b");
    binding.link_class = LinkClass::EthernetLike;
    binding.symmetry = LinkSymmetryGuarantee::Symmetric;
    return binding;
  }();
  record.state = LinkOperationalState::Up;
  record.state_generation = LinkStateGeneration::from_value(4);
  const Digest baseline = digest_record(record);
  LSF_CHECK(digest_record(record) == baseline);

  LinkStateRecord changed = record;
  changed.state = LinkOperationalState::Down;
  LSF_CHECK(digest_record(changed) != baseline);

  changed = record;
  changed.binding.topology_generation = TopologyGeneration::from_value(4);
  LSF_CHECK(digest_record(changed) != baseline);

  changed = record;
  AdministrativeIntent intent;
  intent.publication = PublicationId::from_validated("pub.admin");
  intent.publisher = PublisherId::from_validated("publisher.admin");
  intent.state = LinkOperationalState::AdminDisabled;
  changed.admin_intent = intent;
  LSF_CHECK(digest_record(changed) != baseline);
}

LSF_TEST(link_class_symmetry_defaults_are_explicit) {
  LSF_CHECK(default_symmetry_for(LinkClass::EthernetLike) == LinkSymmetryGuarantee::Symmetric);
  LSF_CHECK(default_symmetry_for(LinkClass::Optical) == LinkSymmetryGuarantee::Directional);
  LSF_CHECK(default_symmetry_for(LinkClass::Logical) == LinkSymmetryGuarantee::Directional);
  LSF_CHECK(default_symmetry_for(LinkClass::TunnelBacked) == LinkSymmetryGuarantee::Directional);
  LSF_CHECK(default_symmetry_for(LinkClass::Unknown) == LinkSymmetryGuarantee::Unspecified);
  LSF_CHECK(!link_class_is_physical(LinkClass::Logical));
  LSF_CHECK(!link_class_is_physical(LinkClass::TunnelBacked));
  LSF_CHECK(link_class_is_physical(LinkClass::EthernetLike));
  for (std::size_t index = 0; index < 10; ++index) {
    const auto link_class = static_cast<LinkClass>(index);
    LSF_CHECK(parse_link_class(to_string(link_class)).value() == link_class);
  }
}
