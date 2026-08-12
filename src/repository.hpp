// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <git2.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gg::detail {

inline constexpr std::string_view kAliasPrefix = "refs/gg/aliases/";
inline constexpr std::string_view kAliasMapRef = "refs/gg/commit-aliases";
inline constexpr std::string_view kLegacyChangePrefix = "refs/gg/changes/";
inline constexpr std::string_view kLegacyChangeMapRef = "refs/gg/change-map";
inline constexpr std::string_view kWorkspacePrefix = "refs/gg/workspaces/";
inline constexpr std::string_view kWorkspaceRef = "refs/gg/workspaces/default";
inline constexpr std::string_view kOperationRef = "refs/gg/operations/current";
inline constexpr std::string_view kRewriteRef = "refs/gg/rewrite";
inline constexpr std::string_view kConflictPrefix = "refs/gg/conflicts/";
inline constexpr std::string_view kRemoteTagPrefix = "refs/gg/remotes/";
inline constexpr std::string_view kBookmarkTrackingPrefix =
    "refs/gg/tracking/bookmarks/";
inline constexpr std::string_view kTagTrackingPrefix =
    "refs/gg/tracking/tags/";

class UserError : public std::runtime_error {
 public:
  explicit UserError(const std::string& message, int code = GIT_EINVALID)
      : std::runtime_error(message), code_(code) {}

  int code() const { return code_; }

 private:
  int code_;
};

class GitError : public std::runtime_error {
 public:
  explicit GitError(const std::string& message, int code = GIT_ERROR)
      : std::runtime_error(message), code_(code) {}

  int code() const { return code_; }

 private:
  int code_;
};

void check(int result, std::string_view action);
bool string_pattern_matches(std::string_view pattern,
                            std::string_view value,
                            std::string_view default_kind = "glob");
bool any_string_pattern_matches(const std::vector<std::string>& patterns,
                                std::string_view value,
                                std::string_view default_kind = "glob");
bool fileset_matches(std::string_view expression, std::string_view path);

template <typename T, void (*Free)(T*)>
struct GitDeleter {
  void operator()(T* value) const {
    if (value != nullptr) Free(value);
  }
};

template <typename T, void (*Free)(T*)>
using GitPtr = std::unique_ptr<T, GitDeleter<T, Free>>;

struct RepositoryDeleter {
  bool owned{true};
  void operator()(git_repository* value) const {
    if (owned && value != nullptr) git_repository_free(value);
  }
};
using RepositoryPtr = std::unique_ptr<git_repository, RepositoryDeleter>;
using ReferencePtr = GitPtr<git_reference, git_reference_free>;
using ReferenceIteratorPtr = GitPtr<git_reference_iterator, git_reference_iterator_free>;
using CommitPtr = GitPtr<git_commit, git_commit_free>;
using TreePtr = GitPtr<git_tree, git_tree_free>;
using TreeEntryPtr = GitPtr<git_tree_entry, git_tree_entry_free>;
using BlobPtr = GitPtr<git_blob, git_blob_free>;
using IndexPtr = GitPtr<git_index, git_index_free>;
using ConflictIteratorPtr = GitPtr<git_index_conflict_iterator, git_index_conflict_iterator_free>;
using ObjectPtr = GitPtr<git_object, git_object_free>;
using RevwalkPtr = GitPtr<git_revwalk, git_revwalk_free>;
using DiffPtr = GitPtr<git_diff, git_diff_free>;
using DiffStatsPtr = GitPtr<git_diff_stats, git_diff_stats_free>;
using SignaturePtr = GitPtr<git_signature, git_signature_free>;
using TransactionPtr = GitPtr<git_transaction, git_transaction_free>;
using RemotePtr = GitPtr<git_remote, git_remote_free>;
using WorktreePtr = GitPtr<git_worktree, git_worktree_free>;

struct FileValue {
  bool present{false};
  git_oid oid{};
  git_filemode_t mode{GIT_FILEMODE_UNREADABLE};
};
struct ConflictValue {
  std::vector<FileValue> removes;
  std::vector<FileValue> adds;
};
using TreeConflicts = std::map<std::string, ConflictValue>;

