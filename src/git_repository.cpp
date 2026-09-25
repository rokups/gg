// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/odb.h>
#include <git2/sys/commit_graph.h>

#include <git2/sys/errors.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

namespace gg::detail {

void check(int result, std::string_view action);

namespace {

std::filesystem::path workspace_name_path(
    const std::filesystem::path& git_directory) {
  return git_directory / "gg" / "workspace";
}

std::filesystem::path workspace_root_path(
    const std::filesystem::path& common_directory, std::string_view name) {
  return common_directory / "gg" / "workspace-roots" / std::string(name);
}

std::optional<std::filesystem::path> stored_workspace_root(
    const std::filesystem::path& common_directory, std::string_view name) {
  std::ifstream input(workspace_root_path(common_directory, name));
  std::string value;
  if (!std::getline(input, value) || value.empty()) return std::nullopt;
  return std::filesystem::path(value);
}

void write_workspace_root(const std::filesystem::path& common_directory,
                          std::string_view name,
                          const std::filesystem::path& root) {
  const std::filesystem::path path = workspace_root_path(common_directory, name);
  std::filesystem::create_directories(path.parent_path());
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output || !(output << root.string() << '\n')) {
      throw UserError("cannot write workspace root");
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw UserError("cannot replace workspace root: " + error.message());
  }
}

std::optional<std::string> stored_workspace_name(
    const std::filesystem::path& git_directory) {
  std::ifstream input(workspace_name_path(git_directory));
  std::string value;
  if (!std::getline(input, value) || value.empty()) return std::nullopt;
  return value;
}

void validate_workspace_name(std::string_view name) {
  const std::string reference = std::string(kWorkspacePrefix) + std::string(name);
  int valid = 0;
  check(git_reference_name_is_valid(&valid, reference.c_str()),
        "validate workspace name");
  if (valid == 0) throw UserError("invalid workspace name: " + std::string(name));
}

std::set<std::string> other_workspace_names(git_repository* repository,
                                            std::string_view current_id) {
  std::set<std::string> result;
  const std::filesystem::path common = git_repository_commondir(repository);
  result.insert(stored_workspace_name(common).value_or("default"));
  git_strarray names{};
  check(git_worktree_list(&names, repository), "list linked worktrees");
  for (std::size_t index = 0; index < names.count; ++index) {
    if (names.strings[index] == current_id) continue;
    result.insert(stored_workspace_name(common / "worktrees" /
                                        names.strings[index])
                      .value_or(names.strings[index]));
  }
  git_strarray_dispose(&names);
  return result;
}

}  // namespace

void check(int result, std::string_view action) {
if (result >= 0) return;
const git_error* error = git_error_last();
std::string message(action);
if (error != nullptr && error->message != nullptr) {  // GG_COV_EXCL_BRANCH
  message += ": ";
  message += error->message;
}
throw GitError(message, result);
}

Libgit2::Libgit2() {
  check(git_libgit2_init(), "initialize libgit2");
  check(register_filter_drivers(), "register Git filter drivers");
}
Libgit2::~Libgit2() { git_libgit2_shutdown(); }

bool OidLess::operator()(const git_oid& left, const git_oid& right) const {
return git_oid_cmp(&left, &right) < 0;
}

bool operator==(const git_oid& left, const git_oid& right) {
return git_oid_equal(&left, &right) != 0;
}

std::string oid_string(const git_oid& oid, std::size_t length) {
std::array<char, GIT_OID_MAX_HEXSIZE + 1> buffer{};
git_oid_tostr(buffer.data(), buffer.size(), &oid);
return std::string(buffer.data(), std::min(length, std::strlen(buffer.data())));
}

std::string first_line(const char* message) {
const std::string text = message == nullptr ? "" : message;
return text.substr(0, text.find('\n'));
}

bool starts_with(std::string_view value, std::string_view prefix) {
return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}
Repository::Repository(const std::filesystem::path& path,
                       bool ignore_working_copy)
    : ignore_working_copy_(ignore_working_copy) {
  git_repository* repository = nullptr;
  check(git_repository_open_ext(&repository, path.string().c_str(),
                                GIT_REPOSITORY_OPEN_CROSS_FS, nullptr),
        "open repository");
  repo_.reset(repository);
  initialize();
}

