// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"

#include <algorithm>

namespace gg::test {

TEST_F(RepositoryTest, RewritesAncestorsWhileEditingDescendants) {
  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  write("one.txt", "one\n");
  write("two.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "descendant"}).code, 0);

  ASSERT_EQ(invoke({"split", "-r", first_id, "-m", "selected", "one.txt"}).code,
            0);
  ASSERT_EQ(invoke({"squash", "-r", first_id, "-m", "combined"}).code, 0);
  EXPECT_NE(invoke({"log"}).output.find("descendant"), std::string::npos);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, DuplicatesABranchWithoutRewritingTheOriginal) {
  const git_oid base = ref("HEAD");
  const Result root_result = invoke({"new", "-m", "root", "main"});
  ASSERT_EQ(root_result.code, 0) << root_result.error;
  const std::string root_id = token_after(root_result.output, "Working copy now at: ");
  write("root.txt", "root\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result child_result = invoke({"new", "-m", "child"});
  ASSERT_EQ(child_result.code, 0) << child_result.error;
  const std::string child_id = token_after(child_result.output, "Working copy now at: ");
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  detail::Repository before(path_);
  const git_oid original_root = before.resolve(root_id);
  const git_oid original_child = before.resolve(child_id);
  const Result duplicated = invoke({"duplicate", "-r", root_id, "--descendants"});
  ASSERT_EQ(duplicated.code, 0) << duplicated.error;
  EXPECT_NE(duplicated.output.find("Duplicated 2 revision(s)."), std::string::npos);

  detail::Repository after(path_);
  const git_oid duplicate_child = ref("refs/gg/workspaces/default");
  ASSERT_EQ(git_oid_equal(&duplicate_child, &original_child), 0);
  const std::vector<git_oid> child_parents = after.parents(duplicate_child);
  ASSERT_EQ(child_parents.size(), 1U);
  const git_oid duplicate_root = child_parents.front();
  ASSERT_EQ(git_oid_equal(&duplicate_root, &original_root), 0);
  const std::vector<git_oid> root_parents = after.parents(duplicate_root);
  ASSERT_EQ(root_parents.size(), 1U);
  EXPECT_NE(git_oid_equal(&root_parents.front(), &base), 0);
  EXPECT_NE(git_oid_equal(git_commit_tree_id(after.commit(original_root).get()),
                git_commit_tree_id(after.commit(duplicate_root).get())), 0);
  EXPECT_NE(git_oid_equal(git_commit_tree_id(after.commit(original_child).get()),
                git_commit_tree_id(after.commit(duplicate_child).get())), 0);
  const std::vector<git_oid> visible = after.resolve_set("all()");
  const auto is_visible = [&](const git_oid& expected) {
    return std::ranges::any_of(visible, [&](const git_oid& oid) {
      return git_oid_equal(&oid, &expected) != 0;
    });
  };
  EXPECT_TRUE(is_visible(original_root));
  EXPECT_TRUE(is_visible(original_child));
  EXPECT_TRUE(is_visible(duplicate_root));
  EXPECT_TRUE(is_visible(duplicate_child));

  ASSERT_EQ(invoke({"undo"}).code, 0);
  const git_oid restored_workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&restored_workspace, &original_child), 0);
}

TEST_F(RepositoryTest, DuplicatesOnlyTheSelectedChangeByDefault) {
  const Result root_result = invoke({"new", "-m", "root", "main"});
  ASSERT_EQ(root_result.code, 0) << root_result.error;
  const std::string root_id = token_after(root_result.output, "Working copy now at: ");
  const Result child_result = invoke({"new", "-m", "child"});
  ASSERT_EQ(child_result.code, 0) << child_result.error;
  const git_oid workspace = ref("refs/gg/workspaces/default");

  const Result duplicated = invoke({"duplicate", "-r", root_id});
  ASSERT_EQ(duplicated.code, 0) << duplicated.error;
  EXPECT_NE(duplicated.output.find("Duplicated 1 revision(s)."), std::string::npos);
  const git_oid unchanged_workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&unchanged_workspace, &workspace), 0);
}

TEST_F(RepositoryTest, RewritesChangesOutsideTheCurrentLine) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("one.txt", "one\n");
  write("two.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result current = invoke({"new", "-m", "current", "main"});
  ASSERT_EQ(current.code, 0) << current.error;
  const std::string current_id_value =
      token_after(current.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"split", "-r", source_id, "one.txt"}).code, 0);
  ASSERT_EQ(invoke({"describe", "-m", "described", source_id}).code, 0);
  EXPECT_EQ(current_id(), current_id_value.substr(0, 8));
}

