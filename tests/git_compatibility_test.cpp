// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"
#include "gg/gg.h"

namespace gg::test {

TEST_F(RepositoryTest, ImportsDirtyGitWorkingTreeAsInitialChange) {
  write("tracked.txt", "modified\n");
  write("untracked.txt", "new\n");

  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("M tracked.txt"), std::string::npos);
  EXPECT_NE(status.output.find("A untracked.txt"), std::string::npos);
  expect_workspace_coherent();
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "modified\n");
  EXPECT_EQ(read_path(path_ / "untracked.txt"), "new\n");
}

TEST_F(RepositoryTest, NewSnapshotsStagedAndUnstagedFilesForGit) {
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  write("unstaged.txt", "unstaged\n");
  write("staged.txt", "staged\n");
  ASSERT_EQ(invoke_git({"add", "staged.txt"}).code, 0);

  const Result created = invoke({"new", "-m", "second"});
  ASSERT_EQ(created.code, 0) << created.error;
  expect_workspace_coherent();
  EXPECT_TRUE(std::filesystem::exists(path_ / "unstaged.txt"));
  EXPECT_TRUE(std::filesystem::exists(path_ / "staged.txt"));
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output,
            "");
  EXPECT_EQ(invoke_git({"ls-files", "--", "staged.txt", "unstaged.txt"})
                .output,
            "staged.txt\nunstaged.txt\n");
}

TEST_F(RepositoryTest, SnapshotRepairsIndexOnlyDeletion) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("kept.txt", "kept\n");
  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  ASSERT_EQ(invoke_git({"rm", "--cached", "kept.txt"}).code, 0);
  ASSERT_TRUE(std::filesystem::exists(path_ / "kept.txt"));
  ASSERT_FALSE(invoke_git({"status", "--porcelain=v2",
                           "--untracked-files=all"})
                   .output.empty());

  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  expect_workspace_coherent();
  EXPECT_TRUE(std::filesystem::exists(path_ / "kept.txt"));
  EXPECT_EQ(invoke_git({"ls-files", "--", "kept.txt"}).output,
            "kept.txt\n");
}

TEST_F(RepositoryTest, NavigationUpdatesGitIndexToTheCheckedOutChange) {
  ASSERT_EQ(invoke({"new", "-m", "parent", "main"}).code, 0);
  write("tracked.txt", "parent\n");
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("tracked.txt", "child\n");
  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);

  ASSERT_EQ(invoke({"edit", "@-"}).code, 0);
  EXPECT_EQ(file(), "parent\n");
  expect_workspace_coherent();

  ASSERT_EQ(invoke({"next", "--edit"}).code, 0);
  EXPECT_EQ(file(), "child\n");
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, ExplicitlyUntrackedFilesSurviveCheckoutAndRecovery) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("keep.txt", "keep\n");
  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  ASSERT_EQ(invoke({"file", "untrack", "keep.txt"}).code, 0);

  ASSERT_EQ(invoke({"new", "-m", "next"}).code, 0);
  EXPECT_EQ(read_path(path_ / "keep.txt"), "keep\n");
  expect_workspace_coherent();
  EXPECT_EQ(invoke_git({"ls-files", "--", "keep.txt"}).output, "");
  EXPECT_NE(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output.find("? keep.txt"),
            std::string::npos);

  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(read_path(path_ / "keep.txt"), "keep\n");
  expect_workspace_coherent();
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_EQ(read_path(path_ / "keep.txt"), "keep\n");
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, ImportsNativeGitCommitsIntoACoherentWorkingChange) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("tracked.txt", "native\n");
  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "native commit"}).code, 0);

  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  expect_workspace_coherent();
  EXPECT_EQ(file(), "native\n");
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output,
            "");
}

