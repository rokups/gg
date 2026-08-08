// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>

#include <filesystem>
#include <set>
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
  const auto workspace = repo.workspace();
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
  const auto workspace = repo.workspace();
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
  const auto workspace = repo.workspace();
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
  std::vector<std::string> revisions = options.revisions;
  revisions.insert(revisions.end(), options.revision_options.begin(),
                   options.revision_options.end());
  if (revisions.empty()) revisions.emplace_back("@");
  const std::vector<git_oid> selected_values =
      resolve_revision_arguments(repo, revisions);
  const std::set<git_oid, OidLess> selected(selected_values.begin(),
                                            selected_values.end());
  if (selected.empty()) {
    output << "Nothing changed.\n";
    return;
  }
  for (const git_oid& oid : selected) {
    if (repo.parents(oid).empty()) {
      throw UserError("cannot abandon a root revision");
    }
  }

  const auto refs = repo.rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {  // GG_COV_EXCL_BRANCH
      check(pushed, "walk revisions");
    }
  }
  for (const git_oid& oid : selected) {
    check(git_revwalk_push(walk.get(), &oid), "walk selected revisions");
  }

  RewritePlan plan;
  std::map<git_oid, std::vector<git_oid>, OidLess> replacements;
  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    const std::vector<git_oid> old_parents = repo.parents(oid);
    std::vector<git_oid> new_parents;
    std::set<git_oid, OidLess> seen;
    bool parents_changed = false;
    for (const git_oid& parent : old_parents) {
      const auto abandoned = replacements.find(parent);
      if (abandoned != replacements.end()) {
        parents_changed = true;
        for (const git_oid& replacement : abandoned->second) {
          if (seen.insert(replacement).second) {
            new_parents.push_back(replacement);
          }
        }
      } else {
        const auto rewritten = plan.commits.find(parent);
        parents_changed |= rewritten != plan.commits.end();
        const git_oid next = rewritten == plan.commits.end()
                                 ? parent
                                 : rewritten->second;
        if (seen.insert(next).second) new_parents.push_back(next);
      }
    }
    if (selected.contains(oid)) {
      replacements.emplace(oid, std::move(new_parents));
    } else if (parents_changed) {
      const std::optional<git_oid> tree_override =
          options.restore_descendants
              ? std::optional(*git_commit_tree_id(repo.commit(oid).get()))
              : std::nullopt;
      plan.commits.emplace(
          oid, repo.rewrite_commit(oid, new_parents, tree_override));
    }
  }

  std::set<std::string> deletes;
  for (const auto& [name, target] : refs) {
    const auto rewritten = plan.commits.find(target);
    if (rewritten != plan.commits.end()) {
      plan.updates[name] = rewritten->second;
      continue;
    }
    if (!selected.contains(target) || starts_with(name, kWorkspacePrefix)) {
      continue;
    }
    if (starts_with(name, kChangePrefix) ||
        (!options.retain_bookmarks && starts_with(name, "refs/heads/"))) {
      deletes.insert(name);
    } else {
      plan.updates[name] = replacements.at(target).front();
    }
  }
  const auto workspace = repo.workspace();
  if (workspace.has_value()) {
    git_oid new_workspace = *workspace;
    if (selected.contains(*workspace)) {
      const std::vector<git_oid>& parents = replacements.at(*workspace);
      new_workspace = repo.create_commit(combined_tree(repo, parents), parents, "");
      plan.updates[std::string(kChangePrefix) + repo.new_change_id()] =
          new_workspace;
    } else if (plan.commits.contains(*workspace)) {
      new_workspace = plan.commits.at(*workspace);
    }
    finish_workspace(repo, new_workspace, std::move(plan.updates),
                     std::move(deletes), "gg abandon");
  } else {
    repo.record(std::move(plan.updates), std::move(deletes), repo.head_state(),
                "gg abandon");
  }
  output << "Abandoned " << selected.size() << " revision(s).\n";
}

void command_restore(Repository& repo,
                     const RestoreCommand& options,
                     std::ostream& output) {
  repo.sync_workspace();
  if (options.interactive || !options.tool.empty()) {
    throw UserError("interactive restore selection is not supported yet");
  }
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }

  git_oid destination{};
  git_oid source_tree{};
  if (!options.from.empty() || !options.into.empty()) {
    destination = repo.resolve(options.into.empty() ? "@" : options.into);
    const git_oid source =
        repo.resolve(options.from.empty() ? "@" : options.from);
    source_tree = *git_commit_tree_id(repo.commit(source).get());
  } else {
    destination =
        repo.resolve(options.changes_in.empty() ? "@" : options.changes_in);
    source_tree = combined_tree(repo, repo.parents(destination));
  }

  std::vector<std::string> paths;
  bool select_all = options.paths.empty();
  for (const std::string& path : options.paths) {
    const std::filesystem::path parsed_path(path);
    if (path.empty() || path.front() == '/') {
      throw UserError("restore paths must be repository-relative");
    }
    for (const auto& component : parsed_path) {
      if (component == "..") {
        throw UserError("restore paths must not contain '..'");
      }
    }
    const std::string normalized = parsed_path.lexically_normal().generic_string();
    if (normalized == ".") {
      select_all = true;
    } else {
      paths.push_back(normalized);
    }
  }

  CommitPtr old = repo.commit(destination);
  const git_oid destination_tree = *git_commit_tree_id(old.get());
  const git_oid restored_tree =
      select_all ? source_tree
                 : repo.selected_tree(destination_tree, source_tree, paths);
  if (restored_tree == destination_tree) {
    output << "Nothing changed.\n";
    return;
  }
  const git_oid rewritten =
      repo.rewrite_commit(destination, repo.parents(destination), restored_tree);
  RewritePlan plan = repo.descendants({{destination, rewritten}}, {},
                                      options.restore_descendants);
  const git_oid new_workspace = plan.commits.contains(*workspace)
                                    ? plan.commits.at(*workspace)
                                    : *workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {},
                   "gg restore");
  output << "Restored into " << oid_string(rewritten, 8) << ".\n";
}

