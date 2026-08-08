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
  const Result combined =
      invoke({"new", "-m", "combined", "--after", "main", "--before",
              "child", "--no-edit"});
  ASSERT_EQ(combined.code, 0) << combined.error;
  detail::Repository repo(path_);
  const git_oid combined_oid = repo.resolve(
      token_after(combined.output, "Created change: "));
  const std::vector<git_oid> combined_parents =
      repo.parents(ref("refs/heads/child"));
  EXPECT_TRUE(std::ranges::any_of(combined_parents, [&](const git_oid& oid) {
    return git_oid_equal(&oid, &combined_oid) != 0;
  }));

  const git_oid base = ref("refs/heads/main");
  const git_oid direct = raw_commit("direct", {base});
  set_ref("refs/heads/direct", direct);
  const Result direct_combined =
      invoke({"new", "-m", "direct combined", "--after", "main",
              "--before", "direct", "--no-edit"});
  ASSERT_EQ(direct_combined.code, 0) << direct_combined.error;
  const git_oid direct_combined_oid = repo.resolve(
      token_after(direct_combined.output, "Created change: "));
  const git_oid direct_parent = commit_parent(ref("refs/heads/direct"));
  EXPECT_NE(git_oid_equal(&direct_parent, &direct_combined_oid), 0);

  const git_oid left = raw_commit("left", {base});
  const git_oid right = raw_commit("right", {base});
  set_ref("refs/heads/left", left);
  set_ref("refs/heads/right", right);
  const Result multiple = invoke(
      {"new", "-m", "multiple", "--after", "left", "--after", "right",
       "--no-edit"});
  ASSERT_EQ(multiple.code, 0) << multiple.error;
  const git_oid multiple_oid =
      repo.resolve(token_after(multiple.output, "Created change: "));
  EXPECT_EQ(repo.parents(multiple_oid).size(), 2U);
  const Result multiple_before =
      invoke({"new", "-m", "multiple before", "--before", "left | right",
              "--no-edit"});
  ASSERT_EQ(multiple_before.code, 0) << multiple_before.error;
  const git_oid multiple_before_oid = repo.resolve(
      token_after(multiple_before.output, "Created change: "));
  const git_oid left_parent = commit_parent(ref("refs/heads/left"));
  const git_oid right_parent = commit_parent(ref("refs/heads/right"));
  EXPECT_NE(git_oid_equal(&left_parent, &multiple_before_oid), 0);
  EXPECT_NE(git_oid_equal(&right_parent, &multiple_before_oid), 0);

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
  EXPECT_EQ(invoke({"workspace", "list", "-T", "name"}).output,
            "No workspaces.\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "default"}).code, 2);

  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"workspace", "root", "--name", "default"}).output,
            root + "\n");
  const Result listed = invoke({"workspace", "list"});
  EXPECT_NE(listed.output.find("default: "), std::string::npos);
  EXPECT_NE(listed.output.find(root), std::string::npos);
  const Result templated = invoke(
      {"workspace", "list", "-T",
       "name ++ \" \" ++ target.short() ++ \" \" ++ root ++ \" \" ++ "
       "working_copy ++ \"\\n\""});
  ASSERT_EQ(templated.code, 0) << templated.error;
  EXPECT_TRUE(templated.output.starts_with("default "));
  EXPECT_TRUE(templated.output.ends_with(" " + root + " true\n"));
  EXPECT_EQ(invoke({"workspace", "root", "--name", "other"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "list", "-T", "unknown"}).code, 2);
}

