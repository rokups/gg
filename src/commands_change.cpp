// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>

#include <algorithm>
#include <filesystem>
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

void command_commit(Repository& repo,
                    const CommitCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  if (options.interactive || !options.tool.empty()) {
    throw UserError("interactive commit selection is not supported yet");
  }
  if (options.editor) {
    throw UserError("commit message editing is not supported yet");
  }
  const auto workspace = repo.ref_target(kWorkspaceRef);
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  std::vector<std::string> paths;
  bool select_all = false;
  for (const std::string& path : options.paths) {
    const std::filesystem::path parsed_path(path);
    if (path.empty() || path.front() == '/') {
      throw UserError("commit paths must be repository-relative");
    }
    for (const auto& component : parsed_path) {
      if (component == "..") {
        throw UserError("commit paths must not contain '..'");
      }
    }
    const std::string normalized = parsed_path.lexically_normal().generic_string();
    if (normalized == ".") {
      select_all = true;
    } else {
      paths.push_back(normalized);
    }
  }

  CommitPtr current = repo.commit(*workspace);
  const std::vector<git_oid> parents = repo.parents(*workspace);
  const git_oid base_tree = combined_tree(repo, parents);
  const git_oid full_tree = *git_commit_tree_id(current.get());
  const git_oid selected_tree =
      options.paths.empty() || select_all
          ? full_tree
          : repo.selected_tree(base_tree, full_tree, paths);
  const char* old_message = git_commit_message(current.get());
  const std::string_view message =
      options.message_provided
          ? std::string_view(options.message)
          : std::string_view(old_message == nullptr ? "" : old_message);  // GG_COV_EXCL_BRANCH
  const git_oid committed =
      repo.rewrite_commit(*workspace, parents, selected_tree, message);
  const git_oid new_workspace = repo.create_commit(full_tree, {committed}, "");
  const std::string id = repo.new_change_id();
  RewritePlan plan = repo.descendants({{*workspace, committed}});
  plan.updates[std::string(kChangePrefix) + id] = new_workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {},
                   "gg commit");
  output << "Committed as " << oid_string(committed, 8) << '\n'
         << "Working copy now at: " << repo.short_change_id(id) << ' '
         << oid_string(new_workspace, 8) << '\n';
}

void command_status(Repository& repo,
                    const StatusCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  std::vector<std::string> paths;
  for (const std::string& value : options.paths) {
    const std::filesystem::path path(value);
    if (value.empty() || path.is_absolute()) {
      throw UserError("status paths must be repository-relative");
    }
    for (const auto& component : path) {
      if (component == "..") {
        throw UserError("status paths must not contain '..'");
      }
    }
    const std::string normalized = path.lexically_normal().generic_string();
    if (normalized != ".") {
      paths.push_back(normalized);
    }
  }
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
  std::vector<const git_diff_delta*> deltas;
  for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
    const auto selected = [&](const char* raw_path) {
      const std::string_view path = raw_path;
      if (paths.empty()) {
        return true;
      }
      for (const std::string& prefix : paths) {
        if (path == prefix) {
          return true;
        }
        if (path.starts_with(prefix + "/")) {
          return true;
        }
      }
      return false;
    };
    const bool old_selected = selected(delta->old_file.path);
    const bool new_selected = selected(delta->new_file.path);
    if (old_selected | new_selected) {
      deltas.push_back(delta);
    }
  }
  if (deltas.empty()) {
    output << "The working copy has no changes.\n";
  } else {
    output << "Working copy changes:\n";
    for (const git_diff_delta* delta : deltas) {
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
  if (!options.template_value.empty()) {
    throw UserError("log templates are not supported yet");
  }
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
  std::vector<git_oid> revisions;
  git_oid oid{};
  while (revisions.size() < options.limit &&
         git_revwalk_next(&oid, walk.get()) == 0) {
    if (!options.paths.empty() &&
        !revision_matches_paths(repo, oid, options.paths, options.format)) {
      continue;
    }
    revisions.push_back(oid);
  }
  if (options.count) {
    output << revisions.size() << '\n';
    return;
  }
  if (options.reversed) {
    std::reverse(revisions.begin(), revisions.end());
  }
  bool show_diff = options.patch;
  show_diff |= options.format.summary;
  show_diff |= options.format.stat;
  show_diff |= options.format.types;
  show_diff |= options.format.name_only;
  show_diff |= options.format.git;
  show_diff |= options.format.color_words;
  show_diff |= !options.format.tool.empty();
  show_diff |= options.format.context != 3;
  show_diff |= options.format.ignore_all_space;
  show_diff |= options.format.ignore_space_change;
  for (const git_oid& revision : revisions) {
    const git_oid oid = revision;
    CommitPtr value = repo.commit(oid);
    const auto id = repo.change_id(oid);
    const auto bookmarks = repo.bookmarks(oid);
    if (!options.no_graph) {
      output << (workspace.has_value() && *workspace == oid ? '@'
                 : id.has_value()                         ? 'o'
                                                          : '*')
             << "  ";
    }
    output
           << (id.has_value() ? repo.short_change_id(*id) : oid_string(oid, 8))
           << ' ' << oid_string(oid, 8);
    for (const std::string& bookmark : bookmarks) {
      output << " " << bookmark;
    }
    const std::string description = first_line(git_commit_message(value.get()));
    output << " " << (description.empty() ? "(no description set)" : description)
           << '\n';
    if (show_diff) {
      render_revision_diff(repo, oid, options.paths, options.format, output);
    }
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