TEST_F(RepositoryTest, ReconcilesGitPulledRemoteBookmarks) {
  const git_oid base = ref("refs/heads/main");
  set_ref("refs/remotes/origin/main", base);
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  const git_oid imported_tracking =
      ref("refs/gg/tracking/bookmarks/origin/main");
  EXPECT_NE(git_oid_equal(&imported_tracking, &base), 0);

  const git_oid pulled = raw_commit("pulled", {base});
  ASSERT_EQ(invoke_git({"update-ref", "refs/remotes/origin/main",
                        git_oid_tostr_s(&pulled)})
                .code,
            0);
  ASSERT_EQ(invoke_git({"reset", "--hard", git_oid_tostr_s(&pulled)}).code,
            0);
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid pulled_local = ref("refs/heads/main");
  const git_oid pulled_tracking =
      ref("refs/gg/tracking/bookmarks/origin/main");
  const git_oid pulled_parent =
      commit_parent(ref("refs/gg/workspaces/default"));
  EXPECT_NE(git_oid_equal(&pulled_local, &pulled), 0);
  EXPECT_NE(git_oid_equal(&pulled_tracking, &pulled), 0);
  EXPECT_NE(git_oid_equal(&pulled_parent, &pulled), 0);

  const git_oid local = raw_commit("local", {pulled});
  const git_oid remote = raw_commit("remote", {local});
  set_ref("refs/heads/main", local);
  set_ref("refs/remotes/origin/main", remote);
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid advanced_local = ref("refs/heads/main");
  const git_oid updated_tracking =
      ref("refs/gg/tracking/bookmarks/origin/main");
  EXPECT_NE(git_oid_equal(&advanced_local, &remote), 0);
  EXPECT_NE(git_oid_equal(&updated_tracking, &remote), 0);

  const git_oid local_side = raw_commit("local side", {remote});
  const git_oid remote_side = raw_commit("remote side", {remote});
  set_ref("refs/heads/main", local_side);
  set_ref("refs/remotes/origin/main", remote_side);
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid preserved_local = ref("refs/heads/main");
  EXPECT_NE(git_oid_equal(&preserved_local, &local_side), 0);
}

TEST_F(RepositoryTest, FailedCommandsLeaveGitProjectionUnchanged) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("tracked.txt", "changed\n");
  ASSERT_EQ(invoke({"log", "--no-graph"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  const git_oid head = ref("HEAD");
  const std::string status =
      invoke_git({"status", "--porcelain=v2", "--untracked-files=all"})
          .output;

  EXPECT_EQ(invoke({"edit", "missing"}).code, 2);
  const git_oid workspace_after = ref("refs/gg/workspaces/default");
  const git_oid head_after = ref("HEAD");
  EXPECT_NE(git_oid_equal(&workspace, &workspace_after), 0);
  EXPECT_NE(git_oid_equal(&head, &head_after), 0);
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output,
            status);
  expect_workspace_coherent();
  EXPECT_EQ(invoke_git({"fsck", "--full", "--no-dangling"}).code, 0);
}

TEST_F(RepositoryTest, SnapshotsRenamesDeletesModesSymlinksAndIgnoredFiles) {
  ASSERT_EQ(invoke({"new", "-m", "files", "main"}).code, 0);
  std::filesystem::rename(path_ / "tracked.txt", path_ / "renamed.txt");
  write("nested/tool.sh", "#!/bin/sh\n");
  std::filesystem::permissions(
      path_ / "nested/tool.sh", std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add);
  std::filesystem::create_symlink("tool.sh", path_ / "nested/link");
  write(".gitignore", "ignored.txt\n");
  write("ignored.txt", "ignored\n");

  ASSERT_EQ(invoke({"new", "-m", "next"}).code, 0);
  expect_workspace_coherent();
  EXPECT_FALSE(std::filesystem::exists(path_ / "tracked.txt"));
  EXPECT_EQ(read_path(path_ / "renamed.txt"), "base\n");
  EXPECT_TRUE(std::filesystem::is_symlink(path_ / "nested/link"));
  EXPECT_TRUE(std::filesystem::exists(path_ / "ignored.txt"));
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output,
            "");
  EXPECT_EQ(invoke_git({"ls-files", "--", "ignored.txt"}).output, "");
  EXPECT_NE(invoke_git({"status", "--porcelain=v2", "--ignored",
                        "--untracked-files=all"})
                .output.find("! ignored.txt"),
            std::string::npos);
}

