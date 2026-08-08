// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace gg::test {

TEST_F(RepositoryTest, StartsFromGitHeadWithoutAnExplicitParent) {
  const Result created = invoke({"new"});
  ASSERT_EQ(created.code, 0) << created.error;
  EXPECT_NE(created.output.find("(no description set)"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("Parent commit"), std::string::npos);
}

TEST_F(RepositoryTest, CreatesChangesWithoutEditingThem) {
  const Result detached = invoke({"new", "--no-edit", "-m", "detached", "main"});
  ASSERT_EQ(detached.code, 0) << detached.error;
  EXPECT_NE(detached.output.find("Created change: "), std::string::npos);
  EXPECT_EQ(invoke({"workspace", "list"}).output, "No workspaces.\n");

  ASSERT_EQ(invoke({"new", "-m", "current", "main"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "--no-edit", "-m", "side"}).code, 0);
  const git_oid unchanged = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&workspace, &unchanged), 0);

  ASSERT_EQ(
      invoke({"new", "--no-edit", "-m", "inserted", "--before", "@"})
          .code,
      0);
  const git_oid rewritten = ref("refs/gg/workspaces/default");
  EXPECT_EQ(git_oid_equal(&workspace, &rewritten), 0);
  const git_oid inserted = commit_parent(rewritten);
  EXPECT_NE(invoke({"show", git_oid_tostr_s(&inserted), "--no-patch"})
                .output.find("Description: inserted"),
            std::string::npos);
}

TEST_F(RepositoryTest, InsertsNewChangesBeforeAndAfterRevisions) {
  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "child"}).code, 0);

  const Result after =
      invoke({"new", "-m", "after", "--after", first_id});
  ASSERT_EQ(after.code, 0) << after.error;
  const git_oid after_oid = ref("refs/gg/workspaces/default");
  const git_oid rewritten_child = ref("refs/heads/child");
  const git_oid child_parent = commit_parent(rewritten_child);
  EXPECT_NE(git_oid_equal(&child_parent, &after_oid), 0);

  const Result before =
      invoke({"new", "-m", "before", "--before", "child", "--no-edit"});
  ASSERT_EQ(before.code, 0) << before.error;
  const git_oid newest_child = ref("refs/heads/child");
  const git_oid before_oid = commit_parent(newest_child);
  EXPECT_NE(invoke({"show", git_oid_tostr_s(&before_oid), "--no-patch"})
                .output.find("Description: before"),
            std::string::npos);
  const git_oid current = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&current, &after_oid), 0);

  EXPECT_EQ(invoke({"new", "main", "--insert-after", "main"}).code, 2);
  EXPECT_EQ(invoke({"new", "--insert-after", "main", "--insert-before",
                    "main"})
                .code,
            2);
}

TEST_F(RepositoryTest, AssignsImportedIdsThroughAnInitialInsertion) {
  const Result inserted =
      invoke({"new", "-m", "inserted", "--insert-before", "main"});
  ASSERT_EQ(inserted.code, 0) << inserted.error;
  detail::Repository repo(path_);
  const git_oid rewritten_main = ref("refs/heads/main");
  EXPECT_TRUE(repo.change_id(rewritten_main).has_value());
}

TEST_F(RepositoryTest, ListsTheDefaultWorkspaceAndItsRoot) {
  const std::string root = std::filesystem::weakly_canonical(path_).string();
  EXPECT_EQ(invoke({"workspace", "root"}).output, root + "\n");
  EXPECT_EQ(invoke({"workspace", "list"}).output, "No workspaces.\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "default"}).code, 2);

  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"workspace", "root", "--name", "default"}).output,
            root + "\n");
  const Result listed = invoke({"workspace", "list"});
  EXPECT_NE(listed.output.find("default: "), std::string::npos);
  EXPECT_NE(listed.output.find(root), std::string::npos);
  EXPECT_EQ(invoke({"workspace", "root", "--name", "other"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "list", "-T", "name"}).code, 2);
}

TEST_F(RepositoryTest, ReportsRenamedFiles) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  std::filesystem::rename(path_ / "tracked.txt", path_ / "renamed.txt");
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("R renamed.txt"), std::string::npos) << status.output;
}

