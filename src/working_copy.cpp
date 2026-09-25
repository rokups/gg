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
  return snapshot_tree(baseline_tree, true);
}

git_oid Repository::worktree_tree(const git_oid& baseline_tree) const {
  if (ignore_working_copy_) return baseline_tree;
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open index");
  const std::filesystem::path path = git_index_path(raw_index);
  git_index_free(raw_index);
  std::optional<std::string> saved;
  if (std::ifstream input(path, std::ios::binary); input) {
    saved.emplace(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
  }
  const auto restore = [&] {
    std::error_code error;
    if (!saved.has_value()) {
      std::filesystem::remove(path, error);
      return;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << *saved;
  };
  try {
    const git_oid result = snapshot_tree(baseline_tree);
    restore();
    return result;
  } catch (...) {
    restore();
    throw;
  }
}

DiffPtr Repository::worktree_diff(const git_oid& baseline_tree,
                                  const std::vector<std::string>& paths) const {
  TreePtr baseline = tree(baseline_tree);
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open working-tree index");
  IndexPtr index(raw_index);
  check(git_index_read(index.get(), true), "read working-tree index");
  struct Payload {
    git_index* index;
    const Repository* repository;
  } payload{index.get(), this};
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                  GIT_DIFF_RECURSE_UNTRACKED_DIRS |
                  GIT_DIFF_INCLUDE_TYPECHANGE;
  options.notify_cb = [](const git_diff*, const git_diff_delta* delta,
                         const char*, void* raw_payload) {
    if (delta->status != GIT_DELTA_DELETED ||
        delta->old_file.path == nullptr) return 0;
    auto* index = static_cast<Payload*>(raw_payload)->index;
    const git_index_entry* entry =
        git_index_get_bypath(index, delta->old_file.path, 0);
    return entry != nullptr &&
                   (entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) != 0
               ? 1
               : 0;
  };
  // Checked before each file, so a large scan stops promptly.
  options.progress_cb = [](const git_diff*, const char*, const char*,
                           void* raw_payload) {
    return static_cast<Payload*>(raw_payload)->repository->cancelled()
               ? GIT_EUSER
               : 0;
  };
  options.payload = &payload;
  std::vector<char*> pathspec;
  std::vector<std::string> owned_paths = paths;
  for (std::string& path : owned_paths) pathspec.push_back(path.data());
  options.pathspec = {pathspec.data(), pathspec.size()};
  // When the index holds exactly the baseline tree, its stat data identifies
  // unchanged files without reading them. Comparing the tree directly hashes
  // every file, which takes tens of seconds on large checkouts. Staged,
  // conflicted or sparse indexes keep the direct comparison.
  bool sparse = false;
  for (std::size_t position = 0;
       position < git_index_entrycount(index.get()); ++position) {
    const git_index_entry* entry = git_index_get_byindex(index.get(), position);
    if (entry != nullptr &&
        (entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) != 0) {
      sparse = true;
      break;
    }
  }
  git_oid indexed_tree{};
  const bool prepared =
      !sparse &&
      git_index_write_tree_to(&indexed_tree, index.get(), repo_.get()) == 0 &&
      indexed_tree == baseline_tree;
  if (!prepared) git_error_clear();
  git_diff* raw_diff = nullptr;
  int status = 0;
  if (prepared) {
    // Files whose stat data changed without their content (touched, or
    // rewritten by a checkout) are hashed once and their stat data is saved,
    // so later scans skip them again. A locked index, such as while a Git
    // command runs, only loses that caching.
    options.flags |= GIT_DIFF_UPDATE_INDEX;
    status = git_diff_index_to_workdir(&raw_diff, repo_.get(), index.get(),
                                       &options);
    if (status == GIT_ELOCKED) {
      git_error_clear();
      options.flags &= ~GIT_DIFF_UPDATE_INDEX;
      status = git_diff_index_to_workdir(&raw_diff, repo_.get(), index.get(),
                                         &options);
    }
  } else {
    status = git_diff_tree_to_workdir(&raw_diff, repo_.get(), baseline.get(),
                                      &options);
  }
  if (status == GIT_EUSER && cancelled()) {
    git_error_clear();
    throw UserError("operation cancelled", GIT_EUSER);
  }
  check(status, "compare working tree with active change");
  return DiffPtr(raw_diff);
}

bool Repository::worktree_dirty(const git_oid& baseline_tree) const {
  return git_diff_num_deltas(worktree_diff(baseline_tree).get()) != 0;
}

bool Repository::worktree_tracked_dirty(const git_oid& baseline_tree) const {
  DiffPtr diff = worktree_diff(baseline_tree);
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open working-tree index");
  IndexPtr index(raw_index);
  check(git_index_read(index.get(), true), "read working-tree index");
  const std::filesystem::path workdir = git_repository_workdir(repo_.get());
  for (size_t position = 0; position < git_diff_num_deltas(diff.get()); ++position) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), position);
    if (delta->status == GIT_DELTA_UNTRACKED) continue;
    const char* path = delta->new_file.path;
    if (path != nullptr && git_index_get_bypath(index.get(), path, 0) == nullptr) {
      std::error_code error;
      const auto file = std::filesystem::symlink_status(workdir / path, error);
      if (!error && file.type() != std::filesystem::file_type::not_found) {
        // The caller removed this path from the index. Checkout's untracked
        // collision preflight protects its disk content with a path-specific
        // error, even when the baseline commit still contains the path.
        continue;
      }
    }
    return true;
  }
  return false;
}