Repository::Repository(git_repository* repository,
                       bool ignore_working_copy,
                       bool adopt_external_changes)
    : repo_(repository, RepositoryDeleter{false}),
      ignore_working_copy_(ignore_working_copy),
      adopt_external_changes_(adopt_external_changes) {
  if (repository == nullptr) {
    throw UserError("repository must not be null");
  }
  initialize();
}

void Repository::initialize() {
  if (git_repository_is_bare(repo_.get()) != 0) {
    throw UserError("this command requires a working tree", GIT_EBAREREPO);
  }

  // Commit-graphs are optional accelerators. libgit2 deliberately requires
  // callers to attach them to the object database; invalid or stale files
  // must never prevent a repository from opening.
  git_commit_graph* graph = nullptr;
  git_commit_graph_open_options graph_options =
      GIT_COMMIT_GRAPH_OPEN_OPTIONS_INIT;
#ifdef GIT_EXPERIMENTAL_SHA256
  graph_options.oid_type = git_repository_oid_type(repo_.get());
#endif
  const std::filesystem::path objects =
      std::filesystem::path(git_repository_commondir(repo_.get())) / "objects";
  if (git_commit_graph_open(&graph, objects.string().c_str(),
                            &graph_options) == GIT_OK) {
    git_odb* odb = nullptr;
    if (git_repository_odb(&odb, repo_.get()) == GIT_OK) {
      if (git_odb_set_commit_graph(odb, graph) == GIT_OK)
        graph = nullptr;  // Ownership transferred to the ODB.
      git_odb_free(odb);
    }
  }
  if (graph != nullptr) git_commit_graph_free(graph);
  git_error_clear();

  linked_worktree_ = git_repository_is_worktree(repo_.get()) != 0;
  if (linked_worktree_) {
    git_worktree* raw_worktree = nullptr;
    check(git_worktree_open_from_repository(&raw_worktree, repo_.get()),
          "identify linked worktree");
    WorktreePtr worktree(raw_worktree);
    worktree_id_ = git_worktree_name(worktree.get());
  }
  const std::filesystem::path git_directory = git_repository_path(repo_.get());
  workspace_name_ = stored_workspace_name(git_directory).value_or(worktree_id_);
  validate_workspace_name(workspace_name_);
  if (linked_worktree_ && !stored_workspace_name(git_directory).has_value()) {
    const std::set<std::string> occupied =
        other_workspace_names(repo_.get(), worktree_id_);
    std::string candidate = workspace_name_;
    const bool owns_candidate = operation().has_value() &&
                                ref_target(std::string(kWorkspacePrefix) +
                                           candidate)
                                    .has_value();
    for (std::size_t suffix = 2;
         occupied.contains(candidate) ||
         (!owns_candidate &&
          ref_target(std::string(kWorkspacePrefix) + candidate).has_value());
         ++suffix) {
      candidate = workspace_name_ + "-" + std::to_string(suffix);
    }
    set_workspace_name(candidate);
  }
  migrate_legacy_branch_tracking();
}

git_repository* Repository::raw() const { return repo_.get(); }

CommitPtr Repository::commit(const git_oid& oid) const {
  git_commit* value = nullptr;
  check(git_commit_lookup(&value, repo_.get(), &oid), "read commit");
  return CommitPtr(value);
}

TreePtr Repository::tree(const git_oid& oid) const {
  git_tree* value = nullptr;
  check(git_tree_lookup(&value, repo_.get(), &oid), "read tree");
  return TreePtr(value);
}

