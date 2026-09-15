#include "linkstate/version.hpp"

namespace linkstate {

std::string to_string(const Version& version) {
  std::string out;
  out += std::to_string(version.major);
  out.push_back('.');
  out += std::to_string(version.minor);
  out.push_back('.');
  out += std::to_string(version.patch);
  return out;
}

Version runtime_version() noexcept { return Version{}; }

const char* product_name() noexcept { return "Link State Fabric"; }

}  // namespace linkstate