bool Repository::head_matches_workspace() const {
  const auto current = workspace();
  if (!current.has_value()) return true;
  const auto actual = head_oid();
  return actual.has_value() && *actual == *current;
}

void Repository::require_current_head() const {
  if (!head_matches_workspace()) {
    throw UserError("Git HEAD changed outside gg; adopt Git history before editing the working tree");
  }
}

bool Repository::adopt_external_head() const {
  if (head_matches_workspace()) return false;
  const auto head = head_oid();
  if (!head.has_value()) return false;
  // Git is in the middle of a multi-step operation (rebase, merge, ...);
  // adopt only its final result.
  if (git_repository_state(repo_.get()) != GIT_REPOSITORY_STATE_NONE) {
    return false;
  }
  // Follow Git's HEAD without touching the disk. Uncommitted edits stay in
  // the working tree, relative to the new commit.
  record({{workspace_ref_name(), *head}}, {}, head_state(), "gg adopt Git HEAD");
  return true;
}

std::optional<std::string> Repository::current_branch() const {
  const HeadState head = head_state();
  if (!head.symbolic || !starts_with(head.value, "refs/heads/")) {
    return std::nullopt;
  }
  return head.value;
}

std::optional<std::string> Repository::branch_checked_out_elsewhere(
    std::string_view branch) const {
  if (git_repository_is_worktree(repo_.get()) == 0 &&
      !std::filesystem::exists(
          std::filesystem::path(git_repository_commondir(repo_.get())) /
          "worktrees")) {
    return std::nullopt;
  }
  for (const WorkspaceRecord& record : workspaces()) {
    if (!record.current && !record.stale && record.branch == branch) {
      return record.name;
    }
  }
  return std::nullopt;
}

