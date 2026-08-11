// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"

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

}  // namespace gg::test
