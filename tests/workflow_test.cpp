// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, StartsFromGitHeadWithoutAnExplicitParent) {
  const Result created = invoke({"new"});
  ASSERT_EQ(created.code, 0) << created.error;
  EXPECT_NE(created.output.find("(no description set)"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("Parent commit"), std::string::npos);
}

TEST_F(RepositoryTest, ReportsRenamedFiles) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  std::filesystem::rename(path_ / "tracked.txt", path_ / "renamed.txt");
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("R renamed.txt"), std::string::npos) << status.output;
}

TEST_F(RepositoryTest, CreatesSnapshotsAndLogsChanges) {
  const Result created = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(created.code, 0) << created.error;
  write("tracked.txt", "first\n");
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("M tracked.txt"), std::string::npos);
  const Result next = invoke({"new", "-m", "second"});
  ASSERT_EQ(next.code, 0) << next.error;
  const Result log = invoke({"log"});
  ASSERT_EQ(log.code, 0) << log.error;
  EXPECT_NE(log.output.find("first"), std::string::npos);
  EXPECT_NE(log.output.find("second"), std::string::npos);
}

TEST_F(RepositoryTest, ExplicitlySnapshotsTheWorkingCopy) {
  EXPECT_EQ(invoke({"util", "snapshot"}).output, "Nothing changed.\n");
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "snapshot\n");
  EXPECT_EQ(invoke({"util", "snapshot"}).output,
            "Created working-copy snapshot.\n");
  EXPECT_EQ(invoke({"util", "snapshot"}).output, "Nothing changed.\n");
  EXPECT_NE(invoke({"status"}).output.find("M tracked.txt"), std::string::npos);
}

