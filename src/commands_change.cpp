// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>

#include <set>
#include <utility>

namespace gg::detail {

void command_new(Repository& repo,
                 const NewCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
  std::vector<git_oid> parents;
  std::vector<std::string> revisions = options.parents;
  if (revisions.empty()) {
    if (repo.ref_target(kWorkspaceRef).has_value()) {
      revisions.emplace_back("@");
    } else if (const auto head = repo.head_oid(); head.has_value()) {
      parents.push_back(*head);
    }
  }
  if (!revisions.empty()) {
    parents = commit_parents(repo, revisions);
  }
  const git_oid change =
      repo.create_commit(combined_tree(repo, parents), parents, options.message);
  const std::string id = repo.new_change_id();
  std::map<std::string, git_oid> updates{
      {std::string(kChangePrefix) + id, change}};
  finish_workspace(repo, change, std::move(updates), {}, "gg new");
  output << "Working copy now at: " << repo.short_change_id(id) << ' '
         << oid_string(change, 8) << ' '
         << (options.message.empty() ? "(no description set)" : options.message)
         << '\n';
}

void command_status(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  const auto workspace = repo.ref_target(kWorkspaceRef);
  if (!workspace.has_value()) {
    output << "No working-copy change. Run `gg new` to create one.\n";
    return;
  }
  CommitPtr change = repo.commit(*workspace);
  const auto id = repo.change_id(*workspace);
  output << "Working copy (@): "
         << (id.has_value() ? repo.short_change_id(*id) : "--------")
         << ' ' << oid_string(*workspace, 8) << ' ';
  const std::string description = first_line(git_commit_message(change.get()));
  output << (description.empty() ? "(no description set)" : description) << '\n';
  const auto parents = repo.parents(*workspace);
  git_oid base_tree_oid{};
  if (!parents.empty()) {
    CommitPtr parent = repo.commit(parents.front());
    output << "Parent commit (@-): " << oid_string(parents.front(), 8) << ' '
           << first_line(git_commit_message(parent.get())) << '\n';
    base_tree_oid = *git_commit_tree_id(parent.get());
  } else {
    output << "Root working-copy change.\n";
    base_tree_oid = repo.empty_tree();
  }

  git_diff* raw_diff = nullptr;
  TreePtr base_tree = repo.tree(base_tree_oid);
  TreePtr change_tree = repo.tree(*git_commit_tree_id(change.get()));
  check(git_diff_tree_to_tree(&raw_diff, repo.raw(), base_tree.get(),
                              change_tree.get(), nullptr),
        "compare working change");
  DiffPtr diff(raw_diff);
  git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
  check(git_diff_find_similar(diff.get(), &find_options), "find renamed files");
  if (git_diff_num_deltas(diff.get()) == 0) {
    output << "The working copy has no changes.\n";
  } else {
    output << "Working copy changes:\n";
    for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
      const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
      const char status = delta->status == GIT_DELTA_ADDED      ? 'A'
                          : delta->status == GIT_DELTA_DELETED  ? 'D'
                          : delta->status == GIT_DELTA_RENAMED  ? 'R'
                                                                 : 'M';
      output << status << ' ' << delta->new_file.path << '\n';
    }
  }
}

void command_log(Repository& repo,
                 const LogCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  if (!options.revision.empty()) {
    const git_oid selected = repo.resolve(options.revision);
    check(git_revwalk_push(walk.get(), &selected), "walk revisions");
  } else {
    const auto workspace = repo.ref_target(kWorkspaceRef);
    if (workspace.has_value()) {
      check(git_revwalk_push(walk.get(), &*workspace), "walk revisions");
    }
    for (const auto& [name, oid] : repo.data_refs()) {
      if (starts_with(name, "refs/heads/")) {
        check(git_revwalk_push(walk.get(), &oid), "walk revisions");
      }
    }
  }
  const auto workspace = repo.ref_target(kWorkspaceRef);
  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    CommitPtr value = repo.commit(oid);
    const auto id = repo.change_id(oid);
    const auto bookmarks = repo.bookmarks(oid);
    output << (workspace.has_value() && *workspace == oid ? '@'
               : id.has_value()                         ? 'o'
                                                        : '*')
           << "  "
           << (id.has_value() ? repo.short_change_id(*id) : oid_string(oid, 8))
           << ' ' << oid_string(oid, 8);
    for (const std::string& bookmark : bookmarks) {
      output << " " << bookmark;
    }
    const std::string description = first_line(git_commit_message(value.get()));
    output << " " << (description.empty() ? "(no description set)" : description)
           << '\n';
  }
}

