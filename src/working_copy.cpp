// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <algorithm>
#include <ranges>

namespace gg::detail {

git_oid Repository::snapshot_tree(const git_oid& baseline_tree) const {
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open Git index");
  IndexPtr index(raw_index);
  TreePtr baseline = tree(baseline_tree);
  check(git_index_read_tree(index.get(), baseline.get()), "prepare snapshot");
  check(git_index_update_all(index.get(), nullptr, nullptr, nullptr),
        "snapshot tracked files");
  check(git_index_add_all(index.get(), nullptr, GIT_INDEX_ADD_DEFAULT, nullptr,
                          nullptr),
        "snapshot working tree");
  git_oid result{};
  check(git_index_write_tree_to(&result, index.get(), repo_.get()),
        "write working-copy tree");
  return result;
}

git_oid Repository::selected_tree(const git_oid& base_tree,
                      const git_oid& final_tree,
                      const std::vector<std::string>& paths) const {
  git_index* raw_selected = nullptr;
  git_index* raw_final = nullptr;
  check(git_index_new(&raw_selected), "create split index");
  check(git_index_new(&raw_final), "create split index");
  IndexPtr selected(raw_selected);
  IndexPtr final(raw_final);
  TreePtr base = tree(base_tree);
  TreePtr source = tree(final_tree);
  check(git_index_read_tree(selected.get(), base.get()), "prepare split");
  check(git_index_read_tree(final.get(), source.get()), "prepare split");

  for (const std::string& path : paths) {
    git_index_remove_bypath(selected.get(), path.c_str());
    git_index_remove_directory(selected.get(), path.c_str(), 0);
  }
  for (std::size_t index = 0; index < git_index_entrycount(final.get()); ++index) {
    const git_index_entry* entry = git_index_get_byindex(final.get(), index);
    const std::string_view entry_path(entry->path);
    const bool selected_path = std::ranges::any_of(paths, [&](const auto& path) {
      return entry_path == path ||
             (entry_path.size() > path.size() && starts_with(entry_path, path) &&
              entry_path[path.size()] == '/');
    });
    if (selected_path) {
      check(git_index_add(selected.get(), entry), "select split path");
    }
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, selected.get(), repo_.get()),
        "write selected tree");
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
}

void Repository::set_head(const HeadState& head) const {
  if (head.symbolic) {
    check(git_repository_set_head(repo_.get(), head.value.c_str()), "set HEAD");
    return;
  }
  git_oid oid{};
  check(git_oid_fromstr(&oid, head.value.c_str()), "parse HEAD");
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
                              GIT_CHECKOUT_RECREATE_MISSING |
                              GIT_CHECKOUT_REMOVE_UNTRACKED |
                              GIT_CHECKOUT_DONT_UPDATE_INDEX;
  check(git_checkout_tree(repo_.get(),
                          reinterpret_cast<const git_object*>(target.get()),
                          &options),
        "update working copy");
}

bool Repository::sync_workspace() const {
  if (ignore_working_copy_) return false;
  const auto workspace = ref_target(kWorkspaceRef);
  if (!workspace.has_value()) {
    return false;
  }
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
        {std::string(kWorkspaceRef), imported},
        {std::string(kChangePrefix) + id, imported}};
    updates.merge(missing_ids);
    record(updates, {}, head_for_workspace(imported), "gg import Git state");
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


}  // namespace gg::detail