TEST_F(RepositoryTest, RenamesAndForgetsTheCurrentWorkspace) {
  const std::string root = std::filesystem::weakly_canonical(path_).string();
  EXPECT_EQ(invoke({"workspace", "rename", "topic"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "forget"}).output,
            "No such workspace: default\nNothing changed.\n");

  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid original = ref(detail::kWorkspaceRef);
  ASSERT_EQ(invoke({"workspace", "rename", "topic"}).code, 0);
  EXPECT_FALSE(has_ref(detail::kWorkspaceRef));
  ASSERT_TRUE(has_ref("refs/gg/workspaces/topic"));
  const git_oid renamed = ref("refs/gg/workspaces/topic");
  EXPECT_EQ(git_oid_equal(&original, &renamed), 1);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("topic: "),
            std::string::npos);
  EXPECT_EQ(invoke({"workspace", "list", "-T", "name ++ \"\\n\""}).output,
            "topic\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "topic"}).output,
            root + "\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "default"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "rename", "topic"}).output,
            "Nothing changed.\n");
  EXPECT_EQ(invoke({"workspace", "rename", "bad..name"}).code, 2);

  ASSERT_EQ(invoke({"describe", "-m", "renamed workspace"}).code, 0);
  EXPECT_FALSE(has_ref(detail::kWorkspaceRef));
  EXPECT_TRUE(has_ref("refs/gg/workspaces/topic"));
  EXPECT_EQ(invoke({"workspace", "forget", "missing"}).output,
            "No such workspace: missing\nNothing changed.\n");
  ASSERT_EQ(invoke({"workspace", "forget"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/topic"));
  EXPECT_EQ(invoke({"workspace", "list"}).output, "No workspaces.\n");

  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(has_ref("refs/gg/workspaces/topic"));
  EXPECT_EQ(invoke({"log", "-r", "@", "--no-graph", "-T", "description"})
                .output,
            "renamed workspace");

  set_ref("refs/gg/workspaces/duplicate",
          ref("refs/gg/workspaces/topic"));
  EXPECT_EQ(invoke({"workspace", "list"}).code, 2);
}

TEST_F(RepositoryTest, ListsAndResetsFullWorkingCopyPatterns) {
  EXPECT_EQ(invoke({"sparse", "list"}).code, 2);
  EXPECT_EQ(invoke({"sparse", "reset"}).code, 2);

  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"sparse", "list"}).output, ".\n");
  EXPECT_EQ(invoke({"sparse", "reset"}).output, "");
  EXPECT_EQ(invoke({"--at-op", "@", "sparse", "list"}).output, ".\n");
  EXPECT_EQ(invoke({"--at-op", "@", "sparse", "reset"}).code, 2);
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
      invoke({"log", "-r", "ancestors(@)", "--limit", "2", "--reversed",
              "--no-graph"});
  ASSERT_EQ(limited.code, 0) << limited.error;
  EXPECT_EQ(std::count(limited.output.begin(), limited.output.end(), '\n'), 2);
  EXPECT_EQ(limited.output.find("third") > limited.output.find("second"), true);
  EXPECT_FALSE(limited.output.starts_with("@  ") ||
               limited.output.starts_with("○  ") ||
               limited.output.starts_with("*  "));
  EXPECT_EQ(invoke({"log", "-r", "ancestors(@)", "--limit", "2", "--count"}).output,
            "2\n");
  const Result revset_log =
      invoke({"log", "-r", "@ | @-", "--no-graph"});
  EXPECT_NE(revset_log.output.find("third"), std::string::npos);
  EXPECT_NE(revset_log.output.find("second"), std::string::npos);
  EXPECT_EQ(invoke({"log", "--limit", "0"}).output, "");

  const Result colored =
      invoke({"--color", "always", "log", "-r", "@", "-n", "1"});
  EXPECT_NE(colored.output.find("\x1b[1;38;5;2m@\x1b[0m"),
            std::string::npos);
  EXPECT_NE(colored.output.find("\x1b[1;38;5;13m"), std::string::npos);
  EXPECT_EQ(invoke({"--color", "never", "log", "-n", "1"})
                .output.find("\x1b["),
            std::string::npos);
  const Result debug =
      invoke({"--color", "debug", "log", "-r", "@", "-n", "1"});
  EXPECT_NE(debug.output.find("<<working_copy::@>>"), std::string::npos);
  EXPECT_NE(debug.output.find("<<working_copy change_id shortest prefix::"),
            std::string::npos);
  EXPECT_NE(debug.output.find("<<working_copy change_id shortest rest::"),
            std::string::npos);
  EXPECT_NE(debug.output.find("<<working_copy commit_id shortest prefix::"),
            std::string::npos);
  EXPECT_NE(debug.output.find("<<working_copy commit_id shortest rest::"),
            std::string::npos);
  EXPECT_EQ(invoke({"--color", "invalid", "log"}).code, 2);

  const Result filtered =
      invoke({"log", "-r", "ancestors(@)", "tracked.txt"});
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
  const Result templated = invoke(
      {"log", "-r", second_id, "-n", "1", "--no-graph", "-T",
       "change_id.short() ++ \" \" ++ commit_id.short(12) ++ \" \" ++ "
       "description.first_line() ++ \" \" ++ author.name ++ \" \" ++ "
       "author.email ++ \" \" ++ committer.name ++ \" \" ++ "
       "committer.email ++ \" \" ++ bookmarks ++ \" \" ++ working_copy ++ "
       "\"\\n\""});
  ASSERT_EQ(templated.code, 0) << templated.error;
  EXPECT_TRUE(templated.output.starts_with(second_id.substr(0, 8) + " "));
  EXPECT_NE(templated.output.find(" second GG Test gg@example.test GG Test "
                                  "gg@example.test  false\n"),
            std::string::npos);
  EXPECT_EQ(invoke({"log", "-r", "@", "-n", "1", "--no-graph", "-T",
                    "self.subject ++ \" \" ++ working_copy ++ \"\\n\""})
                .output,
            "third true\n");
  EXPECT_EQ(invoke({"log", "--limit", "word"}).code, 2);
}