std::optional<git_oid> Repository::ref_target(std::string_view name) const {
  if (operation_view_.has_value()) {
    if (name == operation_ref_name()) return viewed_operation_;
    if (name == "HEAD") {
      if (operation_view_->head.symbolic) {
        const auto target = operation_view_->refs.find(operation_view_->head.value);
        if (target == operation_view_->refs.end()) return std::nullopt;
        return target->second;
      }
      git_oid oid{};
      check(git_oid_fromstr(&oid, operation_view_->head.value.c_str(),
                            git_repository_oid_type(repo_.get())),
            "parse historical HEAD");
      return oid;
    }
    const auto target = operation_view_->refs.find(std::string(name));
    if (target == operation_view_->refs.end()) return std::nullopt;
    return target->second;
  }
  git_reference* direct = nullptr;
  const int lookup =
      git_reference_lookup(&direct, repo_.get(), std::string(name).c_str());
  if (lookup == GIT_ENOTFOUND) {
    return std::nullopt;
  }
  check(lookup, "read reference");
  ReferencePtr reference(direct);
  if (git_reference_type(reference.get()) == GIT_REFERENCE_SYMBOLIC) {
    git_reference* resolved = nullptr;
    const int resolution = git_reference_resolve(&resolved, reference.get());
    if (resolution == GIT_ENOTFOUND) {
      return std::nullopt;
    }
    check(resolution, "resolve reference");
    reference.reset(resolved);
  }
  return *git_reference_target(reference.get());
}

std::string user_head_ref(const git_oid& oid) {
  return std::string(kVisibleHeadPrefix) + oid_string(oid);
}

HeadState Repository::head_state() const {
  if (operation_view_.has_value()) return operation_view_->head;
  git_reference* head = nullptr;
  check(git_reference_lookup(&head, repo_.get(), "HEAD"), "read HEAD");
  ReferencePtr reference(head);
  if (git_reference_type(reference.get()) == GIT_REFERENCE_SYMBOLIC) {
    return {true, git_reference_symbolic_target(reference.get())};
  }
  return {false, oid_string(*git_reference_target(reference.get()))};
}

std::optional<git_oid> Repository::head_oid() const { return ref_target("HEAD"); }

std::map<std::string, git_oid> Repository::data_refs() const {
  if (operation_view_.has_value()) return operation_view_->refs;
  if (!ref_cache_enabled_) data_refs_cache_.reset();
  if (data_refs_cache_.has_value()) return *data_refs_cache_;
  data_refs_cache_.emplace();
  auto& refs = *data_refs_cache_;
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_new(&raw_iterator, repo_.get()),
        "list references");
  ReferenceIteratorPtr iterator(raw_iterator);
  while (true) {
    git_reference* raw_reference = nullptr;
    const int result = git_reference_next(&raw_reference, iterator.get());
    if (result == GIT_ITEROVER) {
      break;
    }
    check(result, "list references");
    ReferencePtr reference(raw_reference);
    const std::string name = git_reference_name(reference.get());
    if (starts_with(name, "refs/gg/operations/")) {
      continue;
    }
    git_reference* raw_resolved = nullptr;
    const int resolution = git_reference_resolve(&raw_resolved, reference.get());
    if (resolution == GIT_ENOTFOUND) {
      continue;
    }
    check(resolution, "resolve reference");
    ReferencePtr resolved(raw_resolved);
    if (starts_with(name, "refs/heads/") ||
        starts_with(name, "refs/tags/") ||
        starts_with(name, "refs/remotes/") ||
        starts_with(name, kRemoteTagPrefix) ||
        starts_with(name, kBranchTrackingPrefix) ||
        starts_with(name, kTagTrackingPrefix) ||  // GG_COV_EXCL_BRANCH
        starts_with(name, kWorkspacePrefix) ||  // GG_COV_EXCL_BRANCH
        starts_with(name, kVisibleHeadPrefix) || starts_with(name, kAliasPrefix)) {
      refs.emplace(name, *git_reference_target(resolved.get()));
    }
  }
  return refs;
}