class Libgit2 {
 public:
  Libgit2();
  ~Libgit2();
};

struct OidLess {
  bool operator()(const git_oid& left, const git_oid& right) const;
};

bool operator==(const git_oid& left, const git_oid& right);
std::string oid_string(const git_oid& oid,
                       std::size_t length = GIT_OID_MAX_HEXSIZE);
std::string first_line(const char* message);
bool starts_with(std::string_view value, std::string_view prefix);
std::int64_t commit_alias_time();

struct HeadState { bool symbolic = true; std::string value; };
struct ShortId { std::string value; std::size_t prefix_length; };
struct OperationState {
  HeadState head;
  std::map<std::string, git_oid> refs;
  std::string workspace_name;
};
struct RewritePlan {
  std::map<git_oid, git_oid, OidLess> commits;
  std::map<std::string, git_oid> updates;
  std::set<std::string> deletes;
};
struct CommitAlias {
  git_oid target{};
  std::int64_t last_used{};
};

class Repository {
 public:
  explicit Repository(const std::filesystem::path& path,
                      bool ignore_working_copy = false);
  explicit Repository(git_repository* repository,
                      bool ignore_working_copy = false,
                      bool synchronize_commands = false);

  git_repository* raw() const;

  CommitPtr commit(const git_oid& oid) const;

  TreePtr tree(const git_oid& oid) const;

  std::optional<git_oid> ref_target(std::string_view name) const;

  HeadState head_state() const;

  std::optional<git_oid> head_oid() const;

  std::map<std::string, git_oid> data_refs() const;

  void enable_ref_cache();

  void invalidate_ref_cache() const;

  std::optional<std::string> workspace_ref() const;

  const std::string& workspace_name() const;

  std::string workspace_ref_name() const;

  std::string operation_ref_name() const;

  std::string rewrite_ref_name() const;

  std::map<std::string, std::filesystem::path> workspace_roots() const;

  void set_workspace_name(std::string_view name) const;

  std::optional<git_oid> workspace() const;

  std::map<std::string, git_oid> rewrite_refs() const;

  SignaturePtr signature() const;

  git_oid create_commit(const git_oid& tree_oid,
                          const std::vector<git_oid>& parent_oids,
                          std::string_view message,
                          const git_signature* author = nullptr,
                          const git_signature* committer = nullptr) const;

  git_oid empty_tree() const;

  git_oid snapshot_tree(const git_oid& baseline_tree) const;

  git_oid selected_tree(const git_oid& base_tree,
                          const git_oid& final_tree,
                          const std::vector<std::string>& paths) const;

  git_oid merge_trees(const git_oid& ancestor_oid,
                        const git_oid& ours_oid,
                        const git_oid& theirs_oid) const;

  TreeConflicts tree_conflicts(const git_oid& tree_oid) const;

  bool tree_has_conflicts(const git_oid& tree_oid) const;

  bool commit_has_conflicts(const git_oid& commit_oid) const;

  bool history_has_conflicts(const git_oid& commit_oid) const;

  std::vector<std::string> conflict_paths(const git_oid& commit_oid) const;

  void record_conflicts(const git_oid& tree_oid,
                        TreeConflicts conflicts) const;

  void preserve_conflicts(const git_oid& old_tree,
                          const git_oid& new_tree) const;

  git_oid replay(const git_oid& old_parent,
                   const git_oid& new_parent,
                   const git_oid& old_tree) const;

  git_oid rewrite_commit(const git_oid& old_oid,
                           const std::vector<git_oid>& new_parents,
                           std::optional<git_oid> tree_override = std::nullopt,
                           std::optional<std::string_view> message_override =
                               std::nullopt,
                           const git_signature* author_override = nullptr,
                           const git_signature* committer_override = nullptr) const;

  std::vector<git_oid> parents(const git_oid& oid) const;

  std::vector<git_oid> children(const git_oid& oid) const;

  RewritePlan descendants(
        std::map<git_oid, git_oid, OidLess> roots,
        const std::set<git_oid, OidLess>& skipped = {},
        bool preserve_content = false) const;

