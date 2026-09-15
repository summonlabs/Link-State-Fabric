#pragma once

#include <cstddef>
#include <cstdint>

namespace linkstate::limits {

/// Maximum length, in bytes, of any strong identifier (LinkId, PublisherId, ...).
inline constexpr std::size_t max_identifier_length = 128;

/// Maximum length, in bytes, of an evidence detail string.
inline constexpr std::size_t max_detail_length = 256;

/// Maximum length, in bytes, of a link display label.
inline constexpr std::size_t max_label_length = 128;

/// Maximum size of an opaque, publisher supplied evidence metadata blob.
inline constexpr std::size_t max_evidence_metadata_bytes = 2048;

/// Maximum number of records accepted in a single batch request.
inline constexpr std::size_t max_batch_size = 4096;

/// Maximum number of link identifiers a single publisher authority scope may cover.
inline constexpr std::size_t max_authority_scope_links = 65536;

/// Maximum number of links tracked by one engine instance.
inline constexpr std::size_t max_links = 1000000;

/// Maximum number of distinct evidence keys retained per link.
inline constexpr std::size_t max_evidence_per_link = 64;

/// Maximum number of registered publishers (live or fenced) per engine instance.
inline constexpr std::size_t max_publishers = 65536;

/// Maximum number of records carried by a single snapshot.
inline constexpr std::size_t max_snapshot_records = 1000000;

/// Maximum number of snapshots retained by the engine snapshot registry.
inline constexpr std::size_t max_retained_snapshots = 64;

/// Maximum number of state history entries retained per link.
inline constexpr std::size_t max_history_per_link = 32;

/// Maximum number of persisted records accepted by the persistence decoder.
inline constexpr std::size_t max_persisted_records = 1000000;

/// Maximum number of injected synthetic links in one backend construction.
inline constexpr std::size_t max_synthetic_links = 1000000;

/// Maximum framed protocol payload size, in bytes.
inline constexpr std::size_t max_frame_payload_bytes = 1u << 20;

/// Maximum number of concurrent sessions accepted by a coordinator.
inline constexpr std::size_t max_sessions = 1024;

/// Maximum number of messages that may be queued for one session before the
/// connection is failed instead of growing without bound.
inline constexpr std::size_t max_queued_messages_per_session = 64;

/// Maximum number of topology bindings accepted in one supersession request.
inline constexpr std::size_t max_topology_batch = 65536;

}  // namespace linkstate::limits
