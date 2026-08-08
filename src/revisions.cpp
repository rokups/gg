// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <random>

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

std::string Repository::new_change_id() const {
  constexpr std::string_view alphabet = "zyxwvutsrqponmlk";
  std::random_device random;
  std::string result;
  result.reserve(32);
  do {
    result.clear();
    for (int index = 0; index < 16; ++index) {
      const unsigned int byte = random() & 0xffU;
      result.push_back(alphabet[byte >> 4U]);
      result.push_back(alphabet[byte & 0xfU]);
    }
  } while (changes().contains(result));  // GG_COV_EXCL_BRANCH
  return result;
}

std::string Repository::short_change_id(std::string_view id) const {
  std::size_t length = std::min<std::size_t>(8, id.size());
  for (const auto& [other, oid] : changes()) {
    (void)oid;
    if (other == id) {
      continue;
    }
    std::size_t common = 0;
    while (common < id.size() && common < other.size() &&
           id[common] == other[common]) {
      ++common;
    }
    length = std::max(length, std::min(id.size(), common + 1));
  }
  return std::string(id.substr(0, length));
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

std::vector<git_oid> Repository::children(const git_oid& oid) const {
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  for (const auto& [name, target] : rewrite_refs()) {
    (void)name;
    check(git_revwalk_push(walk.get(), &target), "walk revisions");
  }
  std::set<git_oid, OidLess> result;
  git_oid candidate{};
  while (git_revwalk_next(&candidate, walk.get()) == 0) {
    for (const git_oid& parent : parents(candidate)) {
      if (parent == oid) {
        result.insert(candidate);
      }
    }
  }
  return {result.begin(), result.end()};
}

}  // namespace gg::detail