void command_edit(Repository& repo,
                  const EditCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  const git_oid target = repo.resolve(options.revision);
  std::map<std::string, git_oid> updates;
  auto id = repo.change_id(target);
  if (!id.has_value()) {
    id = repo.new_change_id();
    updates[std::string(kChangePrefix) + *id] = target;
  }
  finish_workspace(repo, target, std::move(updates), {}, "gg edit");
  output << "Working copy now at: " << repo.short_change_id(*id) << ' '
         << oid_string(target, 8) << '\n';
}

void command_describe(Repository& repo,
                      const DescribeCommand& options,
                      std::ostream& output) {
  if (options.message.empty()) {
    throw UserError("gg describe requires -m DESCRIPTION");
  }
  repo.sync_workspace();
  const git_oid old =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  CommitPtr value = repo.commit(old);
  const git_oid rewritten = repo.rewrite_commit(
      old, repo.parents(old), *git_commit_tree_id(value.get()), options.message);
  RewritePlan plan = repo.descendants({{old, rewritten}});
  const auto workspace = repo.ref_target(kWorkspaceRef);
  const git_oid new_workspace =
      plan.commits.contains(*workspace) ? plan.commits.at(*workspace) : *workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {}, "gg describe");
  output << "Rewrote change as " << oid_string(rewritten, 8) << '\n';
}

void command_move(Repository& repo,
                  const MovementCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  const auto workspace = repo.ref_target(kWorkspaceRef);
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  if (options.conflict) {
    throw UserError("gg has no first-class conflicted revisions");
  }
  if (!options.edit && !repo.children(*workspace).empty()) {
    throw UserError(
        "the working-copy change has children; create a new change or use --edit");
  }

  std::set<git_oid, OidLess> frontier;
  if (options.edit) {
    frontier.insert(*workspace);
  } else {
    const auto parents = repo.parents(*workspace);
    frontier.insert(parents.begin(), parents.end());
  }
  for (std::uint64_t step = 0; step < options.offset; ++step) {
    std::set<git_oid, OidLess> next;
    for (const git_oid& oid : frontier) {
      const auto candidates =
          options.direction == MovementDirection::next ? repo.children(oid)
                                                       : repo.parents(oid);
      for (const git_oid& candidate : candidates) {
        if (!(options.direction == MovementDirection::next && !options.edit &&  // GG_COV_EXCL_BRANCH
              step == 0 && candidate == *workspace)) {
          next.insert(candidate);
        }
      }
    }
    frontier = std::move(next);
  }
  const std::string direction =
      options.direction == MovementDirection::next ? "next" : "previous";
  if (frontier.empty()) {
    throw UserError("no " + direction + " revision found");
  }
  if (frontier.size() != 1) {
    throw UserError("ambiguous " + direction + " revision");
  }
  const git_oid target = *frontier.begin();

  std::map<std::string, git_oid> updates;
  git_oid destination = target;
  std::string id;
  if (options.edit) {
    const auto existing = repo.change_id(target);
    id = existing.value_or(repo.new_change_id());
    if (!existing.has_value()) {
      updates[std::string(kChangePrefix) + id] = target;
    }
  } else {
    id = repo.new_change_id();
    CommitPtr target_commit = repo.commit(target);
    destination = repo.create_commit(*git_commit_tree_id(target_commit.get()),
                                     {target}, "");
    updates[std::string(kChangePrefix) + id] = destination;
  }
  finish_workspace(repo, destination, std::move(updates), {},
                   options.direction == MovementDirection::next ? "gg next"
                                                                : "gg prev");
  output << "Working copy now at: " << repo.short_change_id(id) << ' '
         << oid_string(destination, 8) << '\n';
}

}  // namespace gg::detail
