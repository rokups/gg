// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "gg/gg.h"

#include "test_support.hpp"
#include "repository.hpp"

#include <algorithm>
#include <cstring>

namespace gg::test {
namespace {

int commit_worktree(gg_mutation_result* out, gg_repository* repository,
                    const char* message, const gg_operation_options* operation) {
  gg_commit_options options = GG_COMMIT_OPTIONS_INIT;
  options.message = message;
  options.message_provided = 1;
  return gg_repository_commit(out, repository, &options, operation);
}

// Amend every working-tree edit into @ and report whether @ changed.
int amend_worktree(int* changed, gg_repository* repository) {
  gg_mutation_result result{};
  const int code = gg_repository_amend(&result, repository, nullptr, nullptr,
                                       nullptr);
  *changed = result.changed;
  gg_mutation_result_dispose(&result);
  return code;
}

// Start an empty change on @ so later amendments have a parent to compare.
void start_change(gg_repository* repository) {
  gg_new_options options = GG_NEW_OPTIONS_INIT;
  gg_mutation_result result{};
  ASSERT_EQ(gg_repository_new_change(&result, repository, &options, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&result);
}

}  // namespace

TEST_F(RepositoryTest, WorktreeStatusLimitsToPathsAndCanBeCancelled) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  write("tracked.txt", "edited\n");
  write("nested/one.txt", "one\n");
  write("nested/deeper/two.txt", "two\n");
  write("other.txt", "other\n");

  // A directory path lists everything below it.
  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  const char* directory = "nested";
  options.filesets = {&directory, 1};
  gg_status status{};
  ASSERT_EQ(gg_repository_worktree_status_ex(&status, repository, &options,
                                             nullptr),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 2U);
  gg_status_dispose(&status);

  // Cancellation is honoured before and during the scan.
  struct Cancel {
    int calls = 0;
    int cancel_after = 0;
  };
  gg_operation_options operation = GG_OPERATION_OPTIONS_INIT;
  operation.cancel_cb = [](void* payload) {
    auto* cancel = static_cast<Cancel*>(payload);
    return ++cancel->calls > cancel->cancel_after ? 1 : 0;
  };
  options.filesets = {nullptr, 0};
  for (const int cancel_after : {0, 1}) {
    Cancel cancel{0, cancel_after};
    operation.payload = &cancel;
    EXPECT_EQ(gg_repository_worktree_status_ex(&status, repository, &options,
                                               &operation),
              GIT_EUSER);
    EXPECT_NE(std::string(git_error_last()->message).find("cancelled"),
              std::string::npos);
    EXPECT_GT(cancel.calls, cancel_after);
  }
  Cancel never{0, 1 << 30};
  operation.payload = &never;
  ASSERT_EQ(gg_repository_worktree_status_ex(&status, repository, &options,
                                             &operation),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 4U);
  gg_status_dispose(&status);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, WorktreeStatusComparesDiskWithActiveTree) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  const std::string original = invoke_git({"show", "HEAD:tracked.txt"}).output;
  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};

  // A staged edit that the disk no longer has is not a change: status
  // compares the disk with the active tree, not with the index.
  write("tracked.txt", "staged value\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  write("tracked.txt", original);
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 0U);
  gg_status_dispose(&status);

  // With the index matching the active tree, its stat cache is used; an
  // edit of the same length is still found.
  ASSERT_EQ(invoke_git({"reset", "-q"}).code, 0);
  std::string same_length = original;
  same_length.front() = same_length.front() == 'x' ? 'y' : 'x';
  write("tracked.txt", same_length);
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  ASSERT_EQ(status.entry_count, 1U);
  EXPECT_STREQ(status.entries[0].new_path, "tracked.txt");
  EXPECT_EQ(status.entries[0].status, GIT_DELTA_MODIFIED);
  gg_status_dispose(&status);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeCommitUsesDiskAndProjectsHead) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  const git_oid base = ref("HEAD");
  write("tracked.txt", "staged value\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  write("tracked.txt", "disk value\n");
  write("new.txt", "new\n");
  write(".gitignore", "ignored.txt\n");
  write("ignored.txt", "ignored\n");

  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 3U);
  EXPECT_TRUE(status.has_working_copy);
  EXPECT_NE(git_oid_equal(&status.working_copy, &base), 0);
  gg_status_dispose(&status);
  EXPECT_FALSE(has_ref(detail::kWorkspaceRef));

  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, repository, "first", nullptr),
            GIT_OK);
  ASSERT_TRUE(result.has_working_copy);
  const git_oid created = result.working_copy;
  gg_mutation_result_dispose(&result);
  const git_oid head = ref("HEAD");
  const git_oid workspace = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&created, &head), 0);
  EXPECT_NE(git_oid_equal(&created, &workspace), 0);
  const git_oid parent = commit_parent(created);
  EXPECT_NE(git_oid_equal(&base, &parent), 0);
  EXPECT_EQ(invoke_git({"show", "HEAD:tracked.txt"}).output, "disk value\n");
  EXPECT_EQ(invoke_git({"show", "HEAD:new.txt"}).output, "new\n");
  EXPECT_EQ(invoke_git({"ls-files", "--", "ignored.txt"}).output, "");
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2", "--untracked-files=all"}).output,
            "");

  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 0U);
  gg_status_dispose(&status);

  write("tracked.txt", "second value\n");
  std::filesystem::remove(path_ / "new.txt");
  write("nested/fresh.txt", "fresh\n");
  const char* selected_path = "tracked.txt";
  options.filesets = {&selected_path, 1};
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 1U);
  gg_status_dispose(&status);
  options.filesets = {nullptr, 0};
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 3U);
  gg_status_dispose(&status);
  ASSERT_EQ(commit_worktree(&result, repository, "second", nullptr),
            GIT_OK);
  const git_oid second = result.working_copy;
  gg_mutation_result_dispose(&result);
  const git_oid second_head = ref("HEAD");
  const git_oid second_parent = commit_parent(second);
  EXPECT_NE(git_oid_equal(&second, &second_head), 0);
  EXPECT_NE(git_oid_equal(&created, &second_parent), 0);
  EXPECT_EQ(invoke_git({"show", "HEAD:tracked.txt"}).output, "second value\n");
  EXPECT_EQ(invoke_git({"show", "HEAD:new.txt"}).code, 128);
  EXPECT_EQ(invoke_git({"show", "HEAD:nested/fresh.txt"}).output, "fresh\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeDetectsExternalGitCommit) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  write("tracked.txt", "first\n");
  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, repository, "first", nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&result);
  write("tracked.txt", "external\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "native"}).code, 0);
  const git_oid native = ref("HEAD");
  EXPECT_EQ(commit_worktree(&result, repository, "wrong", nullptr),
            GIT_EINVALID);
  const git_oid unchanged_head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&native, &unchanged_head), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "external\n");

  // Adoption follows Git's HEAD; @ is the native commit itself.
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  const git_oid imported = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&imported, &native), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "external\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeAdoptsExternalBranchCommit) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  write("tracked.txt", "first\n");
  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, repository, "first", nullptr),
            GIT_OK);
  const git_oid first = result.working_copy;
  gg_mutation_result_dispose(&result);
  ASSERT_EQ(invoke_git({"checkout", "-q", "-B", "main"}).code, 0);
  write("tracked.txt", "external\n");
  ASSERT_EQ(invoke_git({"commit", "-qam", "native"}).code, 0);
  const git_oid native = ref("HEAD");
  write("tracked.txt", "unsaved\n");

  ASSERT_EQ(gg_repository_adopt_git_history_ex(repository, 0, nullptr), GIT_OK);
  const git_oid adopted = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&native, &adopted), 0);
  EXPECT_EQ(invoke_git({"symbolic-ref", "HEAD"}).output, "refs/heads/main\n");
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "unsaved\n");

  // Adoption never snapshots: the edit remains uncommitted on disk.
  EXPECT_EQ(gg_repository_edit(&result, repository,
                                        detail::oid_string(first).c_str(), nullptr),
            GIT_EINVALID);
  write("tracked.txt", "external\n");
  ASSERT_EQ(gg_repository_edit(&result, repository,
                                        detail::oid_string(first).c_str(), nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&result);
  const git_oid head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&head, &first), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "first\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeAmendAdoptsPlainGitHead) {
  const git_oid original = ref("HEAD");
  write("tracked.txt", "amended without workspace\n");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_mutation_result result{};
  const std::string selected = detail::oid_string(original);
  ASSERT_EQ(gg_repository_amend(&result, repository, selected.c_str(),
                                         "amended", nullptr), GIT_OK);
  ASSERT_TRUE(result.has_working_copy);
  const git_oid amended = result.working_copy;
  gg_mutation_result_dispose(&result);
  const git_oid head = ref("HEAD");
  const git_oid workspace = ref(detail::kWorkspaceRef);
  const git_oid main = ref("refs/heads/main");
  EXPECT_NE(git_oid_equal(&head, &amended), 0);
  EXPECT_NE(git_oid_equal(&workspace, &amended), 0);
  EXPECT_NE(git_oid_equal(&main, &amended), 0);
  EXPECT_EQ(invoke_git({"show", "HEAD:tracked.txt"}).output,
            "amended without workspace\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeCommitFromUnbornHead) {
  ASSERT_EQ(invoke_git({"update-ref", "-d", "refs/heads/main"}).code, 0);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, repository, "root", nullptr),
            GIT_OK);
  ASSERT_TRUE(result.has_working_copy);
  const git_oid root = result.working_copy;
  gg_mutation_result_dispose(&result);
  const git_oid head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&head, &root), 0);
  detail::Repository data(path_);
  EXPECT_TRUE(data.parents(root).empty());
  EXPECT_EQ(invoke_git({"show", "HEAD:tracked.txt"}).output, "base\n");
  write("tracked.txt", "native after root\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "native after root"}).code, 0);
  const git_oid native = ref("HEAD");
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  const git_oid imported = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&imported, &native), 0);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeCommitIsolatedInLinkedWorktree) {
  const std::filesystem::path linked = path_.string() + "-explicit-linked";
  std::filesystem::remove_all(linked);
  ASSERT_EQ(invoke_git({"worktree", "add", "--detach", linked.string()}).code,
            0);
  git_repository* linked_raw = nullptr;
  ASSERT_EQ(git_repository_open(&linked_raw, linked.string().c_str()), GIT_OK);
  gg_repository* linked_gg = nullptr;
  ASSERT_EQ(gg_repository_attach(&linked_gg, linked_raw), GIT_OK);
  const git_oid primary_head = ref("HEAD");
  write("primary-only.txt", "primary\n");
  std::ofstream(linked / "linked.txt") << "linked\n";
  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, linked_gg, "linked", nullptr),
            GIT_OK);
  ASSERT_TRUE(result.has_working_copy);
  const git_oid linked_commit = result.working_copy;
  gg_mutation_result_dispose(&result);
  const git_oid still_primary = ref("HEAD");
  EXPECT_NE(git_oid_equal(&primary_head, &still_primary), 0);
  EXPECT_EQ(read_path(path_ / "primary-only.txt"), "primary\n");
  EXPECT_EQ(invoke_git_at(linked, {"rev-parse", "HEAD"}).output,
            gg::detail::oid_string(linked_commit) + "\n");
  EXPECT_EQ(invoke_git_at(linked, {"show", "HEAD:linked.txt"}).output,
            "linked\n");
  gg_repository_free(linked_gg);
  git_repository_free(linked_raw);
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()}).code,
            0);
}

