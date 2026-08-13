// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <limits>
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

bool has_many_loose_change_refs(git_repository* repository) {
  const std::filesystem::path directory =
      std::filesystem::path(git_repository_commondir(repository)) /
      "refs/gg/changes";
  std::error_code error;
  std::size_t count = 0;
  for (std::filesystem::directory_iterator entry(directory, error), end;
       !error && entry != end; entry.increment(error)) {
    if (++count >= 1024) return true;
  }
  return false;
}

void compress_refs(git_repository* repository) {
  git_refdb* raw_refdb = nullptr;
  check(git_repository_refdb(&raw_refdb, repository),
        "open reference database");
  std::unique_ptr<git_refdb, decltype(&git_refdb_free)> refdb(raw_refdb,
                                                               git_refdb_free);
  check(git_refdb_compress(refdb.get()), "pack references");
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

std::uint64_t maximum_new_file_size(const Repository& repo) {
  constexpr std::uint64_t default_limit = 1024 * 1024;
  git_config* raw_config = nullptr;
  check(git_repository_config(&raw_config, repo.raw()),
        "open Git configuration");
  GitPtr<git_config, git_config_free> config(raw_config);
  git_buf value = GIT_BUF_INIT;
  const int result = git_config_get_string_buf(
      &value, config.get(), "snapshot.max-new-file-size");
  if (result == GIT_ENOTFOUND) {
    git_error_clear();
    return default_limit;
  }
  check(result, "read snapshot.max-new-file-size");
  const auto parsed = parse_file_size(
      std::string_view(value.ptr == nullptr ? "" : value.ptr, value.size));
  git_buf_dispose(&value);
  if (!parsed.has_value()) {
    throw UserError("invalid snapshot.max-new-file-size");
  }
  return *parsed;
}

bool tree_contains(git_tree* tree, const char* path) {
  git_tree_entry* raw_entry = nullptr;
  const int result = git_tree_entry_bypath(&raw_entry, tree, path);
  git_tree_entry_free(raw_entry);
  if (result == GIT_ENOTFOUND) {
    git_error_clear();
    return false;
  }
  check(result, "inspect snapshot baseline");
  return true;
}

bool explicitly_tracked(const FileTrackingState& tracking,
                        std::string_view path) {
  const auto selected = [&](const std::string& selector) {
    return selects(selector, path);
  };
  return std::ranges::any_of(tracking.tracked, selected) ||
         std::ranges::any_of(tracking.forced, selected);
}

}  // namespace

std::optional<std::uint64_t> parse_file_size(std::string_view value) {
  const std::size_t suffix_begin = value.find_first_not_of("0123456789");
  const std::string_view number = value.substr(0, suffix_begin);
  if (number.empty()) return std::nullopt;
  std::uint64_t bytes = 0;
  const auto [end, error] =
      std::from_chars(number.data(), number.data() + number.size(), bytes);
  if (error != std::errc{} || end != number.data() + number.size()) {
    return std::nullopt;
  }
  std::string suffix(suffix_begin == std::string_view::npos
                         ? std::string_view{}
                         : value.substr(suffix_begin));
  std::ranges::transform(suffix, suffix.begin(),
                         [](unsigned char character) {
                           return static_cast<char>(std::toupper(character));
                         });
  std::uint64_t multiplier = 1;
  if (suffix.empty() || suffix == "B") {
    multiplier = 1;
  } else if (suffix == "K" || suffix == "KB" || suffix == "KIB") {
    multiplier = 1024;
  } else if (suffix == "M" || suffix == "MB" || suffix == "MIB") {
    multiplier = 1024 * 1024;
  } else if (suffix == "G" || suffix == "GB" || suffix == "GIB") {
    multiplier = 1024ULL * 1024 * 1024;
  } else {
    return std::nullopt;
  }
  if (bytes > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return std::nullopt;
  }
  return bytes * multiplier;
}

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
  const std::uint64_t maximum_size = maximum_new_file_size(*this);
  if (maximum_size != 0) {
    std::vector<std::string> oversized;
    const std::filesystem::path workdir = git_repository_workdir(repo_.get());
    for (std::size_t position = 0;
         position < git_index_entrycount(index.get()); ++position) {
      const git_index_entry* entry =
          git_index_get_byindex(index.get(), position);
      if (entry == nullptr ||
          (entry->mode != GIT_FILEMODE_BLOB &&
           entry->mode != GIT_FILEMODE_BLOB_EXECUTABLE) ||
          tree_contains(baseline.get(), entry->path) ||
          explicitly_tracked(tracking, entry->path)) {
        continue;
      }
      std::error_code error;
      const std::uintmax_t size =
          std::filesystem::file_size(workdir / entry->path, error);
      if (!error && size > maximum_size) oversized.emplace_back(entry->path);
    }
    for (const std::string& path : oversized) {
      check(git_index_remove_bypath(index.get(), path.c_str()),
            "leave oversized file untracked");
    }
  }
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