void command_simplify_parents(Repository& repo,
                              const SimplifyParentsCommand& options,
                              std::ostream& output) {
  repo.sync_workspace();
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }

  std::set<git_oid, OidLess> sources;
  std::set<git_oid, OidLess> revisions;
  const std::vector<git_oid> resolved_sources =
      resolve_revision_arguments(repo, options.sources);
  sources.insert(resolved_sources.begin(), resolved_sources.end());
  const std::vector<git_oid> resolved_revisions =
      resolve_revision_arguments(repo, options.revisions);
  revisions.insert(resolved_revisions.begin(), resolved_revisions.end());
  if (sources.empty() && revisions.empty()) {
    std::vector<git_oid> pending{*workspace};
    while (!pending.empty()) {
      const git_oid oid = pending.back();
      pending.pop_back();
      if (revisions.insert(oid).second) {
        const auto parents = repo.parents(oid);
        pending.insert(pending.end(), parents.begin(), parents.end());
      }
    }
  }

  const auto refs = repo.rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {  // GG_COV_EXCL_BRANCH
      check(pushed, "walk revisions");
    }
  }
  for (const git_oid& oid : sources) {
    check(git_revwalk_push(walk.get(), &oid), "walk source revisions");
  }
  for (const git_oid& oid : revisions) {
    check(git_revwalk_push(walk.get(), &oid), "walk selected revisions");
  }

  RewritePlan plan;
  std::set<git_oid, OidLess> source_descendants;
  std::size_t selected_count = 0;
  std::size_t simplified_count = 0;
  std::size_t removed_edges = 0;
  std::size_t reparented_count = 0;
  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    const auto old_parents = repo.parents(oid);
    bool from_source = sources.contains(oid);
    for (const git_oid& parent : old_parents) {
      if (source_descendants.contains(parent)) {
        from_source = true;
      }
    }
    if (from_source) {
      source_descendants.insert(oid);
    }
    const bool selected = from_source || revisions.contains(oid);
    selected_count += selected ? 1 : 0;

    std::vector<git_oid> new_parents;
    new_parents.reserve(old_parents.size());
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      new_parents.push_back(replacement == plan.commits.end()
                                ? parent
                                : replacement->second);
    }
    if (selected && new_parents.size() > 1) {
      std::vector<git_oid> heads;
      for (std::size_t index = 0; index < new_parents.size(); ++index) {
        bool redundant = false;
        for (std::size_t other = 0; other < new_parents.size(); ++other) {
          if (index == other) {
            continue;
          }
          if (new_parents[index] == new_parents[other]) {
            redundant = other < index;
          } else {
            const int descendant = git_graph_descendant_of(
                repo.raw(), &new_parents[other], &new_parents[index]);
            check(descendant, "compare parent ancestry");
            redundant = descendant != 0;
          }
          if (redundant) {
            break;
          }
        }
        if (!redundant) {
          heads.push_back(new_parents[index]);
        }
      }
      if (heads.size() < new_parents.size()) {
        ++simplified_count;
        removed_edges += new_parents.size() - heads.size();
        new_parents = std::move(heads);
      }
    }
    bool parents_changed = new_parents.size() != old_parents.size();
    for (std::size_t index = 0;
         !parents_changed && index < old_parents.size(); ++index) {
      parents_changed = !(new_parents[index] == old_parents[index]);
    }
    if (parents_changed) {
      const git_oid tree = *git_commit_tree_id(repo.commit(oid).get());
      plan.commits.emplace(oid, repo.rewrite_commit(oid, new_parents, tree));
      if (!selected || new_parents.size() == old_parents.size()) {
        ++reparented_count;
      }
    }
  }

  if (plan.commits.empty()) {
    output << "Nothing changed.\n";
    return;
  }
  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  const git_oid new_workspace = plan.commits.contains(*workspace)
                                    ? plan.commits.at(*workspace)
                                    : *workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {},
                   "gg simplify-parents");
  output << "Removed " << removed_edges << " edges from " << simplified_count
         << " out of " << selected_count << " commits.\n";
  if (reparented_count > 0) {
    output << "Rebased " << reparented_count << " descendant commits.\n";
  }
}

}  // namespace gg::detail