TEST_F(RepositoryTest, HighlightsPrefixesWithinTheDisplayedLog) {
  const git_oid first = ref("HEAD");
  const std::string first_commit = detail::oid_string(first);
  git_oid second{};
  for (int index = 0; index < 1000; ++index) {
    second = raw_commit("prefix collision " + std::to_string(index));
    if (detail::oid_string(second).front() == first_commit.front()) break;
  }
  ASSERT_EQ(detail::oid_string(second).front(), first_commit.front());
  set_ref("refs/heads/first", first);
  set_ref("refs/heads/second", second);
  const std::string first_change = "zzzzzzzzl" + std::string(23, 'k');
  const std::string second_change = "zzzzzzzzm" + std::string(23, 'k');
  set_ref(std::string(detail::kChangePrefix) + first_change, first);
  set_ref(std::string(detail::kChangePrefix) + second_change, second);

  const Result single = invoke(
      {"--color", "debug", "log", "-r", "first", "--no-graph"});
  ASSERT_EQ(single.code, 0) << single.error;
  EXPECT_NE(single.output.find("<<change_id shortest prefix::z>>"),
            std::string::npos);
  EXPECT_NE(single.output.find("<<commit_id shortest prefix::" +
                               first_commit.substr(0, 1) + ">>"),
            std::string::npos);

  const Result both = invoke({"--color", "debug", "log", "-r",
                              "first | second", "--no-graph"});
  ASSERT_EQ(both.code, 0) << both.error;
  EXPECT_NE(both.output.find(
                "<<change_id shortest prefix::zzzzzzzzl>>"),
            std::string::npos);
  std::size_t common = 0;
  const std::string second_commit = detail::oid_string(second);
  while (first_commit[common] == second_commit[common]) ++common;
  EXPECT_NE(both.output.find("<<commit_id shortest prefix::" +
                             first_commit.substr(0, common + 1) + ">>"),
            std::string::npos);
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

  ASSERT_EQ(invoke({"prev"}).code, 0);
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

TEST_F(RepositoryTest, NavigationUsesLayeredEditConfiguration) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  const git_oid child = raw_commit("configured child", {workspace});
  set_ref("refs/heads/configured-child", child);
  ASSERT_EQ(invoke({"config", "set", "--repo", "ui.movement.edit", "true"})
                .code,
            0);

  ASSERT_EQ(invoke({"next"}).code, 0);
  git_oid actual = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&actual, &child), 0);

  detail::Repository repo(path_);
  const std::size_t ids_before_no_edit = repo.changes().size();
  ASSERT_EQ(invoke({"prev", "--no-edit"}).code, 0);
  const git_oid no_edit = ref("refs/gg/workspaces/default");
  EXPECT_EQ(repo.changes().size(), ids_before_no_edit + 1);
  const std::vector<git_oid> no_edit_parents = repo.parents(no_edit);
  ASSERT_EQ(no_edit_parents.size(), 1U);
  const git_oid main = ref("refs/heads/main");
  EXPECT_NE(git_oid_equal(&no_edit_parents.front(), &main), 0);

  ASSERT_EQ(invoke({"config", "set", "--workspace", "ui.movement.edit", "1"})
                .code,
            0);
  EXPECT_EQ(invoke({"prev"}).code, 2);
  ASSERT_EQ(invoke({"config", "set", "--workspace", "ui.movement.edit", "false"})
                .code,
            0);
  EXPECT_EQ(invoke({"prev"}).code, 2);
  EXPECT_EQ(invoke({"prev", "--edit"}).code, 0);
}