TEST_F(RepositoryTest, EditsDescriptionsAndRestacksDescendantsAndBookmarks) {
  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  write("tracked.txt", "first\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid first_before = ref("refs/gg/workspaces/default");

  const Result second = invoke({"new", "-m", "second"});
  ASSERT_EQ(second.code, 0) << second.error;
  const std::string second_id = token_after(second.output, "Working copy now at: ");
  write("second.txt", "second\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "feature"}).code, 0);
  const git_oid second_before = ref("refs/heads/feature");

  ASSERT_EQ(invoke({"edit", "-r", first_id}).code, 0);
  ASSERT_EQ(invoke({"describe", "-m", "first rewritten"}).code, 0);
  write("tracked.txt", "first rewritten\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const git_oid first_after = ref("refs/gg/workspaces/default");
  EXPECT_FALSE(git_oid_equal(&first_before, &first_after) != 0);
  const Result first_log = invoke({"log", "-r", first_id});
  EXPECT_NE(first_log.output.find("first rewritten"), std::string::npos);
  const Result second_log = invoke({"log", "-r", second_id});
  EXPECT_NE(second_log.output.find("second"), std::string::npos);
  const git_oid second_after = ref("refs/heads/feature");
  EXPECT_FALSE(git_oid_equal(&second_before, &second_after) != 0);
}

TEST_F(RepositoryTest, SplitsAndSquashesPathChanges) {
  const Result created = invoke({"new", "-m", "both", "main"});
  ASSERT_EQ(created.code, 0) << created.error;
  write("first.txt", "first\n");
  write("second.txt", "second\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result split = invoke({"split", "-m", "selected", "first.txt"});
  ASSERT_EQ(split.code, 0) << split.error;
  EXPECT_NE(split.output.find("Selected change"), std::string::npos);
  const Result status = invoke({"status"});
  EXPECT_NE(status.output.find("A second.txt"), std::string::npos) << status.output;
  EXPECT_EQ(status.output.find("A first.txt"), std::string::npos) << status.output;

  const Result squash = invoke({"squash"});
  ASSERT_EQ(squash.code, 0) << squash.error;
  EXPECT_NE(squash.output.find("Squashed into"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("no changes"), std::string::npos);
}

TEST_F(RepositoryTest, AbandonsChangesAndUndoRestoresOperations) {
  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  write("first.txt", "first\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result second = invoke({"new", "-m", "second"});
  ASSERT_EQ(second.code, 0) << second.error;
  write("second.txt", "second\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  const git_oid topic_before = ref("refs/heads/topic");

  const Result abandoned = invoke({"abandon", first_id});
  ASSERT_EQ(abandoned.code, 0) << abandoned.error;
  const git_oid topic_after = ref("refs/heads/topic");
  EXPECT_FALSE(git_oid_equal(&topic_before, &topic_after) != 0);
  EXPECT_EQ(invoke({"log"}).output.find("first"), std::string::npos);

  ASSERT_EQ(invoke({"bookmark", "delete", "topic"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/topic"));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(has_ref("refs/heads/topic"));
}

TEST_F(RepositoryTest, SupportsEditorStyleUndoAndRedo) {
  constexpr std::string_view workspace = "refs/gg/workspaces/default";
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  const git_oid first = ref(workspace);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref(workspace);

  ASSERT_EQ(invoke({"undo"}).code, 0);
  git_oid actual = ref(workspace);
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_FALSE(has_ref(workspace));
  ASSERT_EQ(invoke({"redo"}).code, 0);
  actual = ref(workspace);
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  actual = ref(workspace);
  EXPECT_NE(git_oid_equal(&actual, &second), 0);

  ASSERT_EQ(invoke({"undo"}).code, 0);
  actual = ref(workspace);
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  actual = ref(workspace);
  EXPECT_NE(git_oid_equal(&actual, &second), 0);

  ASSERT_EQ(invoke({"undo"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "after-undo"}).code, 0);
  EXPECT_EQ(invoke({"redo"}).code, 2);
}

TEST_F(RepositoryTest, ShowsTheOperationLogAndAlias) {
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  ASSERT_EQ(invoke({"redo"}).code, 0);

  const Result log = invoke({"operation", "log"});
  ASSERT_EQ(log.code, 0) << log.error;
  EXPECT_EQ(log.output.front(), '@');
  EXPECT_NE(log.output.find("redo: restore to operation "), std::string::npos);
  EXPECT_NE(log.output.find("undo: restore to operation "), std::string::npos);
  EXPECT_NE(log.output.find("gg new"), std::string::npos);
  EXPECT_NE(log.output.find("initialize repository"), std::string::npos);
  EXPECT_NE(log.output.find("T"), std::string::npos);
  EXPECT_NE(log.output.find("○ "), std::string::npos);
  EXPECT_NE(log.output.find("│\n"), std::string::npos);
  EXPECT_EQ(invoke({"op", "log"}).output, log.output);
}

TEST_F(RepositoryTest, RestoresAllOrSelectedOperationState) {
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  const git_oid first = ref("refs/gg/workspaces/default");
  set_ref("refs/remotes/origin/main", first);
  ASSERT_EQ(invoke({"bookmark", "create", "keep"}).code, 0);
  const git_oid target_operation = ref("refs/gg/operations/current");
  const std::string target =
      std::string(git_oid_tostr_s(&target_operation)).substr(0, 8);

  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  set_ref("refs/remotes/origin/main", second);
  ASSERT_EQ(invoke({"bookmark", "delete", "keep"}).code, 0);

  Result restored = invoke({"operation", "restore", "--what", "repo", target});
  ASSERT_EQ(restored.code, 0) << restored.error;
  EXPECT_NE(restored.output.find(target), std::string::npos);
  EXPECT_TRUE(has_ref("refs/heads/keep"));
  git_oid actual = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &second), 0);

  restored = invoke(
      {"op", "restore", "--what", "remote-tracking", target});
  ASSERT_EQ(restored.code, 0) << restored.error;
  actual = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);

  ASSERT_EQ(invoke({"new", "-m", "third"}).code, 0);
  set_ref("refs/remotes/origin/main", second);
  ASSERT_EQ(invoke({"operation", "restore", target}).code, 0);
  actual = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  EXPECT_EQ(invoke({"operation", "restore", "--what", "repo", "--what",
                    "remote-tracking", "@"})
                .code,
            0);
  EXPECT_EQ(invoke({"operation", "restore", "@-"}).code, 0);
  EXPECT_EQ(invoke({"operation", "restore", "missing"}).code, 2);
  EXPECT_EQ(invoke({"operation", "restore", "@x"}).code, 2);
  EXPECT_EQ(invoke({"operation", "restore", "@-x"}).code, 2);
}

TEST_F(RepositoryTest, SupportsRootAndMergeWorkingCopyChanges) {
  EXPECT_NE(invoke({"status"}).output.find("No working-copy"), std::string::npos);
  ASSERT_EQ(git_reference_remove(repository_.get(), "refs/heads/main"), 0);
  std::filesystem::remove(path_ / "tracked.txt");
  const Result root = invoke({"new", "-m", "root"});
  ASSERT_EQ(root.code, 0) << root.error;
  EXPECT_NE(invoke({"status"}).output.find("Root working-copy"),
            std::string::npos);
  EXPECT_EQ(invoke({"log", "-r", "@-"}).code, 2);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  const std::string child = current_id();

  ASSERT_EQ(invoke({"edit", root.output.substr(root.output.find(": ") + 2, 8)}).code,
            0);
  write("left.txt", "left\n");
  const Result left_status = invoke({"status"});
  ASSERT_EQ(left_status.code, 0);
  EXPECT_NE(left_status.output.find("A left.txt"), std::string::npos);
  const std::string left = current_id();
  ASSERT_EQ(invoke({"new", "-m", "merge", left, child}).code, 0);
  git_commit* merge = nullptr;
  const git_oid workspace = ref("refs/gg/workspaces/default");
  ASSERT_EQ(git_commit_lookup(&merge, repository_.get(), &workspace), 0);
  EXPECT_EQ(git_commit_parentcount(merge), 2U);
  git_commit_free(merge);
}

TEST_F(RepositoryTest, MergesUnrelatedParents) {
  const git_oid first = raw_commit("first root");
  const git_oid second = raw_commit("second root");
  set_ref("refs/heads/first", first);
  set_ref("refs/heads/second", second);
  const Result merge = invoke({"new", "first", "second"});
  ASSERT_EQ(merge.code, 0) << merge.error;
  EXPECT_EQ(invoke({"log", "-r", "@"}).code, 0);
}

TEST_F(RepositoryTest, ImportsRawGitHeadChangesAndResolvesObjectIds) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid base = ref("refs/heads/main");
  const git_oid external = raw_commit("external", {base});
  set_ref("refs/heads/main", external);
  ASSERT_EQ(git_repository_set_head(repository_.get(), "refs/heads/main"), 0);
  const Result imported = invoke({"status"});
  ASSERT_EQ(imported.code, 0) << imported.error;
  EXPECT_NE(imported.output.find("Working copy (@)"), std::string::npos);

  const Result edit = invoke({"edit", std::string(git_oid_tostr_s(&base))});
  EXPECT_EQ(edit.code, 0) << edit.error;
}

}  // namespace gg::test