TEST_F(RepositoryTest, SplitsDirectoryPaths) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("directory/one.txt", "one\n");
  write("directory-other.txt", "not in the directory\n");
  write("other.txt", "other\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result split = invoke({"split", "directory"});
  ASSERT_EQ(split.code, 0) << split.error;
  EXPECT_NE(invoke({"status"}).output.find("A other.txt"), std::string::npos);
}

TEST_F(RepositoryTest, SquashesAnAncestorOfTheWorkingCopy) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  const Result child = invoke({"new", "-m", "child"});
  ASSERT_EQ(child.code, 0) << child.error;
  const std::string child_id = token_after(child.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"squash", "-r", source_id}).code, 0);
  detail::Repository repo(path_);
  const git_oid resolved_child = repo.resolve(child_id);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&resolved_child, &workspace), 0);
}

TEST_F(RepositoryTest, RebasesWithoutConflicts) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result rebased =
      invoke({"rebase", "-s", source_id, "--onto", destination_id});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  EXPECT_NE(rebased.output.find("Rebased"), std::string::npos);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, RebasesAnAncestorOfTheWorkingCopy) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result child = invoke({"new", "-m", "child"});
  ASSERT_EQ(child.code, 0) << child.error;
  const std::string child_id = token_after(child.output, "Working copy now at: ");
  EXPECT_EQ(invoke({"rebase", "-s", source_id, "-d", child_id}).code, 2);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"edit", child_id}).code, 0);

  const Result rebased =
      invoke({"rebase", "-s", source_id, "-d", destination_id});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  detail::Repository repo(path_);
  const git_oid resolved_child = repo.resolve(child_id);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&resolved_child, &workspace), 0);
}

TEST_F(RepositoryTest, RejectsSplittingAllChanges) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("only.txt", "only\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(invoke({"split", "only.txt"}).code, 2);
}

TEST_F(RepositoryTest, SquashesAndAbandonsCurrentChanges) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const Result source = invoke({"new", "-m", "source"});
  ASSERT_EQ(source.code, 0) << source.error;
  ASSERT_EQ(invoke({"squash"}).code, 0);

  ASSERT_EQ(invoke({"new", "-m", "discard"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "discarded"}).code, 0);
  ASSERT_EQ(invoke({"abandon"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/discarded"));
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, RejectsInvalidRewriteShapes) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(git_reference_remove(repository_.get(), "refs/heads/main"), 0);
  std::filesystem::remove(path_ / "tracked.txt");
  const Result root = invoke({"new", "-m", "root"});
  ASSERT_EQ(root.code, 0) << root.error;
  const std::string root_id = token_after(root.output, "Working copy now at: ");
  EXPECT_EQ(invoke({"rebase", "-s", root_id, "-d",
                    std::string(git_oid_tostr_s(&base))})
                .code,
            2);
  EXPECT_EQ(invoke({"split", "-r", root_id, "anything"}).code, 2);
  EXPECT_EQ(invoke({"squash", "-r", root_id}).code, 2);
  EXPECT_EQ(invoke({"abandon", root_id}).code, 2);

}

TEST_F(RepositoryTest, SquashesIntoANonParent) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  const Result other = invoke({"new", "-m", "other", "main"});
  ASSERT_EQ(other.code, 0) << other.error;
  const std::string other_id = token_after(other.output, "Working copy now at: ");
  const Result squash =
      invoke({"squash", "--from", source_id, "--into", other_id});
  ASSERT_EQ(squash.code, 0) << squash.error;
  detail::Repository repo(path_);
  const git_oid resolved_source = repo.resolve(source_id);
  const git_oid resolved_other = repo.resolve(other_id);
  EXPECT_NE(git_oid_equal(&resolved_source, &resolved_other), 0);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, SquashesEntireBranchIntoDestination) {
  const Result root = invoke({"new", "-m", "branch root", "main"});
  ASSERT_EQ(root.code, 0) << root.error;
  write("root.txt", "root\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result tip = invoke({"new", "-m", "branch tip"});
  ASSERT_EQ(tip.code, 0) << tip.error;
  const std::string root_id = token_after(root.output, "Working copy now at: ");
  const std::string tip_id = token_after(tip.output, "Working copy now at: ");
  write("tip.txt", "tip\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result squash = invoke({"squash", "--from", tip_id, "--into",
                                destination_id, "--entire-branch"});
  ASSERT_EQ(squash.code, 0) << squash.error;
  detail::Repository repo(path_);
  const git_oid resolved_root = repo.resolve(root_id);
  const git_oid resolved_tip = repo.resolve(tip_id);
  const git_oid resolved_destination = repo.resolve(destination_id);
  EXPECT_NE(git_oid_equal(&resolved_root, &resolved_destination), 0);
  EXPECT_NE(git_oid_equal(&resolved_tip, &resolved_destination), 0);
  EXPECT_TRUE(std::filesystem::exists(path_ / "root.txt"));
  EXPECT_TRUE(std::filesystem::exists(path_ / "tip.txt"));
  EXPECT_TRUE(std::filesystem::exists(path_ / "destination.txt"));
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, SquashesAndAbandonsUnrelatedChanges) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id = token_after(source.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"new", "-m", "current", "main"}).code, 0);
  ASSERT_EQ(invoke({"squash", "-r", source_id}).code, 0);

  const Result discarded = invoke({"new", "-m", "discarded", "main"});
  ASSERT_EQ(discarded.code, 0) << discarded.error;
  const std::string discarded_id =
      token_after(discarded.output, "Working copy now at: ");
  ASSERT_EQ(invoke({"new", "-m", "still current", "main"}).code, 0);
  ASSERT_EQ(invoke({"abandon", discarded_id}).code, 0);
}

