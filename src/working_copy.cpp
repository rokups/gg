// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <sys/resource.h>

#include <algorithm>
#include <fstream>
#include <ranges>
#include <system_error>

namespace gg::detail {
namespace {

struct FileTrackingState {
  std::set<std::string> untracked;
  std::set<std::string> tracked;
  std::set<std::string> forced;
};

std::filesystem::path tracking_path(const Repository& repo) {
  return std::filesystem::path(git_repository_path(repo.raw())) / "gg" /
         "file-tracking";
}

FileTrackingState read_tracking(const Repository& repo) {
  FileTrackingState state;
  std::ifstream input(tracking_path(repo));
  std::string line;
  while (std::getline(input, line)) {
    if (line.size() < 3 || line[1] != ' ') continue;  // GG_COV_EXCL_BRANCH
    if (line[0] == 'U') {
      state.untracked.insert(line.substr(2));
    } else if (line[0] == 'T') {
      state.tracked.insert(line.substr(2));
    } else if (line[0] == 'F') {  // GG_COV_EXCL_BRANCH
      state.forced.insert(line.substr(2));
    }
  }
  return state;
}

void write_tracking(const Repository& repo, const FileTrackingState& state) {
  const std::filesystem::path path = tracking_path(repo);
  std::filesystem::create_directories(path.parent_path());
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
      throw UserError("cannot write file tracking state");
    }
    for (const std::string& value : state.untracked) {
      output << "U " << value << '\n';
    }
    for (const std::string& value : state.tracked) {
      output << "T " << value << '\n';
    }
    for (const std::string& value : state.forced) {
      output << "F " << value << '\n';
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw UserError("cannot replace file tracking state: " + error.message());
  }
}

bool selects(std::string_view selector, std::string_view path) {
  return selector == "." || path == selector ||
         (path.size() > selector.size() && starts_with(path, selector) &&
          path[selector.size()] == '/');
}

void remove_selector(git_index* index, const std::string& selector) {
  if (selector == ".") {
    check(git_index_clear(index), "exclude working-copy paths");
    return;
  }
  git_index_remove_bypath(index, selector.c_str());
  git_index_remove_directory(index, selector.c_str(), 0);
}

void add_selector(git_index* index,
                  const std::string& selector,
                  unsigned int flags) {
  char* value = const_cast<char*>(selector.c_str());
  git_strarray paths{&value, 1};
  check(git_index_add_all(index, selector == "." ? nullptr : &paths, flags,
                          nullptr, nullptr),
        "include working-copy paths");
}

std::string stored_path(const std::string& path) {
  return path.empty() ? "." : path;
}

}  // namespace

git_oid Repository::snapshot_tree(const git_oid& baseline_tree) const {
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open index");
  IndexPtr index(raw_index);
  TreePtr baseline = tree(baseline_tree);
  git_oid indexed_tree{};
  const bool prepared =
      git_index_write_tree_to(&indexed_tree, index.get(), repo_.get()) == 0 &&  // GG_COV_EXCL_BRANCH
      indexed_tree == baseline_tree;
  if (!prepared) {
    git_error_clear();
    check(git_index_read_tree(index.get(), baseline.get()), "prepare snapshot");
  }
  check(git_index_update_all(index.get(), nullptr, nullptr, nullptr),
        "snapshot tracked files");
  check(git_index_add_all(index.get(), nullptr, GIT_INDEX_ADD_DEFAULT, nullptr,
                          nullptr),
        "snapshot working tree");
  const FileTrackingState tracking = read_tracking(*this);
  for (const std::string& path : tracking.untracked) {
    remove_selector(index.get(), path);
  }
  for (const std::string& path : tracking.tracked) {
    add_selector(index.get(), path, GIT_INDEX_ADD_DEFAULT);
  }
  for (const std::string& path : tracking.forced) {
    add_selector(index.get(), path, GIT_INDEX_ADD_FORCE);
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, index.get(), repo_.get()),
        "write working-copy tree");
  check(git_index_write(index.get()), "cache working-copy snapshot");
  preserve_conflicts(baseline_tree, result);
  return result;
}