TEST_F(RepositoryTest, CommitRestoreAndMetadataCommandsKeepGitCoherent) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  write("selected.txt", "selected\n");
  write("remaining.txt", "remaining\n");
  ASSERT_EQ(invoke({"commit", "-m", "selected", "selected.txt"}).code, 0);
  expect_workspace_coherent();
  EXPECT_NE(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output.find("remaining.txt"),
            std::string::npos);

  ASSERT_EQ(invoke({"describe", "-m", "remaining"}).code, 0);
  ASSERT_EQ(invoke({"metaedit", "-m", "metadata"}).code, 0);
  expect_workspace_coherent();
  ASSERT_EQ(invoke({"restore", "remaining.txt"}).code, 0);
  expect_workspace_coherent();
  EXPECT_EQ(invoke_git({"status", "--porcelain=v2",
                        "--untracked-files=all"})
                .output,
            "");
}

TEST_F(RepositoryTest, IsolatesGgStateAcrossLinkedWorkspaces) {
  const auto linked = path_.parent_path() /
                      (path_.filename().string() + "-linked");
  std::filesystem::remove_all(linked);
  ASSERT_EQ(invoke({"new", "-m", "primary", "main"}).code, 0);
  const git_oid primary_workspace = ref(detail::kWorkspaceRef);
  detail::Repository initial(path_);
  initial.record({}, {std::string(detail::kWorkspaceRef)},
                 initial.head_state(), "test current workspace delete");
  EXPECT_FALSE(has_ref(detail::kWorkspaceRef));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  ASSERT_TRUE(has_ref(detail::kWorkspaceRef));
  const std::string primary_before = file();
  const Result added = invoke({"workspace", "add", linked.string(), "--name",
                               "secondary", "-r", "@", "-m",
                               "secondary"});
  ASSERT_EQ(added.code, 0) << added.error;
  ASSERT_TRUE(has_ref("refs/gg/workspaces/secondary"));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/secondary"));
  ASSERT_EQ(invoke({"redo"}).code, 0);
  ASSERT_TRUE(has_ref("refs/gg/workspaces/secondary"));
  EXPECT_EQ(invoke({"workspace", "rename", "secondary"}).code, 2);
  EXPECT_NE(invoke({"workspace", "list"}).output.find(linked.string()),
            std::string::npos);
  const Result workspaces = invoke({"workspace", "list"});
  EXPECT_NE(workspaces.output.find("default: "), std::string::npos);
  EXPECT_NE(workspaces.output.find("secondary: "), std::string::npos);
  detail::Repository primary(path_);
  const git_oid secondary_workspace =
      ref("refs/gg/workspaces/secondary");
  primary.record({{"refs/gg/workspaces/secondary", secondary_workspace}}, {},
                 primary.head_state(), "test unchanged workspace");
  EXPECT_THROW(primary.record({{"refs/gg/workspaces/missing",
                                secondary_workspace}},
                              {}, primary.head_state(),
                              "test missing workspace update"),
               detail::UserError);
  primary.record({}, {"refs/gg/workspaces/missing"}, primary.head_state(),
                 "test missing workspace delete");
  EXPECT_THROW(primary.record({}, {"refs/gg/workspaces/secondary"},
                              primary.head_state(), "test protected delete"),
               detail::UserError);
  EXPECT_EQ(invoke_at(linked, {"workspace", "root", "--name", "secondary"})
                .output,
            std::filesystem::weakly_canonical(linked).string() + "\n");

  std::ofstream(linked / "linked.txt") << "linked\n";
  ASSERT_EQ(invoke_at(linked, {"new", "-m", "linked work"}).code, 0);
  EXPECT_EQ(invoke_git_at(linked, {"status", "--porcelain=v2",
                                   "--untracked-files=all"})
                .output,
            "");
  EXPECT_EQ(file(), primary_before);
  git_oid actual_primary = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&actual_primary, &primary_workspace), 0);

  const std::string primary_id = git_oid_tostr_s(&primary_workspace);
  const Result protected_rewrite =
      invoke_at(linked, {"describe", "-m", "forbidden", primary_id});
  EXPECT_EQ(protected_rewrite.code, 2);
  EXPECT_NE(protected_rewrite.error.find(
                "operation would rewrite active workspace: default"),
            std::string::npos);
  actual_primary = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&actual_primary, &primary_workspace), 0);

  const git_oid linked_first = ref("refs/gg/workspaces/secondary");
  ASSERT_EQ(invoke_at(linked, {"new", "-m", "another"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"undo"}).code, 0);
  const git_oid linked_after_undo = ref("refs/gg/workspaces/secondary");
  EXPECT_NE(git_oid_equal(&linked_after_undo, &linked_first), 0);
  actual_primary = ref(detail::kWorkspaceRef);
  EXPECT_NE(git_oid_equal(&actual_primary, &primary_workspace), 0);
  EXPECT_EQ(invoke({"undo"}).code, 2);

  const Result operation_refs =
      invoke_git({"for-each-ref", "--format=%(refname)",
                  "refs/gg/operations"});
  EXPECT_NE(operation_refs.output.find("refs/gg/operations/current"),
            std::string::npos);
  EXPECT_NE(operation_refs.output.find("refs/gg/operations/worktrees/"),
            std::string::npos);

  ASSERT_EQ(invoke({"workspace", "forget", "secondary"}).code, 0);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(has_ref("refs/gg/workspaces/secondary"));
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/secondary"));
  EXPECT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()})
                .code,
            0);
}