git_oid Repository::snapshot_tree(const git_oid& baseline_tree,
                                  bool allow_stat_cache) const {
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open index");
  IndexPtr index(raw_index);
  check(git_index_read(index.get(), true), "refresh index");
  TreePtr baseline = tree(baseline_tree);
  const std::filesystem::path workdir = git_repository_workdir(repo_.get());
  bool sparse = false;
  for (std::size_t position = 0;
       position < git_index_entrycount(index.get()); ++position) {
    const git_index_entry* entry = git_index_get_byindex(index.get(), position);
    if (entry != nullptr &&
        (entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) != 0) {
      sparse = true;
      break;
    }
  }
  git_oid indexed_tree{};
  const bool prepared =
      git_index_write_tree_to(&indexed_tree, index.get(), repo_.get()) == 0 &&  // GG_COV_EXCL_BRANCH
      indexed_tree == baseline_tree;
  if (!prepared && !sparse) {
    git_error_clear();
    check(git_index_read_tree(index.get(), baseline.get()), "prepare snapshot");
  }
  const FileTrackingState tracking = read_tracking(*this);
  if (allow_stat_cache && prepared && !sparse && tracking.tracked.empty() &&
      tracking.untracked.empty() && tracking.forced.empty()) {
    // The cached index already represents the working-copy change, so use it
    // as the stat cache to discover the small set of paths which actually
    // need snapshotting. add_all performs another full worktree traversal and
    // is especially expensive on Windows. Explicit tracking selectors retain
    // the full path because they can change membership even when file contents
    // are unchanged, and inclusive selectors may select ignored files.
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    options.flags = GIT_DIFF_INCLUDE_UNTRACKED |
                    GIT_DIFF_RECURSE_UNTRACKED_DIRS;
    git_diff* raw_diff = nullptr;
    check(git_diff_index_to_workdir(&raw_diff, repo_.get(), index.get(),
                                    &options),
          "discover working-tree changes");
    DiffPtr diff(raw_diff);
    std::vector<std::string> changed_paths;
    for (std::size_t position = 0; position < git_diff_num_deltas(diff.get());
         ++position) {
      const git_diff_delta* delta = git_diff_get_delta(diff.get(), position);
      if (delta == nullptr) continue;  // GG_COV_EXCL_LINE
      if (delta->old_file.path != nullptr)
        changed_paths.emplace_back(delta->old_file.path);
      if (delta->new_file.path != nullptr &&
          (delta->old_file.path == nullptr ||
           std::string_view(delta->new_file.path) != delta->old_file.path))
        changed_paths.emplace_back(delta->new_file.path);
    }
    if (changed_paths.empty()) return baseline_tree;
    diff.reset();
    index.reset();
    return snapshot_tree(baseline_tree, changed_paths);
  }
  if (sparse) {
    // add_all treats absent skip-worktree entries as deletions and rebuilding
    // them makes a sparse checkout scale with the full repository. The status
    // scan already honors sparse boundaries, so add only materialized new
    // paths while update_all handles tracked modifications and deletions.
    std::vector<std::string> materialized;
    materialized.reserve(git_index_entrycount(index.get()));
    for (std::size_t position = 0;
         position < git_index_entrycount(index.get()); ++position) {
      const git_index_entry* entry = git_index_get_byindex(index.get(), position);
      if (entry != nullptr && GIT_INDEX_ENTRY_STAGE(entry) == 0 &&
          (entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) == 0) {
        materialized.emplace_back(entry->path);
      }
    }
    for (const std::string& path : materialized) {
      std::error_code error;
      const auto status = std::filesystem::symlink_status(workdir / path, error);
      if (!error && status.type() != std::filesystem::file_type::not_found) {
        check(git_index_add_bypath(index.get(), path.c_str()),
              "snapshot sparse tracked file");
      } else if (!error || error == std::errc::no_such_file_or_directory) {
        check(git_index_remove_bypath(index.get(), path.c_str()),
              "snapshot sparse deleted file");
      } else {
        throw GitError("inspect sparse working-tree file: " + error.message());
      }
    }
    for (const std::string& path : untracked_paths()) {
      check(git_index_add_bypath(index.get(), path.c_str()),
            "snapshot sparse working-tree file");
    }
  } else {
    check(git_index_update_all(index.get(), nullptr, nullptr, nullptr),
          "snapshot tracked files");
    check(git_index_add_all(index.get(), nullptr, GIT_INDEX_ADD_DEFAULT,
                            nullptr, nullptr),
          "snapshot working tree");
  }
  const std::uint64_t maximum_size = maximum_new_file_size(*this);
  if (maximum_size != 0) {
    std::vector<std::string> oversized;
    for (std::size_t position = 0;
         position < git_index_entrycount(index.get()); ++position) {
      const git_index_entry* entry =
          git_index_get_byindex(index.get(), position);
      if (entry == nullptr ||
          (entry->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE) != 0 ||
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

git_oid Repository::snapshot_tree(
    const git_oid& baseline_tree,
    const std::vector<std::string>& requested_paths) const {
  if (requested_paths.empty()) return snapshot_tree(baseline_tree);

  std::vector<std::string> paths;
  paths.reserve(requested_paths.size());
  for (const std::string& requested : requested_paths) {
    const std::string path = stored_path(requested);
    if (path == "." || path.empty() || path.starts_with("../") ||
        std::filesystem::path(path).is_absolute()) {
      return snapshot_tree(baseline_tree);
    }
    if (std::ranges::find(paths, path) == paths.end()) paths.push_back(path);
  }

  TreePtr baseline = tree(baseline_tree);
  const FileTrackingState tracking = read_tracking(*this);
  const std::uint64_t maximum_size = maximum_new_file_size(*this);
  const std::filesystem::path workdir = git_repository_workdir(repo_.get());
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open index");
  IndexPtr index(raw_index);
  check(git_index_read(index.get(), true), "refresh index");
  std::vector<git_tree_update> updates;
  updates.reserve(paths.size());
  for (const std::string& path : paths) {
    git_tree_entry* raw_baseline_entry = nullptr;
    const int lookup = git_tree_entry_bypath(
        &raw_baseline_entry, baseline.get(), path.c_str());
    TreeEntryPtr baseline_entry(raw_baseline_entry);
    if (lookup != 0 && lookup != GIT_ENOTFOUND) {
      check(lookup, "inspect incremental snapshot baseline");
    }
    if (lookup == GIT_ENOTFOUND) git_error_clear();
    const bool in_baseline = baseline_entry != nullptr;

    const auto selected_by = [&](const std::set<std::string>& selectors) {
      return std::ranges::any_of(
          selectors, [&](const std::string& selector) {
            return selects(selector, path);
          });
    };
    const bool excluded = selected_by(tracking.untracked);
    const bool forced = selected_by(tracking.forced);
    const bool tracked = forced || selected_by(tracking.tracked);

    std::error_code error;
    const auto status = std::filesystem::symlink_status(workdir / path, error);
    const bool missing =
        (!error && status.type() == std::filesystem::file_type::not_found) ||
        error == std::errc::no_such_file_or_directory;
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error) {
      throw GitError("inspect incremental working-tree path: " +
                     error.message());
    }
    if (excluded || missing) {
      if (in_baseline) {
        updates.push_back(
            {GIT_TREE_UPDATE_REMOVE, {}, GIT_FILEMODE_UNREADABLE,
             path.c_str()});
      }
      git_index_remove_bypath(index.get(), path.c_str());
      git_index_remove_directory(index.get(), path.c_str(), 0);
      continue;
    }
    if (status.type() == std::filesystem::file_type::directory) {
      // A directory event can name a dirty submodule (a gitlink). A full
      // fallback must bypass the stat-cache fast path, otherwise the same
      // directory is rediscovered and recursively falls back forever.
      return snapshot_tree(baseline_tree, false);
    }
    if (in_baseline && git_tree_entry_type(baseline_entry.get()) == GIT_OBJECT_TREE) {
      return snapshot_tree(baseline_tree, false);
    }

    if (!in_baseline && !tracked) {
      int ignored = 0;
      check(git_ignore_path_is_ignored(&ignored, repo_.get(), path.c_str()),
            "check incremental ignored path");
      if (ignored != 0) continue;
      if (maximum_size != 0 &&
          status.type() == std::filesystem::file_type::regular) {
        const std::uintmax_t size =
            std::filesystem::file_size(workdir / path, error);
        if (error) {
          throw GitError("inspect incremental file size: " + error.message());
        }
        if (size > maximum_size) continue;
      }
    }

    if (status.type() != std::filesystem::file_type::symlink &&
        status.type() != std::filesystem::file_type::regular) {
      return snapshot_tree(baseline_tree, false);
    }

    // add_bypath applies Git's clean filters and platform configuration, but
    // it also uses the current index entry to decide whether a mode change is
    // significant. Seed it from the snapshot baseline so this path-scoped
    // update has the same semantics as a full working-copy snapshot.
    if (in_baseline) {
      git_index_entry seeded{};
      seeded.id = *git_tree_entry_id(baseline_entry.get());
      seeded.mode = git_tree_entry_filemode(baseline_entry.get());
      seeded.path = path.c_str();
      check(git_index_add(index.get(), &seeded),
            "prepare incremental working-copy file");
    } else {
      git_index_remove_bypath(index.get(), path.c_str());
    }
    check(git_index_add_bypath(index.get(), path.c_str()),
          "snapshot incremental working-copy file");
    const git_index_entry* indexed =
        git_index_get_bypath(index.get(), path.c_str(), 0);
    if (indexed == nullptr) {  // GG_COV_EXCL_BRANCH
      throw GitError(
          "snapshot incremental working-copy file: index entry missing");
    }
    updates.push_back(
        {GIT_TREE_UPDATE_UPSERT, indexed->id,
         static_cast<git_filemode_t>(indexed->mode), path.c_str()});
  }
  if (updates.empty()) return baseline_tree;

  git_oid result{};
  check(git_tree_create_updated(&result, repo_.get(), baseline.get(),
                                updates.size(), updates.data()),
        "write incremental working-copy tree");

  check(git_index_write(index.get()), "cache incremental working-copy snapshot");
  preserve_conflicts(baseline_tree, result);
  return result;
}

std::vector<std::string> Repository::untracked_paths() const {
  return untracked_paths({});
}

std::vector<std::string> Repository::untracked_paths(
    const std::vector<std::string>& pathspecs) const {
  if (operation_view_.has_value() || ignore_working_copy_) return {};
  git_status_options options = GIT_STATUS_OPTIONS_INIT;
  options.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
  options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                  GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;
  std::vector<char*> mutable_pathspecs;
  if (!pathspecs.empty()) {
    mutable_pathspecs.reserve(pathspecs.size());
    for (const std::string& path : pathspecs) {
      mutable_pathspecs.push_back(const_cast<char*>(path.c_str()));
    }
    options.pathspec = {mutable_pathspecs.data(), mutable_pathspecs.size()};
  }
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
                      const std::vector<std::string>& paths,
                      bool preserve_conflicts) const {
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
  std::erase_if(conflicts, [&](const auto& item) {
    if (!selected_path(item.first)) return false;
    return !preserve_conflicts ||
           git_index_get_bypath(final.get(), item.first.c_str(), 0) == nullptr;
  });  // GG_COV_EXCL_BRANCH
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
  const std::set<std::string> legacy_refs = legacy_change_refs();
  if (!legacy_refs.empty()) {
    physical_deletes.insert(legacy_refs.begin(), legacy_refs.end());
  }
  if (ref_target(kLegacyChangeMapRef).has_value()) {
    physical_deletes.insert(std::string(kLegacyChangeMapRef));
  }
  // Transactions should lock only references whose physical value changes.
  std::erase_if(physical_updates, [&](const auto& entry) {
    const auto current = ref_target(entry.first);
    return current.has_value() && *current == entry.second;
  });
  std::erase_if(physical_deletes, [&](const std::string& name) {
    return !ref_target(name).has_value();
  });

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
    if (physical_updates.contains(name) && starts_with(name, kAliasPrefix)) {
      check(git_reference_ensure_log(repo_.get(), name.c_str()),
            "prepare commit alias usage log");
    }
    check(git_transaction_lock_ref(transaction.get(), name.c_str()),
          "lock reference");
  }
  SignaturePtr actor = signature();
  if (std::ranges::any_of(physical_updates, [](const auto& update) {
        return starts_with(update.first, kAliasPrefix);
      })) {
    git_signature* timed = nullptr;
    check(git_signature_new(&timed, actor->name, actor->email,
                            commit_alias_time(), actor->when.offset),
          "timestamp commit alias");
    actor.reset(timed);
  }
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
  const HeadState current = head_state();
  if (current.symbolic == head.symbolic && current.value == head.value) return;
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

HeadState Repository::head_after(const git_oid& workspace,
                                 std::map<std::string, git_oid>& updates,
                                 const std::set<std::string>& deletes,
                                 const HeadIntent& intent) const {
  const auto target_of = [&](const std::string& name) {
    const auto updated = updates.find(name);
    return updated != updates.end() ? std::optional<git_oid>(updated->second)
           : deletes.contains(name) ? std::nullopt
                                    : ref_target(name);
  };
  const HeadState detached{false, oid_string(workspace)};
  if (intent.kind == HeadIntent::Kind::detach) return detached;
  if (intent.kind == HeadIntent::Kind::attach) {
    const auto target = target_of(intent.branch);
    if (!target.has_value()) {
      updates[intent.branch] = workspace;
    } else if (!(*target == workspace)) {
      throw GitError("attached branch must point at the working copy");  // GG_COV_EXCL_LINE
    }
    if (const auto other = branch_checked_out_elsewhere(intent.branch)) {
      throw UserError("branch " + intent.branch.substr(11) +
                      " is checked out in workspace " + *other);
    }
    return {true, intent.branch};
  }
  const HeadState current = head_state();
  if (!current.symbolic || !starts_with(current.value, "refs/heads/") ||
      deletes.contains(current.value)) {
    return detached;
  }
  const auto target = target_of(current.value);
  if (!target.has_value()) {
    // An unborn branch is born by the first commit made on it.
    updates[current.value] = workspace;
    return current;
  }
  return *target == workspace ? current : detached;
}

void Repository::clear_checkout_recovery() const {
  failed_checkout_tree_.reset();
}

void Repository::checkout(std::optional<git_oid> oid) const {
  if (ignore_working_copy_) return;
  const git_oid target_tree = oid.has_value()
                                  ? *git_commit_tree_id(commit(*oid).get())
                                  : empty_tree();
  TreePtr target = tree(target_tree);

  // Snapshot exclusions (large files, explicit untracking, ignores) must not
  // turn a graph checkout into permission to overwrite local-only content.
  git_status_options status_options = GIT_STATUS_OPTIONS_INIT;
  status_options.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
  status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                         GIT_STATUS_OPT_INCLUDE_IGNORED |
                         GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                         GIT_STATUS_OPT_RECURSE_IGNORED_DIRS;
  git_status_list* raw_status = nullptr;
  check(git_status_list_new(&raw_status, repo_.get(), &status_options),
        "inspect checkout local files");
  GitPtr<git_status_list, git_status_list_free> status(raw_status);
  for (std::size_t index = 0; index < git_status_list_entrycount(status.get()); ++index) {
    const git_status_entry* entry = git_status_byindex(status.get(), index);
    if ((entry->status & (GIT_STATUS_WT_NEW | GIT_STATUS_IGNORED)) == 0 ||
        entry->index_to_workdir == nullptr) continue;
    std::string path = entry->index_to_workdir->new_file.path;
    const bool directory = !path.empty() && path.back() == '/';
    if (directory) path.pop_back();
    if (failed_checkout_tree_.has_value()) {
      TreePtr attempted = tree(*failed_checkout_tree_);
      if (tree_contains(attempted.get(), path.c_str())) continue;
    }
    for (std::size_t end = path.find('/');;) {
      const bool full_path = end == std::string::npos;
      const std::string prefix = full_path ? path : path.substr(0, end);
      git_tree_entry* raw_entry = nullptr;
      const int lookup = git_tree_entry_bypath(&raw_entry, target.get(), prefix.c_str());
      if (lookup == GIT_ENOTFOUND) {
        git_error_clear();
        break;
      }
      check(lookup, "inspect checkout destination");
      TreeEntryPtr destination(raw_entry);
      const bool target_directory = git_tree_entry_type(destination.get()) == GIT_OBJECT_TREE;
      if (!target_directory || (full_path && !directory)) {
        throw UserError("checkout would overwrite untracked or ignored path: " + path);
      }
      if (full_path) break;
      end = path.find('/', end + 1);
    }
  }

  // Compare against the actual tracked worktree, not the already-updated HEAD.
  // This also avoids rewriting an already-correct file during error recovery.
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo_.get()), "open checkout index");
  IndexPtr index(raw_index);
  check(git_index_read(index.get(), 1), "read checkout index");
  git_oid baseline_oid{};
  try {
    check(git_index_update_all(index.get(), nullptr, nullptr, nullptr),
          "inspect checkout baseline");
    if (failed_checkout_tree_.has_value()) {
      git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
      index_options.oid_type = git_repository_oid_type(repo_.get());
      git_index* raw_attempted = nullptr;
      check(git_index_new(&raw_attempted, &index_options), "open attempted checkout index");
      IndexPtr attempted(raw_attempted);
      TreePtr attempted_tree = tree(*failed_checkout_tree_);
      check(git_index_read_tree(attempted.get(), attempted_tree.get()), "read attempted checkout");
      const std::filesystem::path workdir = git_repository_workdir(repo_.get());
      for (std::size_t offset = 0; offset < git_index_entrycount(attempted.get()); ++offset) {
        const git_index_entry* entry = git_index_get_byindex(attempted.get(), offset);
        if (git_index_get_bypath(index.get(), entry->path, 0) != nullptr) continue;
        std::error_code error;
        const auto file = std::filesystem::symlink_status(workdir / entry->path, error);
        if (error || !std::filesystem::exists(file) || std::filesystem::is_directory(file)) continue;
        check(git_index_add_bypath(index.get(), entry->path), "inspect partially checked out file");
      }
    }
    check(git_index_write_tree_to(&baseline_oid, index.get(), repo_.get()),
          "write checkout baseline");
  } catch (...) {
    git_index_read(index.get(), 1);
    throw;
  }
  check(git_index_read(index.get(), 1), "restore checkout index");
  TreePtr baseline = tree(baseline_oid);
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.baseline = baseline.get();
  options.checkout_strategy = GIT_CHECKOUT_FORCE |
                              GIT_CHECKOUT_RECREATE_MISSING |
                              GIT_CHECKOUT_DONT_OVERWRITE_IGNORED;
  // Only paths from an actual failed checkout may be removed on recovery.
  // A preflight refusal must leave pre-existing local-only paths untouched.
  failed_checkout_tree_ = target_tree;
  check(git_checkout_tree(repo_.get(),
                          reinterpret_cast<const git_object*>(target.get()),
                          &options),
        "update working copy");
  failed_checkout_tree_.reset();
}

void Repository::add_remote_branch_updates(
    std::map<std::string, git_oid>& updates,
    bool advance_branches,
    std::optional<git_oid>* current_branch_target) const {
  constexpr std::string_view remote_prefix = "refs/remotes/";
  std::set<std::string> tracking_refs;
  for (const auto& [reference, oid] : data_refs()) {
    (void)oid;
    if (starts_with(reference, kBranchTrackingPrefix)) {
      tracking_refs.insert(reference);
    }
  }
  for (const auto& [reference, oid] : updates) {
    (void)oid;
    if (starts_with(reference, kBranchTrackingPrefix)) {
      tracking_refs.insert(reference);
    }
  }

  std::map<std::string, std::set<git_oid, OidLess>> proposals;
  for (const std::string& tracking : tracking_refs) {
    const std::string suffix =
        tracking.substr(kBranchTrackingPrefix.size());
    const std::string remote_ref = std::string(remote_prefix) + suffix;
    const auto remote = ref_target(remote_ref);
    if (!remote.has_value()) continue;

    const auto previous_remote = ref_target(tracking);
    if (previous_remote.has_value() && !(*previous_remote == *remote)) {
      updates[tracking] = *remote;
    }
    if (!advance_branches) continue;

    const std::size_t slash = suffix.find('/');
    if (slash == std::string::npos) continue;  // GG_COV_EXCL_BRANCH
    const std::string local_ref = "refs/heads/" + suffix.substr(slash + 1);
    const auto local = ref_target(local_ref);
    if (!local.has_value() || *local == *remote) continue;
    const int forward = git_graph_descendant_of(raw(), &*remote, &*local);
    check(forward, "reconcile remote branch");
    if (forward != 0) proposals[local_ref].insert(*remote);
  }
  if (proposals.empty()) return;
  std::set<std::string> checked_out;
  for (const WorkspaceRecord& workspace : workspaces()) {
    if (!workspace.branch.empty()) checked_out.insert(workspace.branch);
  }
  const auto current = current_branch();
  for (const auto& [local, targets] : proposals) {
    if (targets.size() != 1) continue;
    if (current.has_value() && local == *current) {
      if (current_branch_target != nullptr) {
        *current_branch_target = *targets.begin();
      }
      continue;
    }
    if (!checked_out.contains(local)) updates[local] = *targets.begin();
  }
}

bool Repository::sync_remote_branches(bool advance_branches) const {
  if (operation_view_.has_value()) return false;
  std::map<std::string, git_oid> updates;
  const auto current_operation = operation();
  if (!current_operation.has_value() ||
      operation_description(*current_operation) == "gg import history" ||
      operation_description(*current_operation) == "initialize repository") {
    constexpr std::string_view remote_prefix = "refs/remotes/";
    for (const auto& [reference, oid] : data_refs()) {
      if (starts_with(reference, remote_prefix) &&
          !reference.ends_with("/HEAD")) {
        updates.emplace(std::string(kBranchTrackingPrefix) +
                            reference.substr(remote_prefix.size()),
                        oid);
      }
    }
  }
  add_remote_branch_updates(updates, advance_branches);
  if (updates.empty()) return false;
  record(std::move(updates), {}, head_state(), "gg import remote branches");
  return true;
}

bool Repository::prepare_command() const {
  if (!adopt_external_changes_ || operation_view_.has_value()) return false;
  // Follow what Git did since the last gg command (commits, checkouts,
  // fetches) before changing anything.
  import_git_history();
  return sync_remote_branches();
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
