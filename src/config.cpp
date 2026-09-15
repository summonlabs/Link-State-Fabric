#include "linkstate/config.hpp"

#include <filesystem>

#include "text.hpp"

namespace linkstate {
namespace {

bool is_plain_path(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  for (const char ch : path) {
    const auto value = static_cast<unsigned char>(ch);
    if (value < 0x20u || value == 0x7fu) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string EngineConfig::validate() const {
  if (max_links == 0 || max_links > limits::max_links) {
    return "max_links must be between 1 and limits::max_links";
  }
  if (max_evidence_per_link == 0 || max_evidence_per_link > limits::max_evidence_per_link) {
    return "max_evidence_per_link must be between 1 and limits::max_evidence_per_link";
  }
  if (max_publishers == 0 || max_publishers > limits::max_publishers) {
    return "max_publishers must be between 1 and limits::max_publishers";
  }
  if (max_retained_snapshots > limits::max_retained_snapshots) {
    return "max_retained_snapshots exceeds limits::max_retained_snapshots";
  }
  if (max_history_per_link > limits::max_history_per_link) {
    return "max_history_per_link exceeds limits::max_history_per_link";
  }
  if (max_batch_size == 0 || max_batch_size > limits::max_batch_size) {
    return "max_batch_size must be between 1 and limits::max_batch_size";
  }
  if (fence_retention_per_publisher == 0 || fence_retention_per_publisher > 4096) {
    return "fence_retention_per_publisher must be between 1 and 4096";
  }
  return std::string();
}

std::string PersistenceConfig::validate() const {
  if (!is_plain_path(path)) {
    return "persistence path is empty or contains control characters";
  }
  const std::filesystem::path candidate(path);
  if (candidate.is_relative()) {
    const std::string generic = candidate.generic_string();
    if (generic == "." || generic == "..") {
      return "persistence path must name a file";
    }
  } else if (!candidate.has_filename()) {
    return "persistence path must name a file";
  }
  for (const auto& part : candidate) {
    if (part == "..") {
      return "persistence path must not contain relative traversal components";
    }
  }
  if (max_records == 0 || max_records > limits::max_persisted_records) {
    return "max_records must be between 1 and limits::max_persisted_records";
  }
  return std::string();
}

}  // namespace linkstate
