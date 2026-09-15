#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "linkstate/authority.hpp"
#include "linkstate/config.hpp"
#include "linkstate/digest.hpp"
#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/outcome.hpp"
#include "linkstate/record.hpp"

namespace linkstate {

/// Journal magic: the four bytes "LSF1" read as a big endian word.
inline constexpr std::uint32_t journal_magic = 0x4C534631u;

/// On disk format version of the journal.
inline constexpr std::uint16_t journal_format_version = 1;

/// Size of the fixed journal header in bytes.
inline constexpr std::size_t journal_header_bytes = 64;

/// Size of the fixed journal trailer in bytes: the payload digest followed by
/// the whole file digest.
inline constexpr std::size_t journal_trailer_bytes = 2 * Digest::byte_count;

/// One durable link record together with the evidence metadata that is kept for
/// conservative recovery.
struct LSF_EXPORT PersistedLink {
  LinkStateRecord record;
  std::vector<EvidenceRecord> evidence;
};

/// The complete durable payload of a journal.
struct LSF_EXPORT JournalPayload {
  LayoutGeneration layout;
  CoordinatorEpoch epoch;
  GlobalGeneration global_generation;
  LinkStateGeneration global_state_generation;
  TopologyGeneration topology_generation;
  std::uint64_t snapshot_counter = 0;
  std::uint64_t transition_counter = 0;
  std::vector<PersistedLink> links;
  std::vector<PublisherFence> fences;
};

/// Validated description of a journal, produced without applying it.
struct LSF_EXPORT JournalSummary {
  std::uint16_t format_version = 0;
  std::uint64_t flags = 0;
  LayoutGeneration layout;
  CoordinatorEpoch epoch;
  GlobalGeneration global_generation;
  LinkStateGeneration global_state_generation;
  TopologyGeneration topology_generation;
  std::size_t record_count = 0;
  std::size_t evidence_count = 0;
  std::size_t fence_count = 0;
  Digest payload_digest;
  Digest file_digest;
  std::uint64_t file_size = 0;

  std::string render() const;
};

/// Encodes a payload into the canonical journal byte stream.
///
/// The encoding is deterministic: identical payloads always produce identical
/// bytes. Header fields, a payload digest and a whole file digest make truncation
/// and modification detectable.
LSF_EXPORT std::vector<std::byte> encode_journal(const JournalPayload& payload,
                                                 const PersistenceConfig& config,
                                                 JournalSummary& summary);

/// Decodes and validates a journal byte stream.
///
/// Every failure mode is reported through a specific outcome code: bad magic and
/// wrong checksums are PERSISTENCE_CORRUPT, an unsupported version is
/// UNSUPPORTED_STATE, an oversized count is RESOURCE_LIMIT.
LSF_EXPORT Outcome decode_journal(std::span<const std::byte> bytes,
                                  const PersistenceConfig& config, JournalPayload& payload,
                                  JournalSummary& summary);

/// Reads a bounded journal file from disk.
LSF_EXPORT Outcome read_journal_file(const PersistenceConfig& config,
                                     std::vector<std::byte>& bytes, JournalSummary& summary);

/// Writes a journal atomically: the bytes are written to a temporary sibling
/// file, flushed and then moved over the destination.
LSF_EXPORT Outcome write_journal_file(const PersistenceConfig& config,
                                      std::span<const std::byte> bytes);

}  // namespace linkstate
