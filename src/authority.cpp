#include "linkstate/authority.hpp"

#include <algorithm>

namespace linkstate {

bool AuthorityScope::covers(const LinkId& link) const noexcept {
  if (links.empty()) {
    return false;
  }
  const auto found = std::lower_bound(links.begin(), links.end(), link);
  return found != links.end() && *found == link;
}

bool AuthorityScope::normalize() noexcept {
  std::sort(links.begin(), links.end());
  links.erase(std::unique(links.begin(), links.end()), links.end());
  if (links.empty() || links.size() > limits::max_authority_scope_links) {
    return false;
  }
  return true;
}

const char* to_string(AuthorityStatus status) noexcept {
  switch (status) {
    case AuthorityStatus::Active:
      return "ACTIVE";
    case AuthorityStatus::Fenced:
      return "FENCED";
    case AuthorityStatus::Superseded:
      return "SUPERSEDED";
    case AuthorityStatus::EpochSuperseded:
      return "EPOCH_SUPERSEDED";
  }
  return "FENCED";
}

}  // namespace linkstate