  RewritePlan move_files(const git_oid& source,
                           const git_oid& destination,
                           const std::vector<std::string>& paths) const;

  void apply_refs(const std::map<std::string, git_oid>& updates,
                    const std::set<std::string>& deletes,
                    std::string_view message) const;

  void set_head(const HeadState& head) const;

  HeadState head_for_workspace(const git_oid& workspace) const;

  void checkout(const git_oid& oid) const;

  OperationState state() const;

  std::string serialize(const OperationState& state,
                          std::optional<git_oid> previous,
                          std::string_view description) const;

  OperationState parse_operation(const git_oid& oid) const;

  OperationState parse_operation(const git_commit* operation) const;

  std::optional<git_oid> operation_previous(const git_oid& oid) const;

  std::optional<git_oid> operation_previous(
      const git_commit* operation) const;

  std::string operation_description(const git_oid& oid) const;

  std::string operation_description(const git_commit* operation) const;

  std::optional<git_oid> operation_target(const git_oid& oid,
                                            std::string_view prefix) const;

  git_oid create_operation(const OperationState& state,
                             std::optional<git_oid> previous,
                             std::string_view description) const;

  std::optional<git_oid> operation() const;

  git_oid resolve_operation(std::string_view expression) const;

  void view_at_operation(std::string_view expression);

  bool has_legacy_rewrite() const;

  git_oid ensure_operation() const;

  void record(std::map<std::string, git_oid> updates,
                std::set<std::string> deletes,
                const HeadState& head,
                std::string_view description,
                bool manage_workspaces = false) const;

  void restore_operation(const git_oid& operation_oid,
                         std::string_view description = {},
                         bool restore_repository = true,
                         bool restore_remote_tracking = true) const;

  void import_git_history(std::ostream* progress = nullptr) const;

  const std::map<std::string, git_oid>& aliases() const;

  ShortId short_commit_id(const git_oid& oid) const;

  void set_short_id_scope(std::span<const git_oid> revisions);

  std::vector<git_oid> commit_aliases(const git_oid& oid) const;

  void add_alias_updates(RewritePlan& plan) const;

  bool collect_expired_aliases(std::string_view description) const;

  git_oid resolve(std::string_view revision) const;

  std::vector<git_oid> resolve_set(std::string_view revisions) const;

  bool sync_workspace() const;

  void add_remote_bookmark_updates(
      std::map<std::string, git_oid>& updates) const;

  bool sync_remote_bookmarks() const;

  bool sync_for_command() const;

  void track_paths(const std::vector<std::string>& paths,
                   bool include_ignored) const;

  void untrack_paths(const std::vector<std::string>& paths) const;

  std::vector<std::string> bookmarks(const git_oid& oid) const;

 private:
  void initialize();

  git_oid resolve_atom(std::string_view revision) const;

  void migrate_operation_history() const;

  std::map<std::string, CommitAlias> read_alias_map() const;

  git_oid write_alias_map(
      const std::map<std::string, CommitAlias>& aliases) const;

  std::set<std::string> legacy_change_refs() const;

  std::set<std::string> expired_alias_refs() const;

  void touch_aliases(const std::vector<std::string>& aliases) const;

  RepositoryPtr repo_;
  bool ignore_working_copy_{false};
  bool synchronize_commands_{true};
  std::optional<OperationState> operation_view_;
  std::optional<git_oid> viewed_operation_;
  mutable std::optional<std::map<std::string, git_oid>> data_refs_cache_;
  mutable std::optional<std::map<std::string, git_oid>> aliases_cache_;
  mutable std::map<git_oid, TreeConflicts, OidLess> conflict_cache_;
  mutable std::map<std::string, git_oid> pending_conflict_refs_;
  std::optional<std::vector<std::string>> scoped_commit_ids_;
  bool ref_cache_enabled_{false};
  bool linked_worktree_{false};
  std::string worktree_id_{"default"};
  mutable std::string workspace_name_{"default"};
};

}  // namespace gg::detail