git_oid Repository::selected_tree(const git_oid& base_tree,
                      const git_oid& final_tree,
                      const std::vector<std::string>& paths) const {
  git_index* raw_selected = nullptr;
  git_index* raw_final = nullptr;
  git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
  index_options.oid_type = git_repository_oid_type(repo_.get());
  check(git_index_new(&raw_selected, &index_options), "create split index");
  check(git_index_new(&raw_final, &index_options), "create split index");
  IndexPtr selected(raw_selected);
  IndexPtr final(raw_final);
  TreePtr base = tree(base_tree);
  TreePtr source = tree(final_tree);
  check(git_index_read_tree(selected.get(), base.get()), "prepare split");
  check(git_index_read_tree(final.get(), source.get()), "prepare split");

  std::vector<std::string> removed_paths;
  for (std::size_t index = 0; index < git_index_entrycount(selected.get());
       ++index) {
    const git_index_entry* entry = git_index_get_byindex(selected.get(), index);
    if (std::ranges::any_of(paths, [&](const auto& fileset) {
          return fileset_matches(fileset, entry->path);
        })) {
      removed_paths.emplace_back(entry->path);
    }
  }
  for (const std::string& path : removed_paths) {
    check(git_index_remove_bypath(selected.get(), path.c_str()),
          "remove selected path");
  }
  for (std::size_t index = 0; index < git_index_entrycount(final.get()); ++index) {
    const git_index_entry* entry = git_index_get_byindex(final.get(), index);
    const std::string_view entry_path(entry->path);
    const bool selected_path = std::ranges::any_of(paths, [&](const auto& fileset) {
      return fileset_matches(fileset, entry_path);
    });
    if (selected_path) {
      check(git_index_add(selected.get(), entry), "select split path");
    }
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, selected.get(), repo_.get()),
        "write selected tree");
  TreeConflicts conflicts = tree_conflicts(base_tree);
  const TreeConflicts final_conflicts = tree_conflicts(final_tree);
  const auto selected_path = [&](std::string_view entry_path) {
    return std::ranges::any_of(paths, [&](const auto& fileset) {
      return fileset_matches(fileset, entry_path);
    });
  };
  std::erase_if(conflicts, [&](const auto& item) { return selected_path(item.first); });  // GG_COV_EXCL_BRANCH
  for (const auto& [path, conflict] : final_conflicts) {
    if (selected_path(path)) conflicts[path] = conflict;
  }
  record_conflicts(result, std::move(conflicts));
  return result;
}

void Repository::apply_refs(const std::map<std::string, git_oid>& updates,
                const std::set<std::string>& deletes,
                std::string_view message) const {
  if (updates.empty() && deletes.empty()) {
    return;
  }
  git_transaction* raw_transaction = nullptr;
  check(git_transaction_new(&raw_transaction, repo_.get()),
        "create reference transaction");
  TransactionPtr transaction(raw_transaction);
  std::set<std::string> names;
  for (const auto& [name, oid] : updates) {
    (void)oid;
    names.insert(name);
  }
  names.insert(deletes.begin(), deletes.end());
  struct rlimit limit {};
  const rlim_t needed = names.size() + 64;
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0 &&  // GG_COV_EXCL_BRANCH
      limit.rlim_cur < needed) {
    limit.rlim_cur = std::min(limit.rlim_max, needed);
    (void)setrlimit(RLIMIT_NOFILE, &limit);
  }
  for (const std::string& name : names) {
    check(git_transaction_lock_ref(transaction.get(), name.c_str()),
          "lock reference");
  }
  SignaturePtr actor = signature();
  const std::string owned_message(message);
  for (const auto& [name, oid] : updates) {
    check(git_transaction_set_target(transaction.get(), name.c_str(), &oid,
                                     actor.get(), owned_message.c_str()),
          "queue reference update");
  }
  for (const std::string& name : deletes) {
    check(git_transaction_remove(transaction.get(), name.c_str()),
          "queue reference deletion");
  }
  check(git_transaction_commit(transaction.get()), "update references");
  invalidate_ref_cache();
}

void Repository::set_head(const HeadState& head) const {
  if (head.symbolic) {
    check(git_repository_set_head(repo_.get(), head.value.c_str()), "set HEAD");
    return;
  }
  git_oid oid{};
  check(git_oid_fromstr(&oid, head.value.c_str(),
                        git_repository_oid_type(repo_.get())),
        "parse HEAD");
  check(git_repository_set_head_detached(repo_.get(), &oid), "detach HEAD");
}