TEST_F(RepositoryTest, ExplicitWorktreeCommitPreservesSparseFiles) {
  write("included/visible.txt", "visible\n");
  write("excluded/hidden.txt", "hidden\n");
  ASSERT_EQ(invoke_git({"add", "."}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "sparse base"}).code, 0);
  ASSERT_EQ(invoke_git({"sparse-checkout", "init", "--cone"}).code, 0);
  ASSERT_EQ(invoke_git({"sparse-checkout", "set", "included"}).code, 0);
  ASSERT_FALSE(std::filesystem::exists(path_ / "excluded/hidden.txt"));
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 0U);
  gg_status_dispose(&status);
  write("included/visible.txt", "changed\n");
  gg_mutation_result result{};
  ASSERT_EQ(commit_worktree(&result, repository, "sparse", nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&result);
  EXPECT_EQ(invoke_git({"show", "HEAD:excluded/hidden.txt"}).output, "hidden\n");
  EXPECT_EQ(invoke_git({"show", "HEAD:included/visible.txt"}).output, "changed\n");
  EXPECT_FALSE(std::filesystem::exists(path_ / "excluded/hidden.txt"));
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeAmendRestacksAndUndoRestores) {
  ASSERT_EQ(invoke({"new", "-m", "first", main_id()}).code, 0);
  write("tracked.txt", "first\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid first = ref(detail::kWorkspaceRef);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid child = ref(detail::kWorkspaceRef);
  set_ref("refs/heads/topic", child);
  ASSERT_EQ(invoke({"edit", gg::detail::oid_string(first)}).code, 0);
  write("tracked.txt", "amended\n");

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_mutation_result result{};
  const std::string selected = gg::detail::oid_string(first);
  ASSERT_EQ(gg_repository_amend(&result, repository, selected.c_str(),
                                         nullptr, nullptr), GIT_OK);
  const git_oid amended = result.working_copy;
  EXPECT_GT(result.rewrite_count, 0U);
  gg_mutation_result_dispose(&result);
  const git_oid amended_head = ref("HEAD");
  const git_oid amended_workspace = ref(detail::kWorkspaceRef);
  const git_oid amended_child_parent = commit_parent(ref("refs/heads/topic"));
  EXPECT_NE(git_oid_equal(&amended, &amended_head), 0);
  EXPECT_NE(git_oid_equal(&amended, &amended_workspace), 0);
  EXPECT_NE(git_oid_equal(&amended, &amended_child_parent), 0);
  EXPECT_EQ(invoke_git({"show", "topic:tracked.txt"}).output, "amended\n");
  EXPECT_EQ(invoke_git({"show", "topic:child.txt"}).output, "child\n");
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "amended\n");
  EXPECT_EQ(gg_repository_amend(&result, repository, selected.c_str(),
                                         nullptr, nullptr), GIT_EINVALIDSPEC);
  const git_oid still_amended = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&amended, &still_amended), 0);

  ASSERT_EQ(gg_repository_undo(&result, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&result);
  const git_oid restored_workspace = ref(detail::kWorkspaceRef);
  const git_oid restored_child = ref("refs/heads/topic");
  EXPECT_NE(git_oid_equal(&first, &restored_workspace), 0);
  EXPECT_NE(git_oid_equal(&child, &restored_child), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "first\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitTreeAmendRestacksWithoutTouchingDisk) {
  ASSERT_EQ(invoke({"new", "-m", "first", main_id()}).code, 0);
  write("tracked.txt", "first\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid first = ref(detail::kWorkspaceRef);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid child = ref(detail::kWorkspaceRef);
  set_ref("refs/heads/topic", child);
  ASSERT_EQ(invoke({"edit", detail::oid_string(first)}).code, 0);
  const git_oid base = commit_parent(first);
  detail::Repository data(path_);
  const git_oid replacement_tree =
      *git_commit_tree_id(data.commit(base).get());
  const std::string selected = detail::oid_string(first);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_mutation_result result{};
  ASSERT_EQ(gg_repository_amend_tree(&result, repository,
                                              selected.c_str(),
                                              &replacement_tree, nullptr),
            GIT_OK);
  const git_oid amended = result.working_copy;
  EXPECT_GT(result.rewrite_count, 0U);
  gg_mutation_result_dispose(&result);
  const git_oid new_head = ref("HEAD");
  const git_oid new_child_parent = commit_parent(ref("refs/heads/topic"));
  EXPECT_NE(git_oid_equal(&new_head, &amended), 0);
  EXPECT_NE(git_oid_equal(&new_child_parent, &amended), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "first\n");
  EXPECT_EQ(invoke_git({"show", "HEAD:tracked.txt"}).output, "base\n");
  EXPECT_EQ(invoke_git({"show", "topic:child.txt"}).output, "child\n");

  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  ASSERT_EQ(status.entry_count, 1U);
  EXPECT_EQ(status.entries[0].status, GIT_DELTA_MODIFIED);
  gg_status_dispose(&status);

  ASSERT_EQ(gg_repository_undo(&result, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&result);
  const git_oid restored = ref(detail::kWorkspaceRef);
  const git_oid restored_child = ref("refs/heads/topic");
  EXPECT_NE(git_oid_equal(&restored, &first), 0);
  EXPECT_NE(git_oid_equal(&restored_child, &child), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "first\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeEditRejectsDirtyDisk) {
  ASSERT_EQ(invoke({"new", "-m", "work", main_id()}).code, 0);
  write("tracked.txt", "committed\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid before = ref(detail::kWorkspaceRef);
  write("tracked.txt", "unsaved\n");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_mutation_result result{};
  EXPECT_EQ(gg_repository_edit(&result, repository, "main", nullptr),
            GIT_EINVALID);
  EXPECT_EQ(gg_repository_undo(&result, repository, nullptr), GIT_EINVALID);
  const git_oid after = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&before, &after), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "unsaved\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ExplicitWorktreeAbandonActivatesParent) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "", main_id()}).code, 0);
  const git_oid empty = ref(detail::kWorkspaceRef);
  write("tracked.txt", "unsaved\n");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  const std::string empty_id = git_oid_tostr_s(&empty);
  const char* revisions[]{empty_id.c_str()};
  gg_abandon_options options = GG_ABANDON_OPTIONS_INIT;
  options.revisions = {revisions, 1};
  gg_mutation_result result{};
  ASSERT_EQ(gg_repository_abandon(&result, repository, &options, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&result);
  // The parent becomes active; no replacement empty change is created.
  const git_oid workspace = ref(detail::kWorkspaceRef);
  const git_oid head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&workspace, &base), 0);
  EXPECT_NE(git_oid_equal(&head, &base), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "unsaved\n");

  // Abandoning a non-empty active commit would overwrite uncommitted edits.
  ASSERT_EQ(invoke({"new", "-m", "work", main_id()}).code, 0);
  write("tracked.txt", "committed\n");
  ASSERT_EQ(invoke({"squash", "-m", "work"}).code, 0);
  const git_oid work = ref(detail::kWorkspaceRef);
  write("tracked.txt", "unsaved again\n");
  const std::string work_id = git_oid_tostr_s(&work);
  revisions[0] = work_id.c_str();
  gg_repository_free(repository);
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  EXPECT_EQ(gg_repository_abandon(&result, repository, &options, nullptr),
            GIT_EINVALID);
  const git_oid unchanged = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&unchanged, &work), 0);
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "unsaved again\n");
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ListsExistingGitWorktreesBeforeTheyAreManaged) {
  const std::filesystem::path linked = path_.string() + "-existing-worktree";
  std::filesystem::remove_all(linked);
  const Result added =
      invoke_git({"worktree", "add", "--detach", linked.string()});
  ASSERT_EQ(added.code, 0) << added.error;

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  gg_workspace_array workspaces{};
  ASSERT_EQ(gg_repository_workspaces(&workspaces, repository), GIT_OK);
  ASSERT_EQ(workspaces.count, 2U);

  const auto existing = std::find_if(
      workspaces.items, workspaces.items + workspaces.count,
      [&](const gg_workspace& workspace) {
        return workspace.root != nullptr &&
               std::filesystem::weakly_canonical(workspace.root) ==
                   std::filesystem::weakly_canonical(linked);
      });
  ASSERT_NE(existing, workspaces.items + workspaces.count);
  EXPECT_TRUE(existing->has_working_copy);
  EXPECT_FALSE(existing->stale);
  EXPECT_FALSE(existing->managed);
  EXPECT_FALSE(existing->current);
  EXPECT_FALSE(existing->primary);
  const git_oid head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&existing->working_copy, &head), 0);

  gg_workspace_array_dispose(&workspaces);
  gg_repository_free(repository);
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()})
                .code,
            0);
}