TEST_F(RepositoryTest, FiltersStatusPaths) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("directory/selected.txt", "selected\n");
  write("other.txt", "other\n");
  const Result selected = invoke({"status", "directory"});
  ASSERT_EQ(selected.code, 0) << selected.error;
  EXPECT_NE(selected.output.find("directory/selected.txt"), std::string::npos);
  EXPECT_EQ(selected.output.find("other.txt"), std::string::npos);
  EXPECT_NE(invoke({"status", "other.txt"}).output.find("other.txt"),
            std::string::npos);
  EXPECT_NE(invoke({"st", "missing"}).output.find("has no changes"),
            std::string::npos);
  EXPECT_NE(invoke({"status", "."}).output.find("other.txt"), std::string::npos);
  EXPECT_EQ(invoke({"status", ""}).code, 2);
  EXPECT_EQ(invoke({"status", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"status", "../outside"}).code, 2);
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

TEST_F(RepositoryTest, FiltersAndFormatsRevisionLogs) {
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  write("tracked.txt", "first\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result second = invoke({"new", "-m", "second"});
  ASSERT_EQ(second.code, 0) << second.error;
  const std::string second_id =
      token_after(second.output, "Working copy now at: ");
  write("second.txt", "second\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "third"}).code, 0);

  const Result limited =
      invoke({"log", "-r", "@", "--limit", "2", "--reversed",
              "--no-graph"});
  ASSERT_EQ(limited.code, 0) << limited.error;
  EXPECT_EQ(std::count(limited.output.begin(), limited.output.end(), '\n'), 2);
  EXPECT_EQ(limited.output.find("third") > limited.output.find("second"), true);
  EXPECT_FALSE(limited.output.starts_with("@  ") ||
               limited.output.starts_with("o  ") ||
               limited.output.starts_with("*  "));
  EXPECT_EQ(invoke({"log", "-r", "@", "--limit", "2", "--count"}).output,
            "2\n");
  EXPECT_EQ(invoke({"log", "--limit", "0"}).output, "");

  const Result filtered = invoke({"log", "-r", "@", "tracked.txt"});
  ASSERT_EQ(filtered.code, 0) << filtered.error;
  EXPECT_NE(filtered.output.find("first"), std::string::npos);
  EXPECT_EQ(filtered.output.find("second"), std::string::npos);
  EXPECT_EQ(filtered.output.find("third"), std::string::npos);

  const Result formatted = invoke(
      {"log", "-r", second_id, "-n", "1", "--summary", "--stat",
       "--types", "--name-only", "--git", "--color-words", "--context",
       "1", "--ignore-all-space", "--ignore-space-change"});
  ASSERT_EQ(formatted.code, 0) << formatted.error;
  EXPECT_NE(formatted.output.find("second.txt"), std::string::npos);
  EXPECT_NE(formatted.output.find("diff --git"), std::string::npos);
  EXPECT_EQ(invoke({"log", "--patch", "--tool", "meld"}).code, 2);
  EXPECT_EQ(invoke({"log", "-T", "description"}).code, 2);
  EXPECT_EQ(invoke({"log", "--limit", "word"}).code, 2);
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

TEST_F(RepositoryTest, NavigatesRevisionStacks) {
  const Result first_result = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first_result.code, 0) << first_result.error;
  const git_oid first = ref("refs/gg/workspaces/default");
  const Result second_result = invoke({"new", "-m", "second"});
  ASSERT_EQ(second_result.code, 0) << second_result.error;
  const git_oid second = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "third"}).code, 0);

  ASSERT_EQ(invoke({"prev", "--no-edit"}).code, 0);
  git_oid workspace = ref("refs/gg/workspaces/default");
  git_oid parent = commit_parent(workspace);
  EXPECT_NE(git_oid_equal(&parent, &first), 0);

  ASSERT_EQ(invoke({"next", "--no-edit"}).code, 0);
  workspace = ref("refs/gg/workspaces/default");
  parent = commit_parent(workspace);
  EXPECT_NE(git_oid_equal(&parent, &second), 0);
  EXPECT_EQ(invoke({"next", "--conflict"}).code, 2);
}

TEST_F(RepositoryTest, EditsAndValidatesNavigationTargets) {
  const Result first_result = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first_result.code, 0) << first_result.error;
  const std::string first_id =
      token_after(first_result.output, "Working copy now at: ");
  const git_oid first = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "third"}).code, 0);
  const git_oid third = ref("refs/gg/workspaces/default");

  ASSERT_EQ(invoke({"prev", "--edit"}).code, 0);
  git_oid workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&workspace, &second), 0);
  ASSERT_EQ(invoke({"next", "--edit"}).code, 0);
  workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&workspace, &third), 0);
  ASSERT_EQ(invoke({"prev", "--edit", "2"}).code, 0);
  workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&workspace, &first), 0);
  EXPECT_EQ(invoke({"prev", "--no-edit"}).code, 2);

  ASSERT_EQ(invoke({"new", "-m", "side", first_id}).code, 0);
  ASSERT_EQ(invoke({"edit", first_id}).code, 0);
  EXPECT_EQ(invoke({"next", "--edit"}).code, 2);
  EXPECT_EQ(invoke({"prev", "--edit", "99"}).code, 2);
}