HeadState Repository::head_for_workspace(const git_oid& workspace) const {
  const auto workspace_parents = parents(workspace);
  if (workspace_parents.empty()) {
    return head_state();
  }
  return {false, oid_string(workspace_parents.front())};
}

void Repository::checkout(const git_oid& oid) const {
  if (ignore_working_copy_) return;
  CommitPtr target = commit(oid);
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_FORCE |
                              GIT_CHECKOUT_RECREATE_MISSING;
  check(git_checkout_tree(repo_.get(),
                          reinterpret_cast<const git_object*>(target.get()),
                          &options),
        "update working copy");
}

bool Repository::sync_workspace() const {
  if (ignore_working_copy_) return false;
  const auto workspace_reference = workspace_ref();
  if (!workspace_reference.has_value()) {
    return false;
  }
  const auto workspace = ref_target(*workspace_reference);
  CommitPtr current = commit(*workspace);
  const auto current_parents = parents(*workspace);
  const auto head = head_oid();
  if ((!current_parents.empty() &&
       (!head.has_value() || !(*head == current_parents.front()))) ||
      (current_parents.empty() && head.has_value())) {
    const git_oid base_tree = head.has_value()
                                  ? *git_commit_tree_id(commit(*head).get())
                                  : empty_tree();
    const git_oid tree_oid = snapshot_tree(base_tree);
    const std::vector<git_oid> parent_oids =
        head.has_value() ? std::vector<git_oid>{*head} : std::vector<git_oid>{};
    const git_oid imported = create_commit(tree_oid, parent_oids, "");
    auto missing_ids = missing_change_ids();
    std::string id;
    do {
      id = new_change_id();
    } while (missing_ids.contains(std::string(kChangePrefix) + id));  // GG_COV_EXCL_BRANCH
    std::map<std::string, git_oid> updates{
        {*workspace_reference, imported},
        {std::string(kChangePrefix) + id, imported}};
    updates.merge(missing_ids);
    record(updates, {}, head_for_workspace(imported), "gg import state");
    return true;
  }

  const git_oid old_tree = *git_commit_tree_id(current.get());
  const git_oid new_tree = snapshot_tree(old_tree);
  if (old_tree == new_tree) {
    return false;
  }
  const git_oid rewritten =
      rewrite_commit(*workspace, current_parents, new_tree);
  RewritePlan plan = descendants({{*workspace, rewritten}});
  record(plan.updates, {}, head_for_workspace(rewritten),
         "gg snapshot working copy");
  return true;
}

void Repository::track_paths(const std::vector<std::string>& paths,
                             bool include_ignored) const {
  FileTrackingState state = read_tracking(*this);
  const std::filesystem::path workdir = git_repository_workdir(repo_.get());
  for (const std::string& raw_path : paths) {
    const std::string path = stored_path(raw_path);
    std::error_code error;
    (void)std::filesystem::symlink_status(workdir / path, error);
    if (error) {
      throw UserError("path not found: " + path);
    }
    int ignored = 0;
    if (path != ".") {
      check(git_ignore_path_is_ignored(&ignored, repo_.get(), path.c_str()),
            "check ignored path");
    }
    if (ignored != 0 && !include_ignored) {
      throw UserError("path is ignored: " + path);
    }
    std::erase_if(state.untracked,
                  [&](const std::string& value) { return selects(path, value); });
    std::erase_if(state.tracked,
                  [&](const std::string& value) { return selects(path, value); });
    std::erase_if(state.forced,
                  [&](const std::string& value) { return selects(path, value); });
    (include_ignored ? state.forced : state.tracked).insert(path);
  }
  write_tracking(*this, state);
}

void Repository::untrack_paths(const std::vector<std::string>& paths) const {
  FileTrackingState state = read_tracking(*this);
  for (const std::string& raw_path : paths) {
    const std::string path = stored_path(raw_path);
    std::erase_if(state.tracked,
                  [&](const std::string& value) { return selects(path, value); });
    std::erase_if(state.forced,
                  [&](const std::string& value) { return selects(path, value); });
    state.untracked.insert(path);
  }
  write_tracking(*this, state);
}

}  // namespace gg::detail