std::vector<std::string> Repository::untracked_paths() const {
  if (operation_view_.has_value()) return {};
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
  git_status_list* raw_status = nullptr;
  check(git_status_list_new(&raw_status, repo_.get(), &options),
        "scan untracked files");
  GitPtr<git_status_list, git_status_list_free> status(raw_status);
  std::vector<std::string> paths;
  for (std::size_t index = 0; index < git_status_list_entrycount(status.get());
       ++index) {
    const git_status_entry* entry =
        git_status_byindex(status.get(), index);
    if (entry != nullptr && (entry->status & GIT_STATUS_WT_NEW) != 0 &&
        entry->index_to_workdir != nullptr &&
        entry->index_to_workdir->new_file.path != nullptr) {
      paths.emplace_back(entry->index_to_workdir->new_file.path);
    }
  }
  return paths;
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
  std::map<std::string, git_oid> physical_updates = updates;
  std::set<std::string> physical_deletes = deletes;
  std::map<std::string, CommitAlias> mapped_aliases = read_alias_map();
  bool alias_map_modified = false;
  const std::int64_t now = commit_alias_time();
  for (auto iterator = physical_updates.begin();
       iterator != physical_updates.end();) {
    if (!starts_with(iterator->first, kAliasPrefix)) {
      ++iterator;
      continue;
    }
    const std::string alias = iterator->first.substr(kAliasPrefix.size());
    const auto existing = mapped_aliases.find(alias);
    const std::int64_t last_used =
        existing == mapped_aliases.end() ||
                (oid_string(existing->second.target) == alias &&
                 !(existing->second.target == iterator->second))
            ? now
            : existing->second.last_used;
    mapped_aliases[alias] = {iterator->second, last_used};
    iterator = physical_updates.erase(iterator);
    alias_map_modified = true;
  }
  for (auto iterator = physical_deletes.begin();
       iterator != physical_deletes.end();) {
    if (!starts_with(*iterator, kAliasPrefix)) {
      ++iterator;
      continue;
    }
    mapped_aliases.erase(iterator->substr(kAliasPrefix.size()));
    iterator = physical_deletes.erase(iterator);
    alias_map_modified = true;
  }
  const std::set<std::string> legacy_refs = legacy_change_refs();
  if (!legacy_refs.empty()) {
    physical_deletes.insert(legacy_refs.begin(), legacy_refs.end());
  }
  if (ref_target(kLegacyChangeMapRef).has_value()) {
    physical_deletes.insert(std::string(kLegacyChangeMapRef));
  }
  if (alias_map_modified) {
    if (mapped_aliases.empty()) {
      if (ref_target(kAliasMapRef).has_value()) {
        physical_deletes.insert(std::string(kAliasMapRef));
      }
    } else {
      physical_updates[std::string(kAliasMapRef)] =
          write_alias_map(mapped_aliases);
    }
  }

  const bool should_compress =
      physical_updates.size() + physical_deletes.size() >= 1024 ||
      has_many_loose_change_refs(repo_.get());
  if (physical_updates.empty() && physical_deletes.empty()) {
    if (should_compress) compress_refs(repo_.get());
    return;
  }
  git_transaction* raw_transaction = nullptr;
  check(git_transaction_new(&raw_transaction, repo_.get()),
        "create reference transaction");
  TransactionPtr transaction(raw_transaction);
  std::set<std::string> names;
  for (const auto& [name, oid] : physical_updates) {
    (void)oid;
    names.insert(name);
  }
  names.insert(physical_deletes.begin(), physical_deletes.end());
#ifndef _WIN32
  struct rlimit limit {};
  const rlim_t needed = names.size() + 64;
  if (getrlimit(RLIMIT_NOFILE, &limit) == 0 &&  // GG_COV_EXCL_BRANCH
      limit.rlim_cur < needed) {
    limit.rlim_cur = std::min(limit.rlim_max, needed);
    (void)setrlimit(RLIMIT_NOFILE, &limit);
  }
#endif
  for (const std::string& name : names) {
    check(git_transaction_lock_ref(transaction.get(), name.c_str()),
          "lock reference");
  }
  SignaturePtr actor = signature();
  const std::string owned_message(message);
  for (const auto& [name, oid] : physical_updates) {
    check(git_transaction_set_target(transaction.get(), name.c_str(), &oid,
                                     actor.get(), owned_message.c_str()),
          "queue reference update");
  }
  for (const std::string& name : physical_deletes) {
    check(git_transaction_remove(transaction.get(), name.c_str()),
          "queue reference deletion");
  }
  check(git_transaction_commit(transaction.get()), "update references");
  for (const std::string& name : physical_deletes) {
    const int result = git_reflog_delete(repo_.get(), name.c_str());
    if (result != 0 && result != GIT_ENOTFOUND) {
      check(result, "delete reference log");
    }
  }
  if (should_compress) compress_refs(repo_.get());
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
    const auto head = head_oid();
    const git_oid base_tree = head.has_value()
                                  ? *git_commit_tree_id(commit(*head).get())
                                  : empty_tree();
    const git_oid tree_oid = snapshot_tree(base_tree);
    if (tree_oid == base_tree) return false;
    const std::vector<git_oid> parents =
        head.has_value() ? std::vector<git_oid>{*head} : std::vector<git_oid>{};
    const git_oid imported = create_commit(tree_oid, parents, "");
    record({{workspace_ref_name(), imported}}, {}, head_for_workspace(imported),
           "gg import working copy");
    return true;
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
    record({{*workspace_reference, imported}}, {}, head_for_workspace(imported),
           "gg import state");
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

void Repository::add_remote_bookmark_updates(
    std::map<std::string, git_oid>& updates) const {
  constexpr std::string_view remote_prefix = "refs/remotes/";
  std::set<std::string> tracking_refs;
  for (const auto& [reference, oid] : data_refs()) {
    (void)oid;
    if (starts_with(reference, kBookmarkTrackingPrefix)) {
      tracking_refs.insert(reference);
    }
  }
  for (const auto& [reference, oid] : updates) {
    (void)oid;
    if (starts_with(reference, kBookmarkTrackingPrefix)) {
      tracking_refs.insert(reference);
    }
  }

  std::map<std::string, std::set<git_oid, OidLess>> proposals;
  for (const std::string& tracking : tracking_refs) {
    const std::string suffix =
        tracking.substr(kBookmarkTrackingPrefix.size());
    const std::string remote_ref = std::string(remote_prefix) + suffix;
    const auto remote = ref_target(remote_ref);
    if (!remote.has_value()) continue;

    const auto previous_remote = ref_target(tracking);
    if (previous_remote.has_value() && !(*previous_remote == *remote)) {
      updates[tracking] = *remote;
    }

    const std::size_t slash = suffix.find('/');
    if (slash == std::string::npos) continue;  // GG_COV_EXCL_BRANCH
    const std::string local_ref = "refs/heads/" + suffix.substr(slash + 1);
    const auto local = ref_target(local_ref);
    if (!local.has_value() || *local == *remote) continue;
    const int forward = git_graph_descendant_of(raw(), &*remote, &*local);
    check(forward, "reconcile remote bookmark");
    if (forward != 0) proposals[local_ref].insert(*remote);
  }
  for (const auto& [local, targets] : proposals) {
    if (targets.size() == 1) updates[local] = *targets.begin();
  }
}

bool Repository::sync_remote_bookmarks() const {
  if (operation_view_.has_value()) return false;
  std::map<std::string, git_oid> updates;
  const auto current_operation = operation();
  if (!current_operation.has_value() ||
      operation_description(*current_operation) == "gg import history") {
    constexpr std::string_view remote_prefix = "refs/remotes/";
    for (const auto& [reference, oid] : data_refs()) {
      if (starts_with(reference, remote_prefix) &&
          !reference.ends_with("/HEAD")) {
        updates.emplace(std::string(kBookmarkTrackingPrefix) +
                            reference.substr(remote_prefix.size()),
                        oid);
      }
    }
  }
  add_remote_bookmark_updates(updates);
  if (updates.empty()) return false;
  record(std::move(updates), {}, head_state(), "gg import remote bookmarks");
  return true;
}

bool Repository::sync_for_command() const {
  if (!synchronize_commands_) return false;
  const bool bookmarks_changed = sync_remote_bookmarks();
  return sync_workspace() || bookmarks_changed;
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
