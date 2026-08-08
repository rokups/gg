// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <utility>

namespace gg::detail {
git_oid Repository::merge_trees(const git_oid& ancestor_oid,
                    const git_oid& ours_oid,
                    const git_oid& theirs_oid) const {
  const auto active = pending();
  if (active.has_value()) {
    for (const Resolution& resolution : active->resolutions) {
      if (resolution.ancestor == ancestor_oid && resolution.ours == ours_oid &&  // GG_COV_EXCL_BRANCH
          resolution.theirs == theirs_oid) {  // GG_COV_EXCL_BRANCH
        return resolution.result;
      }
    }
  }
  TreePtr ancestor = tree(ancestor_oid);
  TreePtr ours = tree(ours_oid);
  TreePtr theirs = tree(theirs_oid);
  git_merge_options options = GIT_MERGE_OPTIONS_INIT;
  options.flags = GIT_MERGE_FIND_RENAMES;
  git_index* raw_index = nullptr;
  check(git_merge_trees(&raw_index, repo_.get(), ancestor.get(), ours.get(),
                        theirs.get(), &options),
        "merge trees");
  IndexPtr index(raw_index);
  if (git_index_has_conflicts(index.get()) != 0) {
    std::vector<std::string> paths;
    git_index_conflict_iterator* raw_iterator = nullptr;
    check(git_index_conflict_iterator_new(&raw_iterator, index.get()),
          "inspect rewrite conflicts");
    ConflictIteratorPtr iterator(raw_iterator);
    const git_index_entry* base = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    while (git_index_conflict_next(&base, &ours, &theirs, iterator.get()) == 0) {
      const git_index_entry* entry = ours != nullptr ? ours :
                                     theirs != nullptr ? theirs : base;  // GG_COV_EXCL_BRANCH
      paths.emplace_back(entry->path);
    }
    throw MergeConflict{ancestor_oid, ours_oid, theirs_oid, std::move(index),
                        std::move(paths)};
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, index.get(), repo_.get()),
        "write merged tree");
  return result;
}

git_oid Repository::replay(const git_oid& old_parent,
               const git_oid& new_parent,
               const git_oid& old_tree) const {
  CommitPtr old_parent_commit = commit(old_parent);
  CommitPtr new_parent_commit = commit(new_parent);
  return merge_trees(*git_commit_tree_id(old_parent_commit.get()),
                     *git_commit_tree_id(new_parent_commit.get()), old_tree);
}

git_oid Repository::rewrite_commit(const git_oid& old_oid,
                       const std::vector<git_oid>& new_parents,
                       std::optional<git_oid> tree_override,
                       std::optional<std::string_view> message_override,
                       const git_signature* author_override,
                       const git_signature* committer_override) const {
  CommitPtr old = commit(old_oid);
  git_oid new_tree = tree_override.value_or(*git_commit_tree_id(old.get()));
  if (!tree_override.has_value() && git_commit_parentcount(old.get()) > 0 &&
      !new_parents.empty()) {
    const git_oid& old_parent = *git_commit_parent_id(old.get(), 0);
    if (!(old_parent == new_parents.front())) {
      new_tree = replay(old_parent, new_parents.front(), new_tree);
    }
  }
  const char* old_message = git_commit_message(old.get());
  const std::string_view original =
      old_message == nullptr ? "" : old_message;  // GG_COV_EXCL_BRANCH
  const std::string_view message = message_override.value_or(original);
  return create_commit(
      new_tree, new_parents, message,
      author_override == nullptr ? git_commit_author(old.get()) : author_override,
      committer_override);
}

std::vector<git_oid> Repository::parents(const git_oid& oid) const {
  CommitPtr value = commit(oid);
  std::vector<git_oid> result;
  for (unsigned int index = 0; index < git_commit_parentcount(value.get()); ++index) {
    result.push_back(*git_commit_parent_id(value.get(), index));
  }
  return result;
}

RewritePlan Repository::descendants(
    std::map<git_oid, git_oid, OidLess> roots,
    const std::set<git_oid, OidLess>& skipped,
    bool preserve_content) const {
  RewritePlan plan;
  plan.commits = std::move(roots);
  const auto refs = rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {
      check(pushed, "walk revisions");
    }
  }

  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    if (plan.commits.contains(oid) || skipped.contains(oid)) {  // GG_COV_EXCL_BRANCH
      continue;
    }
    const auto old_parents = parents(oid);
    bool changed = false;
    std::vector<git_oid> new_parents;
    new_parents.reserve(old_parents.size());
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      changed = changed || replacement != plan.commits.end();  // GG_COV_EXCL_BRANCH
      new_parents.push_back(replacement == plan.commits.end()
                                ? parent
                                : replacement->second);
    }
    if (changed) {
      std::optional<git_oid> tree_override;
      if (preserve_content) {
        tree_override = *git_commit_tree_id(commit(oid).get());
      }
      plan.commits.emplace(
          oid, rewrite_commit(oid, new_parents, tree_override));
    }
  }

  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  return plan;
}

}  // namespace gg::detail