TEST_F(RepositoryTest, RegistersGitCreatedLinkedWorktreesOnFirstUse) {
  ASSERT_EQ(invoke({"new", "-m", "primary", "main"}).code, 0);
  const std::string primary_name = path_.filename().string() + "-native";
  ASSERT_EQ(invoke({"workspace", "rename", primary_name}).code, 0);
  const auto linked = path_.parent_path() / primary_name;
  const auto second_linked =
      path_.parent_path() / (primary_name + "-other");
  std::filesystem::remove_all(linked);
  std::filesystem::remove_all(second_linked);
  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "--detach",
                        linked.string(), "main"})
                .code,
            0);

  const Result log = invoke_at(linked, {"log", "-r", "@", "--no-graph"});
  ASSERT_EQ(log.code, 0) << log.error;
  const std::string name = primary_name + "-2";
  ASSERT_TRUE(has_ref(std::string(detail::kWorkspacePrefix) + name));
  const git_oid workspace =
      ref(std::string(detail::kWorkspacePrefix) + name);
  detail::Repository repo(linked);
  EXPECT_TRUE(repo.commit_aliases(workspace).empty());
  EXPECT_EQ(invoke_git_at(linked, {"status", "--porcelain=v2",
                                   "--untracked-files=all"})
                .output,
            "");

  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "--detach",
                        second_linked.string(), "main"})
                .code,
            0);
  ASSERT_EQ(invoke_at(second_linked, {"log", "-r", "@", "--no-graph"}).code,
            0);
  const std::string second_name = second_linked.filename().string();
  detail::Repository second(second_linked);
  const std::filesystem::path name_path =
      std::filesystem::path(git_repository_path(second.raw())) / "gg" /
      "workspace";

  second.set_workspace_name(name);
  EXPECT_EQ(invoke({"workspace", "list"}).code, 2);
  second.set_workspace_name(second_name);

  const std::filesystem::path temporary = name_path.string() + ".tmp";
  ASSERT_TRUE(std::filesystem::create_directory(temporary));
  EXPECT_THROW(second.set_workspace_name(second_name), detail::UserError);
  std::filesystem::remove(temporary);
  ASSERT_TRUE(std::filesystem::remove(name_path));
  ASSERT_TRUE(std::filesystem::create_directory(name_path));
  EXPECT_THROW(second.set_workspace_name(second_name), detail::UserError);
  std::filesystem::remove(name_path);
  second.set_workspace_name(second_name);

  std::ofstream(name_path, std::ios::trunc) << "bad..name\n";
  EXPECT_THROW(
      {
        detail::Repository invalid{second_linked};
        (void)invalid;
      },
      detail::UserError);
  std::ofstream(name_path, std::ios::trunc) << '\n';
  detail::Repository recovered(second_linked);
  EXPECT_EQ(recovered.workspace_name(), second_name);

  ASSERT_EQ(invoke({"workspace", "forget", second_name}).code, 0);
  std::ofstream(name_path, std::ios::trunc) << '\n';
  detail::Repository forgotten(second_linked);
  EXPECT_EQ(forgotten.workspace_name(), second_name);

  const std::string collision_name = primary_name + "-ref-collision";
  const auto collision_linked = path_.parent_path() / collision_name;
  std::filesystem::remove_all(collision_linked);
  set_ref(std::string(detail::kWorkspacePrefix) + collision_name, workspace);
  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "--detach",
                        collision_linked.string(), "main"})
                .code,
            0);
  detail::Repository collision(collision_linked);
  EXPECT_EQ(collision.workspace_name(), collision_name + "-2");
  ASSERT_EQ(invoke({"workspace", "forget", collision_name}).code, 0);
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force",
                        collision_linked.string()})
                .code,
            0);

  ASSERT_EQ(invoke({"workspace", "forget", name}).code, 0);
  EXPECT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()})
                .code,
            0);
  EXPECT_EQ(
      invoke_git({"worktree", "remove", "--force", second_linked.string()})
          .code,
      0);
}