std::map<std::string, git_oid> Repository::refs_with_prefix(
    std::string_view prefix) const {
  if (operation_view_.has_value()) {
    std::map<std::string, git_oid> result;
    for (const auto& [name, target] : operation_view_->refs) {
      if (starts_with(name, prefix)) result.emplace(name, target);
    }
    return result;
  }

  const std::string pattern = std::string(prefix) + "*";
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_glob_new(&raw_iterator, repo_.get(),
                                        pattern.c_str()),
        "list references");
  ReferenceIteratorPtr iterator(raw_iterator);
  std::map<std::string, git_oid> result;
  while (true) {
    git_reference* raw_reference = nullptr;
    const int next = git_reference_next(&raw_reference, iterator.get());
    if (next == GIT_ITEROVER) break;
    check(next, "list references");
    ReferencePtr reference(raw_reference);
    git_reference* raw_resolved = nullptr;
    const int resolution = git_reference_resolve(&raw_resolved, reference.get());
    if (resolution == GIT_ENOTFOUND) continue;
    check(resolution, "resolve reference");
    ReferencePtr resolved(raw_resolved);
    const git_oid* target = git_reference_target(resolved.get());
    if (target != nullptr) {
      result.emplace(git_reference_name(reference.get()), *target);
    }
  }
  return result;
}

void Repository::enable_ref_cache() { ref_cache_enabled_ = true; }

void Repository::invalidate_ref_cache() const {
  data_refs_cache_.reset();
  aliases_cache_.reset();
}

std::optional<std::string> Repository::workspace_ref() const {
  const std::string reference = workspace_ref_name();
  return ref_target(reference).has_value()
             ? std::optional<std::string>{reference}
             : std::nullopt;
}

const std::string& Repository::workspace_name() const {
  return workspace_name_;
}

std::string Repository::workspace_ref_name() const {
  return std::string(kWorkspacePrefix) + workspace_name_;
}

std::string Repository::operation_ref_name() const {
  return linked_worktree_
             ? "refs/gg/operations/worktrees/" + worktree_id_
             : std::string(kOperationRef);
}

std::string Repository::rewrite_ref_name() const {
  return linked_worktree_ ? "refs/gg/rewrites/" + worktree_id_
                          : std::string(kRewriteRef);
}

std::map<std::string, std::filesystem::path> Repository::workspace_roots() const {
  std::map<std::string, std::filesystem::path> result;
  for (const WorkspaceRecord& workspace : workspaces()) {
    if (!workspace.stale && !workspace.root.empty()) {
      result.emplace(workspace.name, workspace.root);
    }
  }
  return result;
}