TEST_F(RepositoryTest, NavigationAssignsChangeIdsToRawGitCommits) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  const git_oid child = raw_commit("raw child", {workspace});
  set_ref("refs/heads/raw-child", child);

  const Result moved = invoke({"next", "--edit"});
  ASSERT_EQ(moved.code, 0) << moved.error;
  const git_oid actual = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&actual, &child), 0);
  EXPECT_FALSE(current_id().empty());

  const git_oid unreferenced = raw_commit("unreferenced", {child});
  const Result edited = invoke({"edit", git_oid_tostr_s(&unreferenced)});
  ASSERT_EQ(edited.code, 0) << edited.error;
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

TEST_F(RepositoryTest, DescribesFromMessagesStdinAndEditors) {
  ASSERT_EQ(invoke({"new", "-m", "original", "main"}).code, 0);
  EXPECT_EQ(invoke({"describe", "-m", ""}).code, 0);
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find(
                "Description: (no description set)"),
            std::string::npos);

  std::istringstream input("from stdin\nsecond line\n");
  std::streambuf* old_input = std::cin.rdbuf(input.rdbuf());
  const Result from_stdin = invoke({"describe", "--stdin"});
  std::cin.rdbuf(old_input);
  std::cin.clear();
  ASSERT_EQ(from_stdin.code, 0) << from_stdin.error;
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find(
                "Description: from stdin"),
            std::string::npos);

  const std::filesystem::path editor = path_ / "description-editor";
  {
    std::ofstream script(editor);
    script << "#!/bin/sh\nprintf 'from editor\\n' > \"$1\"\n";
  }
  std::filesystem::permissions(
      editor, std::filesystem::perms::owner_exec |
                  std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write);
  const char* old_visual = std::getenv("VISUAL");
  const std::string saved_visual = old_visual == nullptr ? "" : old_visual;
  ASSERT_EQ(setenv("VISUAL", editor.string().c_str(), 1), 0);
  const Result from_editor = invoke({"describe", "--editor"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(from_editor.code, 0) << from_editor.error;
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find(
                "Description: from editor"),
            std::string::npos);
  EXPECT_EQ(invoke({"describe", "-m", "explicit", "@"}).code, 0);

  EXPECT_EQ(invoke({"describe", "-m", "x", "--stdin"}).code, 2);
  EXPECT_EQ(invoke({"describe", "--stdin", "--editor"}).code, 2);
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

  const Result limited = invoke({"operation", "log", "--limit", "1"});
  EXPECT_EQ(std::count(limited.output.begin(), limited.output.end(), '\n'), 1);
  EXPECT_EQ(limited.output.front(), '@');
  EXPECT_EQ(limited.output.find("│"), std::string::npos);
  EXPECT_EQ(invoke({"operation", "log", "-n", "0"}).output, "");

  const Result flat =
      invoke({"operation", "log", "--limit", "2", "--no-graph"});
  EXPECT_EQ(flat.output.find("@ "), std::string::npos);
  EXPECT_EQ(flat.output.find("○ "), std::string::npos);
  EXPECT_EQ(flat.output.find("│"), std::string::npos);
  EXPECT_EQ(std::count(flat.output.begin(), flat.output.end(), '\n'), 2);

  const Result reversed = invoke({"operation", "log", "--reversed"});
  EXPECT_LT(reversed.output.find("initialize repository"),
            reversed.output.find("redo: restore to operation "));
  EXPECT_EQ(invoke({"operation", "log", "-T", "description"}).code, 2);
  EXPECT_EQ(invoke({"operation", "log", "--limit", "word"}).code, 2);
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
  detail::Repository repo(path_);
  EXPECT_TRUE(repo.change_id(base).has_value());
  EXPECT_TRUE(repo.change_id(external).has_value());

  const Result edit = invoke({"edit", std::string(git_oid_tostr_s(&base))});
  EXPECT_EQ(edit.code, 0) << edit.error;
}

}  // namespace gg::test
