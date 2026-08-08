// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <git2.h>

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

inline constexpr std::string_view kChangePrefix = "refs/gg/changes/";
inline constexpr std::string_view kWorkspaceRef = "refs/gg/workspaces/default";
inline constexpr std::string_view kOperationRef = "refs/gg/operations/current";
inline constexpr std::string_view kRewriteRef = "refs/gg/rewrite";

class UserError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class GitError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void check(int result, std::string_view action);

template <typename T, void (*Free)(T*)>
struct GitDeleter {
  void operator()(T* value) const {
    if (value != nullptr) Free(value);
  }
};

template <typename T, void (*Free)(T*)>
using GitPtr = std::unique_ptr<T, GitDeleter<T, Free>>;

using RepositoryPtr = GitPtr<git_repository, git_repository_free>;
using ReferencePtr = GitPtr<git_reference, git_reference_free>;
using ReferenceIteratorPtr = GitPtr<git_reference_iterator, git_reference_iterator_free>;
using CommitPtr = GitPtr<git_commit, git_commit_free>;
using TreePtr = GitPtr<git_tree, git_tree_free>;
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

struct MergeConflict {
  git_oid ancestor;
  git_oid ours;
  git_oid theirs;
  IndexPtr index;
  std::vector<std::string> paths;
};

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
                       std::size_t length = GIT_OID_SHA1_HEXSIZE);
std::string first_line(const char* message);
bool starts_with(std::string_view value, std::string_view prefix);

struct HeadState { bool symbolic = true; std::string value; };
struct OperationState { HeadState head; std::map<std::string, git_oid> refs; };
struct RewritePlan {
  std::map<git_oid, git_oid, OidLess> commits;
  std::map<std::string, git_oid> updates;
  std::set<std::string> deletes;
};
struct Resolution { git_oid ancestor; git_oid ours; git_oid theirs; git_oid result; };
struct PendingRewrite {
  git_oid operation;
  std::vector<std::string> arguments;
  std::vector<Resolution> resolutions;
  git_oid ancestor;
  git_oid ours;
  git_oid theirs;
  git_oid marker_tree;
  std::vector<std::string> paths;
};

class Repository {
 public:
  explicit Repository(
      const std::filesystem::path& path,
      std::vector<std::string> config_values = {},
      std::vector<std::filesystem::path> config_files = {});

  git_repository* raw() const;

  CommitPtr commit(const git_oid& oid) const;

  TreePtr tree(const git_oid& oid) const;

  std::optional<git_oid> ref_target(std::string_view name) const;

  HeadState head_state() const;

  std::optional<git_oid> head_oid() const;

  std::map<std::string, git_oid> data_refs() const;

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

  std::optional<git_oid> operation_previous(const git_oid& oid) const;

  std::string operation_description(const git_oid& oid) const;

  std::optional<git_oid> operation_target(const git_oid& oid,
                                            std::string_view prefix) const;

  git_oid create_operation(const OperationState& state,
                             std::optional<git_oid> previous,
                             std::string_view description) const;

  std::optional<git_oid> operation() const;

  git_oid resolve_operation(std::string_view expression) const;

  std::optional<PendingRewrite> pending() const;

  std::string serialize(const PendingRewrite& pending) const;

  void write_pending(const PendingRewrite& pending) const;

  void pause(const std::vector<std::string_view>& arguments,
               MergeConflict& conflict) const;

  std::vector<std::string> prepare_continue() const;

  void finish_rewrite() const;

  void abort_rewrite() const;

  void pending_status(std::ostream& output) const;

  git_oid ensure_operation() const;

  void record(std::map<std::string, git_oid> updates,
                std::set<std::string> deletes,
                const HeadState& head,
                std::string_view description) const;

  void restore_operation(const git_oid& operation_oid,
                         std::string_view description = {},
                         bool restore_repository = true,
                         bool restore_remote_tracking = true) const;

  std::map<std::string, git_oid> changes() const;

  std::map<std::string, git_oid> missing_change_ids() const;

  std::string new_change_id() const;

  std::string short_change_id(std::string_view id) const;

  std::optional<std::string> change_id(const git_oid& oid) const;

  git_oid resolve(std::string_view revision) const;

  bool sync_workspace() const;

  std::vector<std::string> bookmarks(const git_oid& oid) const;

  const std::vector<std::string>& config_values() const;

  const std::vector<std::filesystem::path>& config_files() const;

 private:
  RepositoryPtr repo_;
  std::vector<std::string> config_values_;
  std::vector<std::filesystem::path> config_files_;
};

}  // namespace gg::detail
