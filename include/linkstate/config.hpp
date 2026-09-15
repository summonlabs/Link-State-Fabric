#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "linkstate/export.hpp"
#include "linkstate/generation.hpp"
#include "linkstate/ids.hpp"
#include "linkstate/limits.hpp"

namespace linkstate {

/// Engine configuration.
///
/// Every externally influenced resource is bounded here. The defaults are the
/// values validated by the test suite; a caller may lower them but never raise
/// them beyond the compile time limits in linkstate/limits.hpp.
struct LSF_EXPORT EngineConfig {
  /// Maximum number of links the engine will track.
  std::size_t max_links = limits::max_links;
  /// Maximum number of distinct evidence slots retained per link.
  std::size_t max_evidence_per_link = limits::max_evidence_per_link;
  /// Maximum number of publisher incarnations retained (live plus fenced).
  std::size_t max_publishers = limits::max_publishers;
  /// Maximum number of snapshots retained in the snapshot registry.
  std::size_t max_retained_snapshots = limits::max_retained_snapshots;
  /// Maximum number of state history entries retained per link.
  std::size_t max_history_per_link = limits::max_history_per_link;
  /// Maximum number of records accepted in one batch request.
  std::size_t max_batch_size = limits::max_batch_size;
  /// Number of permanently fenced worker incarnations retained per publisher.
  ///
  /// Fencing is permanent: a fenced worker boot can never become active again,
  /// even if a process presents it after a restart. The retention window bounds
  /// that memory; an incarnation fenced further back than the window is no
  /// longer tracked.
  std::size_t fence_retention_per_publisher = 64;

  /// Coordinator epoch the engine starts in.
  CoordinatorEpoch initial_epoch{};
  /// Topology generation the engine starts with.
  TopologyGeneration initial_topology_generation{};

  /// Retain per-link state history. Disabling it bounds memory further; the
  /// authoritative state model is unaffected because history is never
  /// authoritative.
  bool retain_history = true;

  /// Validates the configuration, clamping nothing silently.
  /// Returns an empty string when valid, otherwise the reason.
  std::string validate() const;
};

/// Persistence configuration.
struct LSF_EXPORT PersistenceConfig {
  /// Journal file path. Must be a plain file name or an absolute path without
  /// relative traversal components.
  std::string path = "linkstate.lsfjournal";
  /// Reject journals holding more than this many records.
  std::size_t max_records = limits::max_persisted_records;
  /// Persist state history alongside the durable records.
  bool include_history = false;
  /// Create the parent directory when missing.
  bool create_parent_directory = false;

  std::string validate() const;
};

}  // namespace linkstate