std::vector<WorkspaceRecord> Repository::workspaces() const {
  const std::filesystem::path common = git_repository_commondir(repo_.get());
  const std::filesystem::path current_root =
      std::filesystem::weakly_canonical(git_repository_workdir(repo_.get()));
  std::map<std::string, WorkspaceRecord> result;

  const auto add_worktree = [&](std::string name, std::filesystem::path root,
                                std::string id, bool primary) {
    validate_workspace_name(name);
    if (result.contains(name)) throw UserError("duplicate workspace name: " + name);
    std::error_code error;
    const bool directory = std::filesystem::is_directory(root, error);
    WorkspaceRecord record;
    record.name = std::move(name);
    record.root = std::move(root);
    record.worktree_id = std::move(id);
    record.stale = error || !directory;
    record.primary = primary;
    if (!record.stale) {
      std::error_code canonical_error;
      record.current = std::filesystem::equivalent(
          current_root, record.root, canonical_error);
    }
    result.emplace(record.name, std::move(record));
  };

  git_repository* raw_main = nullptr;
  check(git_repository_open(&raw_main, common.string().c_str()),
        "open primary worktree");
  RepositoryPtr main(raw_main);
  if (const char* primary_root = git_repository_workdir(main.get())) {
    add_worktree(stored_workspace_name(common).value_or("default"),
                 primary_root, {}, true);
  }

  git_strarray ids{};
  check(git_worktree_list(&ids, repo_.get()), "list linked worktrees");
  try {
    for (std::size_t index = 0; index < ids.count; ++index) {
      git_worktree* raw_worktree = nullptr;
      check(git_worktree_lookup(&raw_worktree, repo_.get(), ids.strings[index]),
            "read linked worktree");
      WorktreePtr worktree(raw_worktree);
      const std::string id = ids.strings[index];
      const std::string name =
          stored_workspace_name(common / "worktrees" / id).value_or(id);
      add_worktree(name,
                   git_worktree_path(worktree.get()), id, false);
      git_buf reason = GIT_BUF_INIT;
      const int locked = git_worktree_is_locked(&reason, worktree.get());
      if (locked >= 0) {
        result.at(name).locked = locked != 0;
        if (reason.ptr != nullptr) result.at(name).lock_reason = reason.ptr;
      }
      git_buf_dispose(&reason);
      check(locked, "inspect worktree lock");
    }
  } catch (...) {
    git_strarray_dispose(&ids);
    throw;
  }
  git_strarray_dispose(&ids);

  for (const auto& [reference, oid] : data_refs()) {
    if (!starts_with(reference, kWorkspacePrefix)) continue;
    const std::string name = reference.substr(kWorkspacePrefix.size());
    WorkspaceRecord& record = result[name];
    if (record.name.empty()) {
      record.name = name;
      record.stale = true;
      if (const auto remembered = stored_workspace_root(common, name)) {
        record.root = *remembered;
      }
    }
    record.managed = true;
    record.working_copy = oid;
  }

  // Unmanaged worktrees expose their Git HEAD for navigation without adopting
  // or otherwise writing gg metadata.
  for (auto& [name, record] : result) {
    (void)name;
    if (record.stale) continue;
    git_repository* raw_worktree = nullptr;
    if (git_repository_open(&raw_worktree, record.root.string().c_str()) !=
        GIT_OK) {
      git_error_clear();
      record.stale = true;
      continue;
    }
    RepositoryPtr worktree(raw_worktree);
    const std::filesystem::path expected = record.primary
        ? common : common / "worktrees" / record.worktree_id;
    std::error_code error;
    if (!std::filesystem::equivalent(
            expected, git_repository_path(worktree.get()), error) || error) {
      record.stale = true;
      record.current = false;
      continue;
    }
    git_reference* raw_symbolic = nullptr;
    if (git_reference_lookup(&raw_symbolic, worktree.get(), "HEAD") == GIT_OK) {
      ReferencePtr symbolic(raw_symbolic);
      if (git_reference_type(symbolic.get()) == GIT_REFERENCE_SYMBOLIC &&
          starts_with(git_reference_symbolic_target(symbolic.get()),
                      "refs/heads/")) {
        record.branch = git_reference_symbolic_target(symbolic.get());
      }
    } else {
      git_error_clear();  // GG_COV_EXCL_LINE
    }
    if (record.managed) continue;
    git_object* raw_head = nullptr;
    if (git_revparse_single(&raw_head, worktree.get(), "HEAD") == GIT_OK) {
      ObjectPtr head(raw_head);
      git_object* raw_commit = nullptr;
      if (git_object_peel(&raw_commit, head.get(), GIT_OBJECT_COMMIT) == GIT_OK) {
        ObjectPtr commit(raw_commit);
        record.working_copy = *git_object_id(commit.get());
      } else {
        git_error_clear();
      }
    } else {
      git_error_clear();
    }
  }

  std::vector<WorkspaceRecord> records;
  records.reserve(result.size());
  for (auto& [name, record] : result) records.push_back(std::move(record));
  return records;
}

void Repository::forget_workspace_root(std::string_view name) const {
  std::error_code error;
  std::filesystem::remove(
      workspace_root_path(git_repository_commondir(repo_.get()), name), error);
}

void Repository::remember_workspace_root(
    std::string_view name, const std::filesystem::path& root) const {
  if (!root.empty()) {
    write_workspace_root(git_repository_commondir(repo_.get()), name, root);
  }
}

