// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

namespace gg::detail {

std::map<std::string, git_oid> Repository::changes() const {
  std::map<std::string, git_oid> result;
  for (const auto& [name, oid] : data_refs()) {
    if (starts_with(name, kChangePrefix)) {
      result.emplace(name.substr(kChangePrefix.size()), oid);
    }
  }
  return result;
}

std::string Repository::new_change_id(const git_oid& seed) const {
  constexpr std::string_view alphabet = "zyxwvutsrqponmlk";
  const std::string hex = oid_string(seed);
  std::string result;
  result.reserve(hex.size());
  for (char character : hex) {
    const int value = character <= '9' ? character - '0' : character - 'a' + 10;
    result.push_back(alphabet[static_cast<std::size_t>(value)]);
  }
  const auto existing = changes();
  while (existing.contains(result)) {
    result.push_back('z');
  }
  return result;
}

std::optional<std::string> Repository::change_id(const git_oid& oid) const {
  for (const auto& [id, target] : changes()) {
    if (target == oid) {
      return id;
    }
  }
  return std::nullopt;
}

git_oid Repository::resolve(std::string_view revision) const {
  if (revision == "@" || starts_with(revision, "@-")) {
    const auto workspace = ref_target(kWorkspaceRef);
    if (!workspace.has_value()) {
      throw UserError("no gg working-copy change; run `gg new` first");
    }
    git_oid current = *workspace;
    for (std::size_t index = 1; index < revision.size(); ++index) {
      if (revision[index] != '-') {
        throw UserError("invalid working-copy revision: " +
                        std::string(revision));
      }
      const auto current_parents = parents(current);
      if (current_parents.empty()) {
        throw UserError("revision has no parent: " + std::string(revision));
      }
      current = current_parents.front();
    }
    return current;
  }

  std::optional<git_oid> match;
  for (const auto& [id, oid] : changes()) {
    if (starts_with(id, revision)) {
      if (match.has_value() && !(*match == oid)) {
        throw UserError("ambiguous change ID: " + std::string(revision));
      }
      match = oid;
    }
  }
  if (match.has_value()) {
    return *match;
  }

  const std::string bookmark = "refs/heads/" + std::string(revision);
  const auto bookmark_target = ref_target(bookmark);
  if (bookmark_target.has_value()) {
    return *bookmark_target;
  }

  git_object* raw_object = nullptr;
  const int result = git_revparse_single(&raw_object, repo_.get(),
                                         std::string(revision).c_str());
  if (result < 0) {
    git_error_clear();
    throw UserError("revision not found: " + std::string(revision));
  }
  ObjectPtr object(raw_object);
  git_object* raw_commit = nullptr;
  check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
        "resolve revision");
  ObjectPtr commit_object(raw_commit);
  return *git_object_id(commit_object.get());
}

std::vector<std::string> Repository::bookmarks(const git_oid& oid) const {
  std::vector<std::string> result;
  for (const auto& [name, target] : data_refs()) {
    if (starts_with(name, "refs/heads/") && target == oid) {
      result.push_back(name.substr(std::string_view("refs/heads/").size()));
    }
  }
  return result;
}

}  // namespace gg::detail