TEST_F(RepositoryTest, LockedHeadRejectsWorkspaceChangesWithoutRecordingThem) {
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  const git_oid original = ref("HEAD");
  write(".git/HEAD.lock", "held by another process\n");
  for (const auto& command : {std::vector<std::string>{"new", "-m", "new child"},
                              std::vector<std::string>{"edit", detail::oid_string(original)}}) {
    SCOPED_TRACE(command.front());
    const Result failed = invoke(command);
    EXPECT_NE(failed.code, 0);
    EXPECT_NE(failed.error.find("HEAD.lock"), std::string::npos);
    EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
    EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
    const git_oid head = ref("HEAD");
    EXPECT_NE(git_oid_equal(&head, &original), 0);
    detail::Repository after(path_);
    EXPECT_TRUE(after.head_state().symbolic);
    EXPECT_EQ(after.head_state().value, "refs/heads/main");
    const auto actual_operation = after.operation();
    ASSERT_TRUE(actual_operation.has_value());
    EXPECT_NE(git_oid_equal(&*actual_operation, &operation), 0);
    EXPECT_EQ(file(), "base\n");
  }
}

TEST_F(RepositoryTest, FailedCheckoutRestoresGraphAndPartiallyWrittenFiles) {
  if (geteuid() == 0) GTEST_SKIP() << "requires filesystem permission enforcement";
  ASSERT_EQ(invoke({"new", "-m", "source"}).code, 0);
  write("aaa.txt", "old first\n");
  write("restricted/file.txt", "old blocked\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid source = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "target"}).code, 0);
  write("aaa.txt", "new first\n");
  write("restricted/file.txt", "new blocked\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid target = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"edit", detail::oid_string(source)}).code, 0);
  write(".git/info/exclude", "ignored.txt\n");
  write("ignored.txt", "keep ignored content\n");
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  const detail::HeadState head = before.head_state();

  struct RestorePermissions {
    std::filesystem::path root;
    ~RestorePermissions() {
      std::error_code error;
      std::filesystem::permissions(root / "restricted", std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
      std::filesystem::permissions(root / "restricted/file.txt", std::filesystem::perms::owner_write,
                                   std::filesystem::perm_options::add, error);
    }
  } restore_permissions{path_};
  std::filesystem::permissions(path_ / "restricted", std::filesystem::perms::owner_read |
      std::filesystem::perms::owner_exec);
  std::filesystem::permissions(path_ / "restricted/file.txt", std::filesystem::perms::owner_read);

  for (const auto& command : {std::vector<std::string>{"edit", detail::oid_string(target)},
                              std::vector<std::string>{"undo"}}) {
    SCOPED_TRACE(command.front());
    const Result failed = invoke(command);
    EXPECT_NE(failed.code, 0);
    EXPECT_NE(failed.error.find("Permission denied"), std::string::npos);
    EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
    detail::Repository after(path_);
    const auto after_operation = after.operation();
    ASSERT_TRUE(after_operation.has_value());
    EXPECT_NE(git_oid_equal(&*after_operation, &operation), 0);
    const auto after_workspace = after.workspace();
    ASSERT_TRUE(after_workspace.has_value());
    EXPECT_NE(git_oid_equal(&*after_workspace, &source), 0);
    EXPECT_EQ(after.head_state().symbolic, head.symbolic);
    EXPECT_EQ(after.head_state().value, head.value);
    EXPECT_EQ(read_path(path_ / "aaa.txt"), "old first\n");
    EXPECT_EQ(read_path(path_ / "restricted/file.txt"), "old blocked\n");
    EXPECT_EQ(read_path(path_ / "ignored.txt"), "keep ignored content\n");
  }
}

TEST_F(RepositoryTest, FailedCheckoutPreservesIgnoredCollisionWithoutCreatingWorkspace) {
  const git_oid original = ref("HEAD");
  write("ignored.txt", "committed target\n");
  ASSERT_EQ(invoke_git({"add", "ignored.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(original)}).code, 0);
  write(".git/info/exclude", "ignored.txt\n");
  write("ignored.txt", "precious local content\n");
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();

  const Result failed = invoke({"edit", detail::oid_string(target)});
  EXPECT_NE(failed.code, 0);
  EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
  EXPECT_EQ(read_path(path_ / "ignored.txt"), "precious local content\n");
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  const git_oid head = ref("HEAD");
  EXPECT_NE(git_oid_equal(&head, &original), 0);
  detail::Repository after(path_);
  const auto after_operation = after.operation();
  ASSERT_TRUE(after_operation.has_value());
  EXPECT_NE(git_oid_equal(&*after_operation, &operation), 0);
}

TEST_F(RepositoryTest, FailedCheckoutRestoresAnUnbornWorkingTree) {
  if (geteuid() == 0) GTEST_SKIP() << "requires filesystem permission enforcement";
  write("aaa.txt", "new early file\n");
  write("restricted/file.txt", "new blocked file\n");
  ASSERT_EQ(invoke_git({"add", "aaa.txt", "restricted/file.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"branch", "target"}).code, 0);
  ASSERT_EQ(invoke_git({"update-ref", "-d", "refs/heads/main"}).code, 0);
  ASSERT_EQ(invoke_git({"read-tree", "--empty"}).code, 0);
  std::filesystem::remove(path_ / "tracked.txt");
  std::filesystem::remove(path_ / "aaa.txt");
  std::filesystem::remove(path_ / "restricted/file.txt");
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  ASSERT_FALSE(before.head_oid().has_value());
  ASSERT_FALSE(before.workspace().has_value());
  struct RestorePermissions {
    std::filesystem::path directory;
    ~RestorePermissions() {
      std::error_code error;
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                   std::filesystem::perm_options::add, error);
    }
  } restore_permissions{path_ / "restricted"};
  std::filesystem::permissions(path_ / "restricted", std::filesystem::perms::owner_read |
      std::filesystem::perms::owner_exec);

  const Result failed = invoke({"edit", detail::oid_string(target)});
  EXPECT_NE(failed.code, 0);
  EXPECT_NE(failed.error.find("Permission denied"), std::string::npos);
  EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
  EXPECT_FALSE(std::filesystem::exists(path_ / "aaa.txt"));
  EXPECT_FALSE(std::filesystem::exists(path_ / "tracked.txt"));
  EXPECT_FALSE(std::filesystem::exists(path_ / "restricted/file.txt"));
  detail::Repository after(path_);
  EXPECT_FALSE(after.head_oid().has_value());
  EXPECT_FALSE(after.workspace().has_value());
  EXPECT_EQ(after.head_state().value, "refs/heads/main");
  const auto actual_operation = after.operation();
  ASSERT_TRUE(actual_operation.has_value());
  EXPECT_NE(git_oid_equal(&*actual_operation, &operation), 0);
  EXPECT_EQ(invoke_git({"ls-files"}).output, "");
}

TEST_F(RepositoryTest, LinkedWorktreeImportPreservesHeadAndFirstUndoLineage) {
  const git_oid base = ref("HEAD");
  const auto linked = path_.parent_path() / (path_.filename().string() + "-attached");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{linked};
  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "-b", "linked", linked.string(), "main"}).code, 0);
  const Result imported = invoke_at(linked, {"status"});
  ASSERT_EQ(imported.code, 0) << imported.error;
  detail::Repository repo(linked);
  const auto workspace = repo.workspace();
  ASSERT_TRUE(workspace.has_value());
  const git_oid operation = *repo.operation();
  const auto recorded = repo.parse_operation(operation);
  EXPECT_EQ(recorded.head.symbolic, repo.head_state().symbolic);
  EXPECT_EQ(recorded.head.value, repo.head_state().value);
  EXPECT_FALSE(repo.head_state().symbolic);
  EXPECT_EQ(detail::oid_string(*repo.head_oid()), detail::oid_string(base));
  ASSERT_EQ(invoke_at(linked, {"status"}).code, 0);
  EXPECT_EQ(detail::oid_string(*repo.operation()), detail::oid_string(operation));
  const Result undone = invoke_at(linked, {"undo"});
  ASSERT_EQ(undone.code, 0) << undone.error;
  EXPECT_FALSE(repo.workspace().has_value());
  EXPECT_TRUE(repo.head_state().symbolic);
  EXPECT_EQ(repo.head_state().value, "refs/heads/linked");
  ASSERT_EQ(invoke_at(linked, {"redo"}).code, 0);
  EXPECT_EQ(detail::oid_string(*repo.workspace()), detail::oid_string(*workspace));
  EXPECT_FALSE(repo.head_state().symbolic);
  EXPECT_EQ(read_path(linked / "tracked.txt"), "base\n");
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()}).code, 0);
}

TEST_F(RepositoryTest, LinkedWorktreeImportWithLockedHeadPreservesDirtyFiles) {
  const auto linked = path_.parent_path() / (path_.filename().string() + "-locked-import");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{linked};
  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "-b", "linked", linked.string(), "main"}).code, 0);
  detail::Repository repo(linked);
  const git_oid operation = repo.ensure_operation();
  const auto head = repo.head_state();
  const auto lock = std::filesystem::path(git_repository_path(repo.raw())) / "HEAD.lock";
  std::ofstream(lock) << "locked\n";
  std::ofstream(linked / "tracked.txt") << "precious dirty content\n";
  const Result refused = invoke_at(linked, {"status"});
  EXPECT_NE(refused.code, 0);
  EXPECT_EQ(refused.error.find("could not restore"), std::string::npos) << refused.error;
  EXPECT_FALSE(repo.workspace().has_value());
  EXPECT_EQ(repo.head_state().symbolic, head.symbolic);
  EXPECT_EQ(repo.head_state().value, head.value);
  EXPECT_EQ(detail::oid_string(*repo.operation()), detail::oid_string(operation));
  EXPECT_EQ(read_path(linked / "tracked.txt"), "precious dirty content\n");
  std::filesystem::remove(lock);
  ASSERT_EQ(invoke_at(linked, {"status"}).code, 0);
  EXPECT_TRUE(repo.workspace().has_value());
  EXPECT_FALSE(repo.head_state().symbolic);
  EXPECT_EQ(read_path(linked / "tracked.txt"), "precious dirty content\n");
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()}).code, 0);
}