TEST_F(RepositoryTest, ExposesStructuredCWorkflowApi) {
  gg_operation_options operation = GG_OPERATION_OPTIONS_INIT;
  gg_new_options new_options = GG_NEW_OPTIONS_INIT;
  gg_commit_options commit_options = GG_COMMIT_OPTIONS_INIT;
  gg_describe_options describe_options = GG_DESCRIBE_OPTIONS_INIT;
  gg_metaedit_options metaedit_options = GG_METAEDIT_OPTIONS_INIT;
  gg_rebase_options rebase_options = GG_REBASE_OPTIONS_INIT;
  gg_duplicate_options duplicate_options = GG_DUPLICATE_OPTIONS_INIT;
  gg_reorder_options reorder_options = GG_REORDER_OPTIONS_INIT;
  gg_split_options split_options = GG_SPLIT_OPTIONS_INIT;
  gg_squash_options squash_options = GG_SQUASH_OPTIONS_INIT;
  gg_abandon_options abandon_options = GG_ABANDON_OPTIONS_INIT;
  gg_restore_options restore_options = GG_RESTORE_OPTIONS_INIT;
  gg_move_files_options move_files_options = GG_MOVE_FILES_OPTIONS_INIT;
  gg_simplify_parents_options simplify_options =
      GG_SIMPLIFY_PARENTS_OPTIONS_INIT;
  gg_branch_options branch_options = GG_BRANCH_OPTIONS_INIT;
  gg_tag_options tag_options = GG_TAG_OPTIONS_INIT;
  gg_move_options move_options = GG_MOVE_OPTIONS_INIT;
  gg_workspace_add_options workspace_options = GG_WORKSPACE_ADD_OPTIONS_INIT;
  gg_fetch_options fetch_options = GG_FETCH_OPTIONS_INIT;
  gg_push_options push_options = GG_PUSH_OPTIONS_INIT;
  gg_revision_query_options revision_options =
      GG_REVISION_QUERY_OPTIONS_INIT;
  gg_status_options status_options = GG_STATUS_OPTIONS_INIT;
  EXPECT_EQ(gg_operation_options_init(&operation, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_new_options_init(&new_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_commit_options_init(&commit_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_describe_options_init(&describe_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_metaedit_options_init(&metaedit_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_rebase_options_init(&rebase_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_duplicate_options_init(&duplicate_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_reorder_options_init(&reorder_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_split_options_init(&split_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_squash_options_init(&squash_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_abandon_options_init(&abandon_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_restore_options_init(&restore_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_move_files_options_init(&move_files_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_simplify_parents_options_init(&simplify_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_branch_options_init(&branch_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_tag_options_init(&tag_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_move_options_init(&move_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_workspace_add_options_init(&workspace_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_fetch_options_init(&fetch_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_push_options_init(&push_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_revision_query_options_init(&revision_options, GG_OPTIONS_VERSION), GIT_OK);
  EXPECT_EQ(gg_status_options_init(&status_options, GG_OPTIONS_VERSION), GIT_OK);

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_operation_capabilities capabilities{};
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  // Adopting the repository is its initial state, not an undoable step.
  EXPECT_FALSE(capabilities.can_undo);
  EXPECT_FALSE(capabilities.can_redo);

  revision_options.revisions = "HEAD";
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &revision_options),
            GIT_OK);
  ASSERT_EQ(revisions.count, 1U);
  EXPECT_EQ(revisions.items[0].aliases.count, 0U);
  EXPECT_STREQ(revisions.items[0].description, "base");
  EXPECT_STREQ(revisions.items[0].author->name, "GG Test");
  gg_revision_array_dispose(&revisions);

  gg_status status{};
  ASSERT_EQ(gg_repository_status(&status, repository, &status_options), GIT_OK);
  // @ is Git's HEAD as soon as gg adopts the repository.
  EXPECT_TRUE(status.has_working_copy);
  gg_status_dispose(&status);

  new_options.message = "work";
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &new_options,
                                     nullptr),
            GIT_OK);
  ASSERT_TRUE(mutation.changed);
  ASSERT_TRUE(mutation.has_working_copy);
  const git_oid working = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  git_oid queried_working{};
  ASSERT_EQ(gg_repository_working_copy(&queried_working, repository), GIT_OK);
  EXPECT_TRUE(git_oid_equal(&working, &queried_working));

  ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  EXPECT_FALSE(capabilities.can_undo);
  EXPECT_TRUE(capabilities.can_redo);
  ASSERT_EQ(gg_repository_redo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  EXPECT_TRUE(capabilities.can_undo);
  EXPECT_FALSE(capabilities.can_redo);

  gg_workspace_array workspaces{};
  ASSERT_EQ(gg_repository_workspaces(&workspaces, repository), GIT_OK);
  ASSERT_EQ(workspaces.count, 1U);
  EXPECT_STREQ(workspaces.items[0].name, "default");
  EXPECT_TRUE(workspaces.items[0].has_working_copy);
  EXPECT_FALSE(workspaces.items[0].stale);
  EXPECT_TRUE(workspaces.items[0].managed);
  EXPECT_TRUE(workspaces.items[0].current);
  EXPECT_TRUE(workspaces.items[0].primary);
  gg_workspace_array_dispose(&workspaces);

  write("tracked.txt", "changed\n");
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  EXPECT_TRUE(changed);
  revision_options.revisions = "@";
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &revision_options),
            GIT_OK);
  ASSERT_EQ(revisions.count, 1U);
  ASSERT_GT(revisions.items[0].aliases.count, 0U);
  EXPECT_TRUE(std::any_of(
      revisions.items[0].aliases.ids,
      revisions.items[0].aliases.ids + revisions.items[0].aliases.count,
      [&](const git_oid& alias) { return git_oid_equal(&alias, &working); }));
  gg_revision_array_dispose(&revisions);
  ASSERT_EQ(gg_repository_status(&status, repository, &status_options), GIT_OK);
  ASSERT_TRUE(status.has_working_copy);
  ASSERT_EQ(status.entry_count, 1U);
  EXPECT_EQ(status.entries[0].status, GIT_DELTA_MODIFIED);
  EXPECT_STREQ(status.entries[0].new_path, "tracked.txt");
  gg_status_dispose(&status);

  const char* tracked_path[] = {"tracked.txt"};
  const gg_string_array tracked_paths{tracked_path, 1};
  ASSERT_EQ(gg_repository_untrack_paths(&mutation, repository, tracked_paths,
                                        nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_track_paths(&mutation, repository, tracked_paths, 0,
                                      nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);

  const char* topic[] = {"topic"};
  branch_options.action = GG_BRANCH_CREATE;
  branch_options.names = {topic, 1};
  branch_options.revision = "@";
  ASSERT_EQ(gg_repository_branch(&mutation, repository, &branch_options,
                                   nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);
  tag_options.action = GG_TAG_SET;
  tag_options.names = {topic, 1};
  tag_options.revision = "@";
  ASSERT_EQ(gg_repository_tag(&mutation, repository, &tag_options, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  gg_reference_array references{};
  ASSERT_EQ(gg_repository_references(&references, repository), GIT_OK);
  EXPECT_GT(references.count, 3U);
  gg_reference_array_dispose(&references);
  gg_named_ref_array named_refs{};
  ASSERT_EQ(gg_repository_named_refs(&named_refs, repository), GIT_OK);
  ASSERT_EQ(named_refs.count, 3U);
  bool found_branch = false;
  bool found_tag = false;
  for (size_t index = 0; index < named_refs.count; ++index) {
    const gg_named_ref& item = named_refs.items[index];
    if (std::strcmp(item.name, "topic") == 0 &&
        item.kind == GG_NAMED_REF_LOCAL_BRANCH) {
      found_branch = true;
    }
    if (std::strcmp(item.name, "topic") == 0 &&
        item.kind == GG_NAMED_REF_LOCAL_TAG) {
      found_tag = true;
    }
    EXPECT_STREQ(item.remote, "");
    EXPECT_FALSE(item.conflicted);
  }
  EXPECT_TRUE(found_branch);
  EXPECT_TRUE(found_tag);
  gg_named_ref_array_dispose(&named_refs);
  gg_conflict_array conflicts{};
  ASSERT_EQ(gg_repository_conflicts(&conflicts, repository, &working), GIT_OK);
  EXPECT_EQ(conflicts.count, 0U);
  gg_conflict_array_dispose(&conflicts);
  gg_operation_array operations{};
  ASSERT_EQ(gg_repository_operations(&operations, repository, 100), GIT_OK);
  EXPECT_GT(operations.count, 0U);
  gg_operation_array_dispose(&operations);
  gg_owned_string_array sparse{};
  ASSERT_EQ(gg_repository_sparse_patterns(&sparse, repository), GIT_OK);
  ASSERT_EQ(sparse.count, 1U);
  EXPECT_STREQ(sparse.strings[0], ".");
  gg_owned_string_array_dispose(&sparse);

  git_remote* remote = nullptr;
  ASSERT_EQ(git_remote_create(&remote, repository_.get(), "origin",
                              path_.string().c_str()),
            GIT_OK);
  git_remote_free(remote);
  push_options.branches = {topic, 1};
  gg_transport_plan push_plan{};
  ASSERT_EQ(gg_repository_plan_push(&push_plan, repository, &push_options),
            GIT_OK);
  ASSERT_EQ(push_plan.refspec_count, 1U);
  EXPECT_TRUE(push_plan.atomic);
  ASSERT_EQ(gg_repository_complete_push(&mutation, repository, &push_plan,
                                        nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  gg_transport_plan_dispose(&push_plan);

  const git_oid head = ref("HEAD");
  const gg_advertised_ref advertised{
      "origin", "main", head, GG_REMOTE_BRANCH};
  fetch_options.advertised_refs = &advertised;
  fetch_options.advertised_ref_count = 1;
  gg_transport_plan fetch_plan{};
  ASSERT_EQ(gg_repository_plan_fetch(&fetch_plan, repository, &fetch_options),
            GIT_OK);
  ASSERT_EQ(fetch_plan.refspec_count, 1U);
  set_ref("refs/remotes/origin/main", head);
  ASSERT_EQ(gg_repository_complete_fetch(&mutation, repository, &fetch_plan,
                                         nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  gg_transport_plan_dispose(&fetch_plan);

  gg_repository_free(repository);
}

TEST_F(RepositoryTest, RefreshesCachedReferencesAfterAdoptingGitChanges) {
  const git_oid base = ref("refs/heads/main");
  set_ref("refs/remotes/origin/main", base);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_named_ref_array refs{};
  ASSERT_EQ(gg_repository_named_refs(&refs, repository), GIT_OK);
  gg_named_ref_array_dispose(&refs);

  const git_oid head = ref("HEAD");
  git_reference* external = nullptr;
  ASSERT_EQ(git_reference_create(&external, repository_.get(),
                                 "refs/heads/external", &head, 0, nullptr),
            GIT_OK);
  git_reference_free(external);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  ASSERT_EQ(gg_repository_named_refs(&refs, repository), GIT_OK);
  bool found_external = false;
  for (size_t index = 0; index < refs.count; ++index) {
    if (std::strcmp(refs.items[index].name, "external") == 0 &&
        refs.items[index].kind == GG_NAMED_REF_LOCAL_BRANCH) {
      found_external = true;
    }
  }
  EXPECT_TRUE(found_external);
  gg_named_ref_array_dispose(&refs);

  const git_oid local = raw_commit("local", {base});
  const git_oid remote = raw_commit("remote", {local});
  set_ref("refs/heads/main", local);
  set_ref("refs/remotes/origin/main", remote);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  // main is checked out, so fetched history never moves it behind the
  // working tree's back.
  const git_oid kept = ref("refs/heads/main");
  EXPECT_NE(git_oid_equal(&kept, &local), 0);
  ASSERT_EQ(invoke_git({"checkout", "-q", "--detach"}).code, 0);
  set_ref("refs/remotes/origin/main", raw_commit("newer", {remote}));
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  const git_oid advanced = ref("refs/heads/main");
  const git_oid newer = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&advanced, &newer), 0);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, NamedRefsIgnoreTargetsThatAreNotCommits) {
  const git_oid head = ref("HEAD");
  git_commit* commit = nullptr;
  ASSERT_EQ(git_commit_lookup(&commit, repository_.get(), &head), GIT_OK);
  const git_oid tree = *git_commit_tree_id(commit);
  git_commit_free(commit);
  ASSERT_EQ(invoke_git({"tag", "-a", "tree-only", "-m", "tree-only",
                        git_oid_tostr_s(&tree)})
                .code,
            0);

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_named_ref_array refs{};
  ASSERT_EQ(gg_repository_named_refs(&refs, repository), GIT_OK);
  for (size_t index = 0; index < refs.count; ++index) {
    EXPECT_STRNE(refs.items[index].name, "tree-only");
  }
  gg_named_ref_array_dispose(&refs);

  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "work";
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  gg_repository_free(repository);
}

TEST_F(RepositoryTest, LookupRevisionsHydratesOnlyRequestedCommitsInOrder) {
  const git_oid base = ref("HEAD");
  write("tracked.txt", "second\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "second"}).code, 0);
  const git_oid tip = ref("HEAD");

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  git_oid requested[] = {tip, base};
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_lookup_revisions(
                &revisions, repository, {requested, 2}), GIT_OK);
  ASSERT_EQ(revisions.count, 2U);
  EXPECT_TRUE(git_oid_equal(&revisions.items[0].oid, &tip));
  EXPECT_TRUE(git_oid_equal(&revisions.items[1].oid, &base));
  EXPECT_STREQ(revisions.items[0].description, "second\n");
  ASSERT_EQ(revisions.items[0].parents.count, 1U);
  EXPECT_TRUE(git_oid_equal(&revisions.items[0].parents.ids[0], &base));
  gg_revision_array_dispose(&revisions);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, SnapshotPreservesSparseCheckoutEntries) {
  write("included/visible.txt", "visible\n");
  write("excluded/hidden.txt", "hidden\n");
  ASSERT_EQ(invoke_git({"add", "."}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "add sparse paths"}).code, 0);
  ASSERT_EQ(invoke_git({"sparse-checkout", "init", "--cone"}).code, 0);
  ASSERT_EQ(invoke_git({"sparse-checkout", "set", "included"}).code, 0);
  ASSERT_TRUE(std::filesystem::exists(path_ / "included/visible.txt"));
  ASSERT_FALSE(std::filesystem::exists(path_ / "excluded/hidden.txt"));
  git_index* raw_index = nullptr;
  ASSERT_EQ(git_repository_index(&raw_index, repository_.get()), GIT_OK);
  std::unique_ptr<git_index, decltype(&git_index_free)> index(raw_index,
                                                               git_index_free);
  ASSERT_EQ(git_index_read(index.get(), true), GIT_OK);
  const git_index_entry* hidden =
      git_index_get_bypath(index.get(), "excluded/hidden.txt", 0);
  ASSERT_NE(hidden, nullptr);
  ASSERT_NE(hidden->flags_extended & GIT_INDEX_ENTRY_SKIP_WORKTREE, 0);

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  start_change(repository);
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);

  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_status(&status, repository, &options), GIT_OK);
  ASSERT_EQ(status.entry_count, 0U)
      << (status.entry_count == 0 ? "" : status.entries[0].new_path);
  gg_status_dispose(&status);
  ASSERT_EQ(gg_repository_worktree_status(&status, repository, &options),
            GIT_OK);
  EXPECT_EQ(status.entry_count, 0U);
  gg_status_dispose(&status);

  write("included/visible.txt", "changed\n");
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  ASSERT_EQ(gg_repository_status(&status, repository, &options), GIT_OK);
  ASSERT_EQ(status.entry_count, 1U)
      << (status.entry_count == 0 ? "" : status.entries[0].new_path);
  EXPECT_STREQ(status.entries[0].new_path, "included/visible.txt");
  EXPECT_EQ(status.entries[0].status, GIT_DELTA_MODIFIED);
  gg_status_dispose(&status);

  std::filesystem::remove(path_ / "included/visible.txt");
  write("included/new.txt", "new\n");
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  ASSERT_EQ(gg_repository_status(&status, repository, &options), GIT_OK);
  ASSERT_EQ(status.entry_count, 2U);
  bool added = false;
  bool deleted = false;
  for (std::size_t entry = 0; entry < status.entry_count; ++entry) {
    const std::string path = status.entries[entry].new_path == nullptr
                                 ? ""
                                 : status.entries[entry].new_path;
    added |= path == "included/new.txt" &&
             status.entries[entry].status == GIT_DELTA_ADDED;
    deleted |= path == "included/visible.txt" &&
               status.entries[entry].status == GIT_DELTA_DELETED;
    EXPECT_NE(path, "excluded/hidden.txt");
  }
  EXPECT_TRUE(added);
  EXPECT_TRUE(deleted);
  gg_status_dispose(&status);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, SnapshotDirtyGitlinkDirectoryDoesNotRecurse) {
  write("gitlink/dirty.txt", "dirty\n");

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  start_change(repository);
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  gg_repository_free(repository);
}

struct LineEndingSnapshotCase {
  const char* name;
  const char* auto_crlf;
  const char* attributes;
  std::string_view worktree_base;
  bool base_changes;
  std::string_view stored_base;
  std::string_view worktree_edit;
  std::string_view stored_edit;
};

class LineEndingSnapshotTest
    : public RepositoryTest,
      public testing::WithParamInterface<LineEndingSnapshotCase> {};

TEST_P(LineEndingSnapshotTest, PathSnapshotUsesGitCleanLineEndingRules) {
  const LineEndingSnapshotCase& test = GetParam();
  git_config* config = nullptr;
  ASSERT_EQ(git_repository_config(&config, repository_.get()), GIT_OK);
  ASSERT_EQ(git_config_set_string(config, "core.autocrlf", test.auto_crlf),
            GIT_OK);
  git_config_free(config);
  if (test.attributes[0] != '\0') {
    write(".gitattributes", test.attributes);
    ASSERT_EQ(invoke_git({"add", ".gitattributes"}).code, 0);
    ASSERT_EQ(invoke_git({"commit", "-m", "add attributes"}).code, 0);
  }

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  start_change(repository);

  write("tracked.txt", test.worktree_base);
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  EXPECT_EQ(changed != 0, test.base_changes);
  const std::string base_revision = test.base_changes
                                        ? "refs/gg/workspaces/default"
                                        : "HEAD";
  Result stored = invoke_git({"show", base_revision + ":tracked.txt"});
  ASSERT_EQ(stored.code, 0) << stored.error;
  EXPECT_EQ(stored.output, test.stored_base);

  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_status(&status, repository, &options), GIT_OK);
  EXPECT_EQ(status.entry_count, test.base_changes ? 1U : 0U);
  gg_status_dispose(&status);

  write("tracked.txt", test.worktree_edit);
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  EXPECT_TRUE(changed);
  stored = invoke_git({"show", "refs/gg/workspaces/default:tracked.txt"});
  ASSERT_EQ(stored.code, 0) << stored.error;
  EXPECT_EQ(stored.output, test.stored_edit);
  gg_repository_free(repository);
}

INSTANTIATE_TEST_SUITE_P(
    LineEndings, LineEndingSnapshotTest,
    testing::Values(
        LineEndingSnapshotCase{"AutoCrlfFalse", "false", "", "base\r\n",
                               true, "base\r\n", "edited\r\n",
                               "edited\r\n"},
        LineEndingSnapshotCase{"AutoCrlfInput", "input", "", "base\r\n",
                               false, "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"AutoCrlfTrue", "true", "", "base\r\n",
                               false, "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"TextOverridesAutoCrlfFalse", "false",
                               "tracked.txt text\n", "base\r\n", false,
                               "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"AutoTextNormalizesText", "false",
                               "tracked.txt text=auto\n", "base\r\n", false,
                               "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{
            "AutoTextPreservesBinary", "true", "tracked.txt text=auto\n",
            std::string_view{"base\0\r\n", 7}, true,
            std::string_view{"base\0\r\n", 7},
            std::string_view{"edited\0\r\n", 9},
            std::string_view{"edited\0\r\n", 9}},
        LineEndingSnapshotCase{"EolLf", "false",
                               "tracked.txt text eol=lf\n", "base\r\n",
                               false, "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"EolCrlf", "false",
                               "tracked.txt text eol=crlf\n", "base\r\n",
                               false, "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"EolImpliesText", "false",
                               "tracked.txt eol=lf\n", "base\r\n", false,
                               "base\n", "edited\r\n", "edited\n"},
        LineEndingSnapshotCase{"NonTextOverridesAutoCrlfTrue", "true",
                               "tracked.txt -text\n", "base\r\n", true,
                               "base\r\n", "edited\r\n", "edited\r\n"},
        LineEndingSnapshotCase{"BinaryOverridesAutoCrlfInput", "input",
                               "tracked.txt binary\n", "base\r\n", true,
                               "base\r\n", "edited\r\n", "edited\r\n"},
        LineEndingSnapshotCase{"AutoCrlfTreatsBareCarriageReturnAsBinary",
                               "input", "", "base\r\n", false, "base\n",
                               "edited\rbare\r\n", "edited\rbare\r\n"},
        LineEndingSnapshotCase{"ForcedTextNormalizesOnlyCrlf", "false",
                               "tracked.txt text\n", "base\r\n", false,
                               "base\n", "edited\rbare\r\n",
                               "edited\rbare\n"}),
    [](const testing::TestParamInfo<LineEndingSnapshotCase>& info) {
      return info.param.name;
    });

TEST_F(RepositoryTest, PathSnapshotHonorsDisabledFileModeTracking) {
  git_config* config = nullptr;
  ASSERT_EQ(git_repository_config(&config, repository_.get()), GIT_OK);
  ASSERT_EQ(git_config_set_bool(config, "core.filemode", false), GIT_OK);
  git_config_free(config);
  repository_.reset();
  git_repository* reopened = nullptr;
  ASSERT_EQ(git_repository_open(&reopened, path_.string().c_str()), GIT_OK);
  repository_.reset(reopened);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);
  start_change(repository);

  std::filesystem::permissions(
      path_ / "tracked.txt", std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add);
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  EXPECT_FALSE(changed);

  gg_status_options options = GG_STATUS_OPTIONS_INIT;
  gg_status status{};
  ASSERT_EQ(gg_repository_status(&status, repository, &options), GIT_OK);
  EXPECT_EQ(status.entry_count, 0U);
  gg_status_dispose(&status);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, MovesFilesBetweenChangesAtomically) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "source";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  write("tracked.txt", "moved\n");
  int changed = 0;
  ASSERT_EQ(amend_worktree(&changed, repository),
            GIT_OK);
  ASSERT_TRUE(changed);
  git_oid source{};
  ASSERT_EQ(gg_repository_working_copy(&source, repository), GIT_OK);

  create.message = "destination";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid destination = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);

  const char* path[] = {"tracked.txt"};
  const std::string source_text = git_oid_tostr_s(&source);
  const std::string destination_text = git_oid_tostr_s(&destination);
  gg_move_files_options move = GG_MOVE_FILES_OPTIONS_INIT;
  move.source = source_text.c_str();
  move.destination = destination_text.c_str();
  move.filesets = {path, 1};
  ASSERT_EQ(gg_repository_move_files(&mutation, repository, &move, nullptr),
            GIT_OK)
      << git_error_last()->message;
  ASSERT_TRUE(mutation.changed);

  git_oid rewritten_source{};
  git_oid rewritten_destination{};
  bool found_source = false;
  bool found_destination = false;
  for (size_t index = 0; index < mutation.rewrite_count; ++index) {
    if (git_oid_equal(&mutation.rewrites[index].before, &source) != 0) {
      rewritten_source = mutation.rewrites[index].after;
      found_source = true;
    }
    if (git_oid_equal(&mutation.rewrites[index].before, &destination) != 0) {
      rewritten_destination = mutation.rewrites[index].after;
      found_destination = true;
    }
  }
  ASSERT_TRUE(found_source);
  ASSERT_TRUE(found_destination);

  const auto delta_count = [&](const git_oid& oid) {
    git_commit* commit = nullptr;
    EXPECT_EQ(git_commit_lookup(&commit, repository_.get(), &oid), GIT_OK);
    git_commit* parent = nullptr;
    EXPECT_EQ(git_commit_parent(&parent, commit, 0), GIT_OK);
    git_tree* tree = nullptr;
    git_tree* parent_tree = nullptr;
    EXPECT_EQ(git_commit_tree(&tree, commit), GIT_OK);
    EXPECT_EQ(git_commit_tree(&parent_tree, parent), GIT_OK);
    git_diff* diff = nullptr;
    EXPECT_EQ(git_diff_tree_to_tree(&diff, repository_.get(), parent_tree, tree,
                                    nullptr),
              GIT_OK);
    const size_t result = git_diff_num_deltas(diff);
    git_diff_free(diff);
    git_tree_free(parent_tree);
    git_tree_free(tree);
    git_commit_free(parent);
    git_commit_free(commit);
    return result;
  };
  EXPECT_EQ(delta_count(rewritten_source), 0U);
  EXPECT_EQ(delta_count(rewritten_destination), 1U);
  gg_mutation_result_dispose(&mutation);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, MovesFilesBetweenRemoteOnlyAndUnreferencedEndpointsAtomically) {
  ASSERT_EQ(invoke({"new", "-m", "workspace", main_id()}).code, 0);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  detail::Repository repo(path_);
  const git_oid base = ref("refs/heads/main");
  const git_oid workspace = *repo.workspace();
  const git_oid base_tree = *git_commit_tree_id(repo.commit(base).get());
  const auto make_revision = [&](const std::string& message, const std::string& contents) {
    git_oid blob{};
    EXPECT_EQ(git_blob_create_from_buffer(&blob, repository_.get(), contents.data(), contents.size()), GIT_OK);
    detail::TreePtr tree = repo.tree(base_tree);
    git_treebuilder* raw_builder = nullptr;
    EXPECT_EQ(git_treebuilder_new(&raw_builder, repository_.get(), tree.get()), GIT_OK);
    std::unique_ptr<git_treebuilder, decltype(&git_treebuilder_free)> builder(raw_builder, git_treebuilder_free);
    EXPECT_EQ(git_treebuilder_insert(nullptr, builder.get(), "tracked.txt", &blob, GIT_FILEMODE_BLOB), GIT_OK);
    git_oid tree_oid{};
    EXPECT_EQ(git_treebuilder_write(&tree_oid, builder.get()), GIT_OK);
    return repo.create_commit(tree_oid, {base}, message);
  };

  for (const bool remote : {true, false}) {
    SCOPED_TRACE(remote);
    const git_oid source = make_revision(remote ? "remote source" : "unreferenced source", "moved\n");
    const git_oid destination = make_revision(remote ? "remote destination" : "unreferenced destination", "base\n");
    if (remote) {
      set_ref("refs/remotes/origin/move-source", source);
      set_ref("refs/remotes/origin/move-destination", destination);
    }
    const auto refs_before = repo.rewrite_refs();
    EXPECT_TRUE(std::ranges::none_of(refs_before, [&](const auto& entry) {
      return git_oid_equal(&entry.second, &source) != 0 || git_oid_equal(&entry.second, &destination) != 0;
    }));
    const std::string source_text = detail::oid_string(source);
    const std::string destination_text = detail::oid_string(destination);
    const char* paths[] = {"tracked.txt"};
    gg_move_files_options move = GG_MOVE_FILES_OPTIONS_INIT;
    move.source = source_text.c_str();
    move.destination = destination_text.c_str();
    move.filesets = {paths, 1};
    gg_mutation_result mutation{};
    ASSERT_EQ(gg_repository_move_files(&mutation, repository, &move, nullptr), GIT_OK)
        << (git_error_last() == nullptr ? "" : git_error_last()->message);
    EXPECT_TRUE(mutation.changed);
    EXPECT_EQ(mutation.rewrite_count, 2U);
    EXPECT_NE(git_oid_equal(&mutation.working_copy, &workspace), 0);
    gg_mutation_result_dispose(&mutation);

    detail::Repository after(path_);
    const git_oid moved_source = after.resolve(source_text);
    const git_oid moved_destination = after.resolve(destination_text);
    EXPECT_EQ(git_oid_equal(&moved_source, &source), 0);
    EXPECT_EQ(git_oid_equal(&moved_destination, &destination), 0);
    EXPECT_EQ(invoke_git({"show", detail::oid_string(moved_source) + ":tracked.txt"}).output, "base\n");
    EXPECT_EQ(invoke_git({"show", detail::oid_string(moved_destination) + ":tracked.txt"}).output, "moved\n");
    const auto visible = after.resolve_set("all()");
    for (const git_oid& endpoint : {moved_source, moved_destination}) {
      EXPECT_TRUE(std::ranges::any_of(visible, [&](const git_oid& oid) {
        return git_oid_equal(&oid, &endpoint) != 0;
      }));
    }
    if (remote) {
      const git_oid source_ref = ref("refs/remotes/origin/move-source");
      const git_oid destination_ref = ref("refs/remotes/origin/move-destination");
      EXPECT_NE(git_oid_equal(&source_ref, &source), 0);
      EXPECT_NE(git_oid_equal(&destination_ref, &destination), 0);
    }
    const git_oid main = ref("refs/heads/main");
    EXPECT_NE(git_oid_equal(&main, &base), 0);
    ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
    gg_mutation_result_dispose(&mutation);
    detail::Repository undone(path_);
    const git_oid restored_source = undone.resolve(source_text);
    const git_oid restored_destination = undone.resolve(destination_text);
    EXPECT_NE(git_oid_equal(&restored_source, &source), 0);
    EXPECT_NE(git_oid_equal(&restored_destination, &destination), 0);
    expect_workspace_coherent();
  }
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ValidatesPartialMoveCarriersAndRestrictsThemToSelectedFiles) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "source", main_id()}).code, 0);
  write("tracked.txt", "moved\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid source = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "destination"}).code, 0);
  const git_oid destination = ref("refs/gg/workspaces/default");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  detail::Repository repo(path_);
  const auto carrier = [&](const git_oid& revision, const char* message) {
    const git_oid tree_oid = *git_commit_tree_id(repo.commit(revision).get());
    detail::TreePtr tree = repo.tree(tree_oid);
    git_treebuilder* raw_builder = nullptr;
    EXPECT_EQ(git_treebuilder_new(&raw_builder, repository_.get(), tree.get()), GIT_OK);
    std::unique_ptr<git_treebuilder, decltype(&git_treebuilder_free)> builder(raw_builder, git_treebuilder_free);
    git_oid blob{};
    EXPECT_EQ(git_blob_create_from_buffer(&blob, repository_.get(), "unselected\n", 11), GIT_OK);
    EXPECT_EQ(git_treebuilder_insert(nullptr, builder.get(), "unselected.txt", &blob, GIT_FILEMODE_BLOB), GIT_OK);
    git_oid result{};
    EXPECT_EQ(git_treebuilder_write(&result, builder.get()), GIT_OK);
    return detail::oid_string(repo.create_commit(result, {}, message));
  };
  const std::string selected = carrier(source, "selected carrier");
  const std::string remaining = carrier(base, "remaining carrier");
  const std::string source_text = detail::oid_string(source);
  std::string destination_text = detail::oid_string(destination);
  const char* paths[] = {"tracked.txt"};
  gg_move_files_options move = GG_MOVE_FILES_OPTIONS_INIT;
  move.source = source_text.c_str();
  move.destination = destination_text.c_str();
  move.filesets = {paths, 1};
  gg_operation_array operations{};
  ASSERT_EQ(gg_repository_operations(&operations, repository, 100), GIT_OK);
  const size_t operation_count = operations.count;
  gg_operation_array_dispose(&operations);
  gg_mutation_result mutation{};
  EXPECT_LT(gg_repository_move_files_ex(&mutation, repository, &move, selected.c_str(), nullptr, nullptr), 0);
  gg_mutation_result_dispose(&mutation);
  EXPECT_LT(gg_repository_move_files_ex(&mutation, repository, &move, nullptr, remaining.c_str(), nullptr), 0);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operations(&operations, repository, 100), GIT_OK);
  EXPECT_EQ(operations.count, operation_count);
  gg_operation_array_dispose(&operations);
  EXPECT_EQ(detail::oid_string(repo.resolve(source_text)), source_text);
  EXPECT_EQ(detail::oid_string(*repo.workspace()), destination_text);
  EXPECT_EQ(invoke_git({"show", destination_text + ":tracked.txt"}).output, "moved\n");

  for (const git_oid& target : {destination, base}) {
    destination_text = detail::oid_string(target);
    move.destination = destination_text.c_str();
    ASSERT_EQ(gg_repository_move_files_ex(&mutation, repository, &move, selected.c_str(), remaining.c_str(), nullptr), GIT_OK)
        << (git_error_last() == nullptr ? "" : git_error_last()->message);
    gg_mutation_result_dispose(&mutation);
    detail::Repository after(path_);
    for (const git_oid& endpoint : {after.resolve(source_text), after.resolve(destination_text)}) {
      EXPECT_NE(invoke_git({"cat-file", "-e", detail::oid_string(endpoint) + ":unselected.txt"}).code, 0);
    }
    EXPECT_FALSE(std::filesystem::exists(path_ / "unselected.txt"));
    ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
    gg_mutation_result_dispose(&mutation);
    expect_workspace_coherent();
  }
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ReordersAStackAsOneCOperation) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "first";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid first = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "second";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  create.message = "third";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid third = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);

  gg_operation_array before{};
  ASSERT_EQ(gg_repository_operations(&before, repository, 100), GIT_OK);
  const size_t before_count = before.count;
  gg_operation_array_dispose(&before);

  gg_reorder_options reorder = GG_REORDER_OPTIONS_INIT;
  const std::string third_text = git_oid_tostr_s(&third);
  const std::string first_text = git_oid_tostr_s(&first);
  reorder.source = third_text.c_str();
  reorder.target = first_text.c_str();
  reorder.placement = GG_REORDER_BEFORE;
  ASSERT_EQ(gg_repository_reorder(&mutation, repository, &reorder, nullptr),
            GIT_OK)
      << git_error_last()->message;
  EXPECT_TRUE(mutation.changed);
  EXPECT_TRUE(mutation.has_operation);
  EXPECT_GE(mutation.rewrite_count, 3U);
  gg_mutation_result_dispose(&mutation);

  gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
  query.revisions = "all()";
  query.reversed = 1;
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &query), GIT_OK);
  ASSERT_GE(revisions.count, 3U);
  EXPECT_STREQ(revisions.items[revisions.count - 3].description, "third");
  EXPECT_STREQ(revisions.items[revisions.count - 2].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 1].description, "second");
  gg_revision_array_dispose(&revisions);

  gg_operation_array after{};
  ASSERT_EQ(gg_repository_operations(&after, repository, 100), GIT_OK);
  EXPECT_EQ(after.count, before_count + 1);
  gg_operation_array_dispose(&after);
  ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);

  git_oid working{};
  ASSERT_EQ(gg_repository_working_copy(&working, repository), GIT_OK);
  EXPECT_TRUE(git_oid_equal(&working, &third));
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ReordersARootChangeUsingAfterPlacement) {
  const git_oid base = ref("HEAD");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "first";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid first = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "second";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  const std::string base_text = git_oid_tostr_s(&base);
  const std::string first_text = git_oid_tostr_s(&first);
  gg_reorder_options reorder = GG_REORDER_OPTIONS_INIT;
  reorder.source = base_text.c_str();
  reorder.target = first_text.c_str();
  reorder.placement = GG_REORDER_AFTER;
  ASSERT_EQ(gg_repository_reorder(&mutation, repository, &reorder, nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);

  gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
  query.revisions = "all()";
  query.reversed = 1;
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &query), GIT_OK);
  ASSERT_GE(revisions.count, 3U);
  EXPECT_STREQ(revisions.items[revisions.count - 3].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 2].description, "base");
  EXPECT_STREQ(revisions.items[revisions.count - 1].description, "second");
  gg_revision_array_dispose(&revisions);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, CopiesAChangeAtAReorderPosition) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "first";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid first = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "second";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid second = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "third";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  const std::string first_text = git_oid_tostr_s(&first);
  const std::string second_text = git_oid_tostr_s(&second);
  gg_reorder_options reorder = GG_REORDER_OPTIONS_INIT;
  reorder.source = first_text.c_str();
  reorder.target = second_text.c_str();
  reorder.placement = GG_REORDER_AFTER;
  reorder.copy = 1;
  ASSERT_EQ(gg_repository_reorder(&mutation, repository, &reorder, nullptr),
            GIT_OK)
      << git_error_last()->message;
  EXPECT_TRUE(mutation.changed);
  EXPECT_TRUE(mutation.has_operation);
  gg_mutation_result_dispose(&mutation);

  gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
  query.revisions = "ancestors(@)";
  query.reversed = 1;
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &query), GIT_OK);
  ASSERT_GE(revisions.count, 4U);
  EXPECT_STREQ(revisions.items[revisions.count - 4].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 3].description, "second");
  EXPECT_STREQ(revisions.items[revisions.count - 2].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 1].description, "third");
  gg_revision_array_dispose(&revisions);
  gg_repository_free(repository);
}

}  // namespace gg::test