void Repository::set_workspace_name(std::string_view name) const {
  validate_workspace_name(name);
  const std::string previous = workspace_name_;
  const std::filesystem::path common = git_repository_commondir(repo_.get());
  const auto previous_root = stored_workspace_root(common, name);
  // Write the fallible shared metadata before changing this worktree's name.
  // A failed rename must not make the checkout adopt a different working ref.
  write_workspace_root(common, name,
                       std::filesystem::weakly_canonical(
                           git_repository_workdir(repo_.get())));
  const std::filesystem::path path =
      workspace_name_path(git_repository_path(repo_.get()));
  const std::filesystem::path temporary = path.string() + ".tmp";
  try {
    std::filesystem::create_directories(path.parent_path());
    {
      std::ofstream output(temporary, std::ios::trunc);
      if (!output || !(output << name << '\n')) {  // GG_COV_EXCL_BRANCH
        throw UserError("cannot write workspace name");
      }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
      throw UserError("cannot replace workspace name: " + error.message());
    }
  } catch (...) {
    std::error_code error;
    std::filesystem::remove(temporary, error);
    if (previous_root.has_value()) {
      write_workspace_root(common, name, *previous_root);
    } else {
      forget_workspace_root(name);
    }
    throw;
  }
  workspace_name_ = name;
  if (previous != name) forget_workspace_root(previous);
}

std::optional<git_oid> Repository::workspace() const {
  const auto reference = workspace_ref();
  return reference.has_value() ? ref_target(*reference) : std::nullopt;
}

std::map<std::string, git_oid> Repository::rewrite_refs() const {
  auto refs = data_refs();
  for (auto iterator = refs.begin(); iterator != refs.end();) {
    if (starts_with(iterator->first, "refs/remotes/") ||
        starts_with(iterator->first, kRemoteTagPrefix) ||
        starts_with(iterator->first,  // GG_COV_EXCL_BRANCH
                    kBranchTrackingPrefix) ||  // GG_COV_EXCL_BRANCH
        starts_with(iterator->first,  // GG_COV_EXCL_BRANCH
                    kTagTrackingPrefix)) {  // GG_COV_EXCL_BRANCH
      iterator = refs.erase(iterator);
      continue;
    }
    git_object* raw_object = nullptr;
    git_object* raw_commit = nullptr;
    const int lookup = git_object_lookup(&raw_object, repo_.get(), &iterator->second,
                                         GIT_OBJECT_ANY);
    ObjectPtr object(raw_object);
    const int peel = lookup < 0
                         ? lookup
                         : git_object_peel(&raw_commit, object.get(),
                                           GIT_OBJECT_COMMIT);
    ObjectPtr commit(raw_commit);
    if (peel < 0) {
      git_error_clear();
      iterator = refs.erase(iterator);
      continue;
    }
    ++iterator;
  }
  return refs;
}

SignaturePtr Repository::signature() const {
  git_signature* raw_signature = nullptr;
  if (git_signature_default(&raw_signature, repo_.get()) < 0) {
    git_error_clear();
    check(git_signature_now(&raw_signature, "gg", "gg@localhost"),
          "create signature");
  }
  return SignaturePtr(raw_signature);
}

git_oid Repository::create_commit(const git_oid& tree_oid,
                      const std::vector<git_oid>& parent_oids,
                      std::string_view message,
                      const git_signature* author,
                      const git_signature* committer_override) const {
  TreePtr commit_tree = tree(tree_oid);
  std::vector<CommitPtr> parents;
  std::vector<const git_commit*> parent_pointers;
  parents.reserve(parent_oids.size());
  parent_pointers.reserve(parent_oids.size());
  for (const git_oid& parent_oid : parent_oids) {
    parents.push_back(commit(parent_oid));
    parent_pointers.push_back(parents.back().get());
  }
  SignaturePtr committer = signature();
  const git_signature* actual_author = author == nullptr ? committer.get() : author;
  const git_signature* actual_committer =
      committer_override == nullptr ? committer.get() : committer_override;
  git_oid result{};
  const std::string owned_message(message);
  check(git_commit_create(&result, repo_.get(), nullptr, actual_author,
                          actual_committer, nullptr, owned_message.c_str(),
                          commit_tree.get(), parent_pointers.size(),
                          parent_pointers.data()),
        "write commit");
  return result;
}

git_oid Repository::empty_tree() const {
  git_treebuilder* raw_builder = nullptr;
  check(git_treebuilder_new(&raw_builder, repo_.get(), nullptr),
        "create empty tree");
  GitPtr<git_treebuilder, git_treebuilder_free> builder(raw_builder);
  git_oid result{};
  check(git_treebuilder_write(&result, builder.get()), "write empty tree");
  return result;
}
}  // namespace gg::detail