TEST_F(RepositoryTest, LinkedWorktreeRefreshKeepsUndoRedoAndForgottenWorkspaceState) {
  const auto linked = path_.parent_path() / (path_.filename().string() + "-refresh");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
  } cleanup{linked};
  ASSERT_EQ(invoke_git({"worktree", "add", "--quiet", "-b", "linked", linked.string(), "main"}).code, 0);
  detail::Repository repo(linked);
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repo.raw()), GIT_OK);
  // The GUI runs both calls on the same attached instance for every refresh.
  const auto refresh = [&] {
    EXPECT_EQ(gg_repository_adopt_git_history_ex(api, 0, nullptr), GIT_OK);
    int changed = 0;
    EXPECT_EQ(gg_repository_snapshot_working_copy(&changed, api, nullptr), GIT_OK);
  };
  refresh();
  ASSERT_TRUE(repo.workspace().has_value());
  const git_oid imported = *repo.workspace();
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_undo(&mutation, api, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  ASSERT_FALSE(repo.workspace().has_value());
  const git_oid undone_operation = *repo.operation();
  refresh();
  refresh();
  EXPECT_FALSE(repo.workspace().has_value());
  EXPECT_EQ(detail::oid_string(*repo.operation()), detail::oid_string(undone_operation));
  gg_operation_capabilities capabilities{};
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, api), GIT_OK);
  EXPECT_TRUE(capabilities.can_redo);
  ASSERT_EQ(gg_repository_redo(&mutation, api, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  refresh();
  EXPECT_EQ(detail::oid_string(*repo.workspace()), detail::oid_string(imported));
  ASSERT_EQ(invoke_at(linked, {"workspace", "forget", repo.workspace_name()}).code, 0);
  const git_oid forgotten_operation = *repo.operation();
  refresh();
  EXPECT_FALSE(repo.workspace().has_value());
  EXPECT_EQ(detail::oid_string(*repo.operation()), detail::oid_string(forgotten_operation));
  gg_repository_free(api);
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()}).code, 0);
}