TEST_F(RepositoryTest, SquashesIntoPushedHistoryWithoutAWorkspace) {
  const git_oid destination = ref("refs/heads/main");
  const git_oid source = raw_commit("source", {destination});
  set_ref("refs/heads/side", source);
  set_ref("refs/remotes/origin/main", destination);

  const Result squashed = invoke({"squash", "-r", "side"});
  ASSERT_EQ(squashed.code, 0) << squashed.error;
  EXPECT_EQ(invoke({"workspace", "list"}).output, "No workspaces.\n");
  detail::Repository repo(path_);
  const git_oid rewritten = repo.resolve("side");
  const git_oid main = repo.resolve("main");
  EXPECT_EQ(git_oid_equal(&rewritten, &source), 0);
  EXPECT_NE(git_oid_equal(&rewritten, &main), 0);
}

TEST_F(RepositoryTest, AbandonCanRetainBookmarksAndDescendantContents) {
  const Result parent = invoke({"new", "-m", "parent", "main"});
  ASSERT_EQ(parent.code, 0) << parent.error;
  const std::string parent_id = token_after(parent.output, "Working copy now at: ");
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "kept"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  ASSERT_EQ(invoke({"abandon", "--retain-bookmarks",
                    "--restore-descendants", parent_id})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/kept"));
  EXPECT_EQ(invoke({"file", "show", "parent.txt"}).output, "parent\n");
  EXPECT_EQ(invoke({"file", "show", "child.txt"}).output, "child\n");
}

TEST_F(RepositoryTest, AbandonsRevisionSetsAndMergeChanges) {
  const git_oid base = ref("HEAD");
  const git_oid left = raw_commit("left", {base});
  const git_oid right = raw_commit("right", {base});
  const git_oid merge = raw_commit("merge", {left, right});
  const git_oid child = raw_commit("child", {merge});
  const git_oid grandchild = raw_commit("grandchild", {child});
  const git_oid duplicate = raw_commit("duplicate", {base, base});
  set_ref("refs/heads/side", grandchild);
  set_ref("refs/heads/duplicate", duplicate);
  set_ref("refs/heads/merge-bookmark", merge);
  set_ref("refs/tags/merge-tag", merge);

  const std::string merge_text = git_oid_tostr_s(&merge);
  const std::string left_text = git_oid_tostr_s(&left);
  const std::string right_text = git_oid_tostr_s(&right);
  const Result abandoned =
      invoke({"abandon", "-r", merge_text + " | " + left_text + " | " +
                             right_text});
  ASSERT_EQ(abandoned.code, 0) << abandoned.error;
  EXPECT_NE(abandoned.output.find("Abandoned 3 revision(s)."),
            std::string::npos) << abandoned.output;
  EXPECT_FALSE(has_ref("refs/heads/merge-bookmark"));
  const git_oid moved_tag = ref("refs/tags/merge-tag");
  EXPECT_NE(git_oid_equal(&moved_tag, &base), 0)
      << git_oid_tostr_s(&moved_tag) << " != " << git_oid_tostr_s(&base);
  const git_oid rewritten_grandchild = ref("refs/heads/side");
  detail::Repository repo(path_);
  const std::vector<git_oid> grandchild_parents =
      repo.parents(rewritten_grandchild);
  ASSERT_EQ(grandchild_parents.size(), 1U);
  EXPECT_EQ(repo.parents(grandchild_parents.front()).size(), 1U);

  ASSERT_EQ(invoke({"new", "-m", "work", "main",
                    git_oid_tostr_s(&right)})
                .code,
            0);
  ASSERT_EQ(invoke({"abandon", "@"}).code, 0);
  EXPECT_EQ(repo.parents(ref("refs/gg/workspaces/default")).size(), 2U);
  EXPECT_EQ(invoke({"abandon", "none()"}).output, "Nothing changed.\n");
}


}  // namespace gg::test