TEST_F(RepositoryTest, CanIgnoreWorkingCopySynchronization) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "unsnapshotted\n");
  const Result stale = invoke({"--ignore-working-copy", "status"});
  ASSERT_EQ(stale.code, 0) << stale.error;
  EXPECT_EQ(stale.output.find("M tracked.txt"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("M tracked.txt"),
            std::string::npos);

  ASSERT_EQ(invoke({"--ignore-working-copy", "edit", "main"}).code, 0);
  EXPECT_EQ(file(), "unsnapshotted\n");
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
  const Result forced_editor =
      invoke({"describe", "-m", "seed", "--editor"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(from_editor.code, 0) << from_editor.error;
  ASSERT_EQ(forced_editor.code, 0) << forced_editor.error;
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find(
                "Description: from editor"),
            std::string::npos);
  EXPECT_EQ(invoke({"describe", "-m", "explicit", "@"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  const Result multiple =
      invoke({"describe", "-m", "bulk", "-r", "@ | @-"});
  ASSERT_EQ(multiple.code, 0) << multiple.error;
  EXPECT_NE(multiple.output.find("Rewrote 2 revision(s)."),
            std::string::npos);
  const std::string shown = invoke({"show", "@ | @-", "--no-patch"}).output;
  const std::size_t first_bulk = shown.find("Description: bulk");
  ASSERT_NE(first_bulk, std::string::npos);
  EXPECT_NE(shown.find("Description: bulk", first_bulk + 1),
            std::string::npos);

  EXPECT_EQ(invoke({"describe", "-m", "x", "--stdin"}).code, 2);
}

TEST_F(RepositoryTest, DescribesRawGitHistoryWithoutAWorkspace) {
  const Result described = invoke({"describe", "-m", "raw", "main"});
  ASSERT_EQ(described.code, 0) << described.error;
  EXPECT_NE(invoke({"show", "main", "--no-patch"})
                .output.find("Description: raw"),
            std::string::npos);
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
  ASSERT_EQ(invoke({"bookmark", "create", "moving"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "set", "moving", "-r", "@"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "delete", "moving"}).code, 0);
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
  EXPECT_NE(log.output.find("\n│  "), std::string::npos);
  EXPECT_EQ(invoke({"op", "log"}).output, log.output);

  const Result colored =
      invoke({"--color", "debug", "operation", "log", "--limit", "2"});
  EXPECT_NE(colored.output.find("<<working_copy::@>>"), std::string::npos);
  EXPECT_NE(colored.output.find("<<current_operation_id::"),
            std::string::npos);
  EXPECT_NE(colored.output.find("<<timestamp::"), std::string::npos);
  EXPECT_NE(colored.output.find("<<change_id::○>>"), std::string::npos);
  EXPECT_NE(colored.output.find("<<operation_id::"), std::string::npos);

  const Result diff = invoke({"operation", "log", "--op-diff", "--no-graph"});
  EXPECT_NE(diff.output.find("  + HEAD "), std::string::npos);
  EXPECT_NE(diff.output.find("  ~ HEAD "), std::string::npos);
  EXPECT_NE(diff.output.find("  + refs/heads/moving "), std::string::npos);
  EXPECT_NE(diff.output.find("  ~ refs/heads/moving "), std::string::npos);
  EXPECT_NE(diff.output.find("  - refs/heads/moving "), std::string::npos);
  const Result colored_diff = invoke(
      {"--color", "debug", "operation", "log", "--op-diff", "--no-graph"});
  EXPECT_NE(colored_diff.output.find("<<added::  + HEAD "),
            std::string::npos);
  EXPECT_NE(colored_diff.output.find("<<modified::  ~ HEAD "),
            std::string::npos);
  EXPECT_NE(colored_diff.output.find("<<removed::  - refs/heads/moving "),
            std::string::npos);

  const Result limited = invoke({"operation", "log", "--limit", "1"});
  EXPECT_EQ(std::count(limited.output.begin(), limited.output.end(), '\n'), 2);
  EXPECT_EQ(limited.output.front(), '@');
  EXPECT_NE(limited.output.find("│"), std::string::npos);
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
  const Result reversed_diff =
      invoke({"operation", "log", "--reversed", "--op-diff"});
  EXPECT_EQ(reversed_diff.code, 0) << reversed_diff.error;
  EXPECT_NE(reversed_diff.output.find("refs/"), std::string::npos);
  EXPECT_EQ(invoke({"operation", "log", "--reversed", "--limit", "2",
                    "--op-diff"})
                .code,
            0);
  EXPECT_EQ(invoke({"operation", "log", "--reversed", "--limit", "0",
                    "--op-diff"})
                .code,
            0);
  EXPECT_EQ(invoke({"operation", "log", "--limit", "0", "--op-diff"})
                .code,
            0);
  const git_oid current = ref("refs/gg/operations/current");
  const std::string current_id = git_oid_tostr_s(&current);
  const Result templated = invoke(
      {"operation", "log", "--limit", "1", "--no-graph", "-T",
       "id.short(12) ++ \" \" ++ current_operation ++ \" \" ++ "
       "description.first_line() ++ \" \" ++ snapshot ++ \" \" ++ "
       "workspace_name ++ \" \" ++ time ++ \" \" ++ user ++ \" \" ++ "
       "root ++ \" \" ++ parents.short() ++ \" \" ++ attributes ++ "
       "tags ++ \"\\n\""});
  ASSERT_EQ(templated.code, 0) << templated.error;
  EXPECT_TRUE(templated.output.starts_with(current_id.substr(0, 12) +
                                           " true redo: restore"));
  const Result templated_states =
      invoke({"operation", "log", "--no-graph", "-T",
              "current_operation ++ \" \" ++ root ++ \" \" ++ "
              "parents.short() ++ \"\\n\""});
  EXPECT_NE(templated_states.output.find("true false "), std::string::npos);
  EXPECT_NE(templated_states.output.find("false true \n"), std::string::npos);
  EXPECT_EQ(invoke({"operation", "log", "-T", "unknown"}).code, 2);
  EXPECT_EQ(invoke({"operation", "log", "--limit", "word"}).code, 2);
}

TEST_F(RepositoryTest, ShowsChangedRevisionPatchesInOperationLogs) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "first\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid first = ref("refs/gg/workspaces/default");
  const std::string first_oid = git_oid_tostr_s(&first);
  write("tracked.txt", "second\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const std::vector<std::string> prefix{
      "operation", "log", "--limit", "1", "--no-graph"};
  const auto with = [&](std::vector<std::string> suffix) {
    std::vector<std::string> arguments = prefix;
    arguments.insert(arguments.end(), suffix.begin(), suffix.end());
    return invoke(std::move(arguments));
  };
  const Result patch = with({"--patch"});
  ASSERT_EQ(patch.code, 0) << patch.error;
  EXPECT_NE(patch.output.find("diff --git a/tracked.txt b/tracked.txt"),
            std::string::npos);
  EXPECT_NE(patch.output.find("-first"), std::string::npos);
  EXPECT_NE(patch.output.find("+second"), std::string::npos);
  EXPECT_NE(with({"--git"}).output.find("diff --git"), std::string::npos);
  EXPECT_NE(with({"--summary"}).output.find("M tracked.txt"),
            std::string::npos);
  EXPECT_NE(with({"--stat"}).output.find("1 file changed"),
            std::string::npos);
  EXPECT_NE(with({"--types"}).output.find("FF tracked.txt"),
            std::string::npos);
  EXPECT_NE(with({"--name-only"}).output.find("tracked.txt"),
            std::string::npos);
  EXPECT_NE(with({"--color-words"}).output.find("+second"),
            std::string::npos);
  EXPECT_NE(with({"--context", "0"}).output.find("@@ -1 +1 @@"),
            std::string::npos);
  EXPECT_NE(with({"--ignore-all-space"}).output.find("diff --git"),
            std::string::npos);
  EXPECT_NE(with({"--ignore-space-change"}).output.find("diff --git"),
            std::string::npos);
  EXPECT_EQ(with({"--summary", "--show-changes-in", "none()"})
                .output.find("M tracked.txt"),
            std::string::npos);
  EXPECT_NE(with({"--summary", "--show-changes-in", "all()"})
                .output.find("M tracked.txt"),
            std::string::npos);
  EXPECT_NE(with({"--summary", "--show-changes-in", first_oid})
                .output.find("M tracked.txt"),
            std::string::npos);
  EXPECT_EQ(with({"--tool", "missing"}).code, 2);
  ASSERT_EQ(invoke({"operation", "log", "--no-graph", "--summary"}).code,
            0);

  ASSERT_EQ(invoke({"new"}).code, 0);
  ASSERT_EQ(with({"--summary", "--show-changes-in", "none()"}).code, 0);
  ASSERT_EQ(invoke({"abandon", "@"}).code, 0);
  ASSERT_EQ(with({"--summary"}).code, 0);
  ASSERT_EQ(with({"--summary", "--show-changes-in", "none()"}).code, 0);
}

TEST_F(RepositoryTest, RestoresAllOrSelectedOperationState) {
  ASSERT_EQ(invoke({"new", "-m", "first", "main"}).code, 0);
  const git_oid first = ref("refs/gg/workspaces/default");
  set_ref("refs/remotes/origin/main", first);
  set_ref("refs/gg/remotes/origin/tags/v1", first);
  set_ref("refs/gg/tracking/bookmarks/origin/main", first);
  set_ref("refs/gg/tracking/tags/origin/v1", first);
  ASSERT_EQ(invoke({"bookmark", "create", "keep"}).code, 0);
  const git_oid target_operation = ref("refs/gg/operations/current");
  const std::string target =
      std::string(git_oid_tostr_s(&target_operation)).substr(0, 8);

  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  set_ref("refs/remotes/origin/main", second);
  set_ref("refs/gg/remotes/origin/tags/v1", second);
  set_ref("refs/gg/tracking/bookmarks/origin/main", second);
  set_ref("refs/gg/tracking/tags/origin/v1", second);
  ASSERT_EQ(invoke({"bookmark", "delete", "keep"}).code, 0);

  Result restored = invoke({"operation", "restore", "--what", "repo", target});
  ASSERT_EQ(restored.code, 0) << restored.error;
  EXPECT_NE(restored.output.find(target), std::string::npos);
  EXPECT_TRUE(has_ref("refs/heads/keep"));
  git_oid actual = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &second), 0);
  actual = ref("refs/gg/remotes/origin/tags/v1");
  EXPECT_NE(git_oid_equal(&actual, &second), 0);
  actual = ref("refs/gg/tracking/bookmarks/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &second), 0);

  restored = invoke(
      {"op", "restore", "--what", "remote-tracking", target});
  ASSERT_EQ(restored.code, 0) << restored.error;
  actual = ref("refs/remotes/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/gg/remotes/origin/tags/v1");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/gg/tracking/bookmarks/origin/main");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);
  actual = ref("refs/gg/tracking/tags/origin/v1");
  EXPECT_NE(git_oid_equal(&actual, &first), 0);

  ASSERT_EQ(invoke({"new", "-m", "third"}).code, 0);
  set_ref("refs/remotes/origin/main", second);
  set_ref("refs/gg/remotes/origin/tags/v1", second);
  set_ref("refs/gg/tracking/bookmarks/origin/main", second);
  set_ref("refs/gg/tracking/tags/origin/v1", second);
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

TEST_F(RepositoryTest, ReadsRepositoryStateAtHistoricalOperations) {
  ASSERT_EQ(invoke({"new", "-m", "before", "main"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "historical"}).code, 0);
  detail::Repository repo(path_);
  const git_oid before_revision = repo.resolve("@");
  const git_oid before_operation = *repo.operation();
  const std::string before_id = detail::oid_string(before_operation);
  ASSERT_EQ(invoke({"describe", "-m", "after"}).code, 0);

  const Result historical = invoke(
      {"--at-operation", before_id, "show", "@", "--no-patch"});
  ASSERT_EQ(historical.code, 0) << historical.error;
  EXPECT_NE(historical.output.find("Description: before"), std::string::npos);
  EXPECT_EQ(historical.output.find("Description: after"), std::string::npos);
  const Result historical_tag =
      invoke({"--at-operation", before_id, "tag", "list", "historical"});
  ASSERT_EQ(historical_tag.code, 0) << historical_tag.error;
  EXPECT_NE(historical_tag.output.find(detail::oid_string(before_revision, 8)),
            std::string::npos);
  EXPECT_NE(invoke({"--at-operation", before_id, "log", "-r",
                    "tags(exact:historical)"})
                .output.find("before"),
            std::string::npos);

  const Result operation_log =
      invoke({"--at-op", before_id, "operation", "log", "--limit", "1"});
  ASSERT_EQ(operation_log.code, 0) << operation_log.error;
  EXPECT_NE(operation_log.output.find(detail::oid_string(before_operation, 8)),
            std::string::npos);
  EXPECT_EQ(invoke({"--at-operation", before_id, "new", "main"}).code, 2);
  EXPECT_EQ(invoke({"--at-operation", before_id, "util", "exec", "true"})
                .code,
            2);
  EXPECT_EQ(invoke({"--at-operation", "missing", "log"}).code, 2);

  detail::Repository historical_repo(path_);
  historical_repo.view_at_operation(before_id);
  EXPECT_FALSE(historical_repo.head_state().symbolic);
  EXPECT_TRUE(historical_repo.ref_target("HEAD").has_value());
  EXPECT_TRUE(historical_repo.ref_target(detail::kOperationRef).has_value());
  EXPECT_FALSE(historical_repo.ref_target("refs/heads/missing").has_value());
  const auto new_operation = repo.operation_previous(before_operation);
  ASSERT_TRUE(new_operation.has_value());
  const auto initial_operation = repo.operation_previous(*new_operation);
  ASSERT_TRUE(initial_operation.has_value());
  historical_repo = detail::Repository(path_);
  historical_repo.view_at_operation(detail::oid_string(*initial_operation));
  EXPECT_TRUE(historical_repo.head_state().symbolic);
  EXPECT_TRUE(historical_repo.ref_target("HEAD").has_value());
  detail::OperationState missing_head = historical_repo.state();
  missing_head.head = {true, "refs/heads/missing"};
  const git_oid missing_head_operation = historical_repo.create_operation(
      missing_head, initial_operation, "missing historical HEAD");
  repo.apply_refs({{std::string(detail::kOperationRef), missing_head_operation}},
                  {}, "test historical HEAD");
  historical_repo = detail::Repository(path_);
  historical_repo.view_at_operation(
      detail::oid_string(missing_head_operation));
  EXPECT_FALSE(historical_repo.ref_target("HEAD").has_value());

  const git_oid current = repo.resolve("@");
  EXPECT_EQ(std::string(git_commit_message(repo.commit(current).get())),
            "after");
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
  ASSERT_EQ(invoke({"new", "-m", "grandchild"}).code, 0);
  const std::string grandchild = current_id();

  ASSERT_EQ(invoke({"edit", root.output.substr(root.output.find(": ") + 2, 8)}).code,
            0);
  ASSERT_EQ(invoke({"new", "-m", "left parent"}).code, 0);
  write("left.txt", "left\n");
  const Result left_status = invoke({"status"});
  ASSERT_EQ(left_status.code, 0);
  EXPECT_NE(left_status.output.find("A left.txt"), std::string::npos);
  const std::string left = current_id();
  ASSERT_EQ(invoke({"new", "-m", "merge", left, grandchild}).code, 0);
  git_commit* merge = nullptr;
  const git_oid workspace = ref("refs/gg/workspaces/default");
  ASSERT_EQ(git_commit_lookup(&merge, repository_.get(), &workspace), 0);
  EXPECT_EQ(git_commit_parentcount(merge), 2U);
  git_commit_free(merge);
  const Result graph = invoke({"log", "-r", "ancestors(@)"});
  EXPECT_NE(graph.output.find("├─╮"), std::string::npos) << graph.output;
  EXPECT_NE(graph.output.find("○ │"), std::string::npos) << graph.output;
  EXPECT_NE(graph.output.find("│ ○"), std::string::npos) << graph.output;
  EXPECT_NE(graph.output.find("├─╯"), std::string::npos) << graph.output;
  EXPECT_EQ(graph.output.find('*'), std::string::npos) << graph.output;
  const Result reversed =
      invoke({"log", "-r", "ancestors(@)", "--reversed"});
  EXPECT_NE(reversed.output.find("│"), std::string::npos);
}

TEST_F(RepositoryTest, MergesUnrelatedParents) {
  const git_oid first_base = raw_commit("first base");
  const git_oid first = raw_commit("first", {first_base});
  const git_oid third = raw_commit("third");
  const git_oid second = raw_commit("second", {third});
  set_ref("refs/heads/first", first);
  set_ref("refs/heads/second", second);
  set_ref("refs/heads/third", third);
  const Result merge = invoke({"new", "first", "second", "third"});
  ASSERT_EQ(merge.code, 0) << merge.error;
  const Result graph = invoke({"log", "-r", "ancestors(@)"});
  EXPECT_EQ(graph.code, 0);
  EXPECT_NE(graph.output.find("┬"), std::string::npos);
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
