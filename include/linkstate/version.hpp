#pragma once

#include <string>

#include "linkstate/export.hpp"

#ifndef LSF_VERSION_MAJOR
#define LSF_VERSION_MAJOR 1
#endif
#ifndef LSF_VERSION_MINOR
#define LSF_VERSION_MINOR 0
#endif
#ifndef LSF_VERSION_PATCH
#define LSF_VERSION_PATCH 0
#endif

namespace linkstate {

/// Semantic version of the Link State Fabric runtime, as compiled.
struct Version {
  int major = LSF_VERSION_MAJOR;
  int minor = LSF_VERSION_MINOR;
  int patch = LSF_VERSION_PATCH;

  friend bool operator==(const Version&, const Version&) = default;
};

/// Major.minor.patch rendering, e.g. "1.0.0".
LSF_EXPORT std::string to_string(const Version& version);

/// Version of the compiled runtime.
LSF_EXPORT Version runtime_version() noexcept;

/// Compact product identifier used by diagnostics and tool banners.
LSF_EXPORT const char* product_name() noexcept;

}  // namespace linkstate
