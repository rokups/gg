// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>

#include <utility>

namespace gg::detail {

void command_rebase(Repository& repo,
                    const RebaseCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  const git_oid old = repo.resolve(options.source);
  const git_oid parent = repo.resolve(options.destination);
  const int destination_is_descendant =
      git_graph_descendant_of(repo.raw(), &parent, &old);
  check(destination_is_descendant, "check rebase destination");
  if (parent == old || destination_is_descendant != 0) {
    throw UserError("rebase destination cannot be the source or its descendant");
  }
  const auto old_parents = repo.parents(old);
  if (old_parents.size() != 1) {
    throw UserError("rebase source must have exactly one parent");
  }
  CommitPtr source_commit = repo.commit(old);
  const git_oid tree = repo.replay(old_parents.front(), parent,
                                   *git_commit_tree_id(source_commit.get()));
  const git_oid rewritten = repo.rewrite_commit(old, {parent}, tree);
  RewritePlan plan = repo.descendants({{old, rewritten}});
  const auto workspace = repo.ref_target(kWorkspaceRef);
  const git_oid new_workspace = plan.commits.contains(*workspace)
                                    ? plan.commits.at(*workspace)
                                    : *workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {}, "gg rebase");
  output << "Rebased " << oid_string(old, 8) << " as "
         << oid_string(rewritten, 8) << '\n';
}

void command_split(Repository& repo,
                   const SplitCommand& options,
                   std::ostream& output) {
  repo.sync_workspace();
  const git_oid old =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  const auto old_parents = repo.parents(old);
  if (old_parents.size() != 1) {
    throw UserError("split revision must have exactly one parent");
  }
  std::vector<std::string> paths;
  for (const std::string& path : options.paths) {
    if (path.empty() || path.front() == '/') {
      throw UserError("split paths must be repository-relative");
    }
    paths.emplace_back(path);
  }
  CommitPtr old_commit = repo.commit(old);
  CommitPtr parent = repo.commit(old_parents.front());
  const git_oid old_tree = *git_commit_tree_id(old_commit.get());
  const git_oid base_tree = *git_commit_tree_id(parent.get());
  const git_oid selected_tree = repo.selected_tree(base_tree, old_tree, paths);
  if (selected_tree == base_tree || selected_tree == old_tree) {
    throw UserError("split must leave changes in both revisions");
  }
  const std::string selected_message = options.message.empty()
                                           ? first_line(git_commit_message(old_commit.get()))
                                           : options.message;
  const git_oid selected = repo.rewrite_commit(old, old_parents, selected_tree,
                                                selected_message);
  const git_oid remainder = repo.create_commit(old_tree, {selected}, "");
  const std::string remainder_id = repo.new_change_id();
  RewritePlan plan = repo.descendants({{old, remainder}}, {old});
  for (const auto& [name, target] : repo.rewrite_refs()) {
    if (target == old && starts_with(name, kChangePrefix)) {
      plan.updates[name] = selected;
    }
  }
  plan.updates[std::string(kChangePrefix) + remainder_id] = remainder;
  const auto workspace = repo.ref_target(kWorkspaceRef);
  const git_oid new_workspace = *workspace == old
                                    ? remainder
                                    : (plan.commits.contains(*workspace)
                                           ? plan.commits.at(*workspace)
                                           : *workspace);
  finish_workspace(repo, new_workspace, std::move(plan.updates), {}, "gg split");
  output << "Selected change: " << oid_string(selected, 8) << '\n'
         << "Remaining change: " << repo.short_change_id(remainder_id) << ' '
         << oid_string(remainder, 8) << '\n';
}

void command_squash(Repository& repo,
                    const SquashCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  std::string source = options.source;
  if (!options.revision.empty()) {
    source = options.revision;
  }
  if (source.empty()) {
    source = "@";
  }
  const git_oid source_oid = repo.resolve(source);
  const auto source_parents = repo.parents(source_oid);
  if (source_parents.size() != 1) {
    throw UserError("squash source must have exactly one parent");
  }
  const git_oid destination_oid =
      options.destination.empty() ? source_parents.front()
                                  : repo.resolve(options.destination);
  if (!(destination_oid == source_parents.front())) {
    throw UserError("MVP squash destination must be the source parent");
  }
  CommitPtr source_commit = repo.commit(source_oid);
  CommitPtr destination_commit = repo.commit(destination_oid);
  std::string combined_message = options.message;
  if (combined_message.empty()) {
    combined_message = first_line(git_commit_message(destination_commit.get()));
    if (combined_message.empty()) {
      combined_message = first_line(git_commit_message(source_commit.get()));
    }
  }
  const git_oid rewritten_destination = repo.rewrite_commit(
      destination_oid, repo.parents(destination_oid),
      *git_commit_tree_id(source_commit.get()), combined_message);
  RewritePlan plan = repo.descendants(
      {{destination_oid, rewritten_destination},
       {source_oid, rewritten_destination}},
      {source_oid});
  std::set<std::string> deletes;
  for (const auto& [name, target] : repo.rewrite_refs()) {
    if (target == source_oid && starts_with(name, kChangePrefix)) {
      plan.updates.erase(name);
      deletes.insert(name);
    }
  }
  const auto workspace = repo.ref_target(kWorkspaceRef);
  git_oid new_workspace = *workspace;
  if (*workspace == source_oid) {
    new_workspace = repo.create_commit(
        *git_commit_tree_id(source_commit.get()), {rewritten_destination}, "");
    const std::string id = repo.new_change_id();
    plan.updates[std::string(kChangePrefix) + id] = new_workspace;
  } else if (plan.commits.contains(*workspace)) {  // GG_COV_EXCL_BRANCH
    new_workspace = plan.commits.at(*workspace);
  }
  finish_workspace(repo, new_workspace, std::move(plan.updates), std::move(deletes),
                   "gg squash");
  output << "Squashed into " << oid_string(rewritten_destination, 8) << '\n';
}

void command_abandon(Repository& repo,
                     const AbandonCommand& options,
                     std::ostream& output) {
  repo.sync_workspace();
  const git_oid old =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  const auto old_parents = repo.parents(old);
  if (old_parents.size() != 1) {
    throw UserError("abandon revision must have exactly one parent");
  }
  const git_oid parent = old_parents.front();
  RewritePlan plan = repo.descendants({{old, parent}}, {old});
  std::set<std::string> deletes;
  for (const auto& [name, target] : repo.rewrite_refs()) {
    if (target != old) {
      continue;
    }
    if (starts_with(name, kChangePrefix) || starts_with(name, "refs/heads/")) {
      plan.updates.erase(name);
      deletes.insert(name);
    }
  }
  const auto workspace = repo.ref_target(kWorkspaceRef);
  git_oid new_workspace = *workspace;
  if (*workspace == old) {
    CommitPtr parent_commit = repo.commit(parent);
    new_workspace = repo.create_commit(*git_commit_tree_id(parent_commit.get()),
                                       {parent}, "");
    const std::string id = repo.new_change_id();
    plan.updates[std::string(kChangePrefix) + id] = new_workspace;
  } else if (plan.commits.contains(*workspace)) {
    new_workspace = plan.commits.at(*workspace);
  }
  finish_workspace(repo, new_workspace, std::move(plan.updates), std::move(deletes),
                   "gg abandon");
  output << "Abandoned " << oid_string(old, 8) << '\n';
}

}  // namespace gg::detail