TEST_F(RepositoryTest, DetachedHeadMetadataRewriteSurvivesGarbageCollectionAndUndo) {
  const git_oid original = raw_commit("detached source", {ref("HEAD")});
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(original)}).code, 0);
  ASSERT_FALSE(has_ref("refs/gg/workspaces/default"));
  const Result described = invoke({"describe", "-m", "rewritten source",
                                   detail::oid_string(original)});
  ASSERT_EQ(described.code, 0) << described.error;
  const git_oid rewritten = ref("HEAD");
  ASSERT_FALSE(git_oid_equal(&original, &rewritten));
  ASSERT_FALSE(has_ref("refs/gg/workspaces/default"));

  // The original commit has no branch or workspace ref, and aliases retain only
  // their replacement. Operation history must keep this detached HEAD alive.
  ASSERT_EQ(invoke_git({"reflog", "expire", "--expire=now", "--all"}).code, 0);
  const Result collected = invoke_git({"gc", "--prune=now"});
  ASSERT_EQ(collected.code, 0) << collected.error;
  const Result retained = invoke_git({"cat-file", "-e",
                                      detail::oid_string(original) + "^{commit}"});
  ASSERT_EQ(retained.code, 0) << retained.error;
  const Result undone = invoke({"undo"});
  ASSERT_EQ(undone.code, 0) << undone.error;
  const git_oid restored = ref("HEAD");
  EXPECT_TRUE(git_oid_equal(&restored, &original));
  EXPECT_FALSE(detail::Repository(path_).head_state().symbolic);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "base\n");
  const Result redone = invoke({"redo"});
  ASSERT_EQ(redone.code, 0) << redone.error;
  const git_oid replayed = ref("HEAD");
  EXPECT_TRUE(git_oid_equal(&replayed, &rewritten));
}

}  // namespace gg::test
