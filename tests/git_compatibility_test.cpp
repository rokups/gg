// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

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

TEST_F(RepositoryTest, RejectsLinkedWorktreesWithoutTouchingEitherCheckout) {
  const auto linked = path_.parent_path() /
                      (path_.filename().string() + "-linked");
  std::filesystem::remove_all(linked);
  ASSERT_EQ(invoke_git({"worktree", "add", "-q", "-b", "linked",
                        linked.string(), "main"})
                .code,
            0);
  const std::string primary_before = file();
  const std::string linked_before = read_path(linked / "tracked.txt");

  const Result result = run({"-R", linked.string(), "log"});
  EXPECT_EQ(result.code, 2);
  EXPECT_NE(result.error.find("linked Git worktrees are not supported yet"),
            std::string::npos);
  EXPECT_EQ(file(), primary_before);
  EXPECT_EQ(read_path(linked / "tracked.txt"), linked_before);
  EXPECT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()})
                .code,
            0);
}

}  // namespace gg::test
