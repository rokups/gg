// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"
#include "gg/gg.h"

#include <algorithm>

namespace gg::test {

using detail::operator==;

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

TEST_F(RepositoryTest, SquashHonorsAnExplicitlyEmptyDescription) {
  ASSERT_EQ(invoke({"new", "-m", "destination", "main"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "source"}).code, 0);
  const Result squash = invoke({"squash", "-m", ""});
  ASSERT_EQ(squash.code, 0) << squash.error;

  detail::Repository repo(path_);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  const std::vector<git_oid> parents = repo.parents(workspace);
  ASSERT_EQ(parents.size(), 1U);
  const char* message = git_commit_message(repo.commit(parents.front()).get());
  EXPECT_TRUE(message == nullptr || std::string_view(message).empty());
}

TEST_F(RepositoryTest, AbandonDoesNotReuseAnAbandonedCommitObject) {
  const Result first = invoke({"new", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const std::string first_id =
      token_after(first.output, "Working copy now at: ");
  detail::Repository before(path_);
  const git_oid first_oid = before.resolve(first_id);
  const std::vector<git_oid> original_parents = before.parents(first_oid);

  const Result child = invoke({"new"});
  ASSERT_EQ(child.code, 0) << child.error;
  ASSERT_EQ(invoke({"abandon", first_id}).code, 0);

  detail::Repository after(path_);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  EXPECT_EQ(git_oid_equal(&workspace, &first_oid), 0);
  const std::vector<git_oid> replacement_parents = after.parents(workspace);
  ASSERT_EQ(replacement_parents.size(), original_parents.size());
  for (std::size_t index = 0; index < replacement_parents.size(); ++index)
    EXPECT_NE(git_oid_equal(&replacement_parents[index], &original_parents[index]),
              0);
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

TEST_F(RepositoryTest, SquashesIntoASiblingAndRestacksSourceChildren) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "source", "main"}).code, 0);
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string source = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"bookmark", "create", "source-name"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string child = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "destination", "main"}).code, 0);
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string destination = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"edit", child}).code, 0);

  const Result result = invoke({"squash", "--from", source, "--into", destination});
  ASSERT_EQ(result.code, 0) << result.error;
  detail::Repository after(path_);
  const git_oid target = after.resolve(destination);
  const git_oid consumed = after.resolve(source);
  const git_oid named = after.resolve("source-name");
  EXPECT_TRUE(git_oid_equal(&target, &consumed));
  EXPECT_TRUE(git_oid_equal(&target, &named));
  EXPECT_TRUE(after.parents(target).front() == base);
  EXPECT_TRUE(after.parents(after.resolve(child)).front() == base);
  EXPECT_EQ(invoke({"file", "show", "-r", destination, "source.txt"}).output, "source\n");
  EXPECT_EQ(invoke({"file", "show", "-r", destination, "destination.txt"}).output, "destination\n");
  EXPECT_EQ(invoke({"file", "show", "child.txt"}).output, "child\n");
  EXPECT_EQ(invoke({"file", "show", "source.txt"}).code, 2);
  EXPECT_TRUE(ref("refs/heads/main") == base);
  expect_workspace_coherent();
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(detail::oid_string(ref("refs/gg/workspaces/default")), child);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "source.txt"}).code, 2);
}

TEST_F(RepositoryTest, SquashesIntoADescendantWithoutLosingItsContents) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "source", "main"}).code, 0);
  write("source.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string source = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "middle"}).code, 0);
  write("middle.txt", "middle\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string middle = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "target"}).code, 0);
  write("target.txt", "target\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid old_target = ref("refs/gg/workspaces/default");
  detail::Repository before(path_);
  const git_oid old_tree = *git_commit_tree_id(before.commit(old_target).get());
  const Result result = invoke({"squash", "--from", source, "--into", "@"});
  ASSERT_EQ(result.code, 0) << result.error;
  detail::Repository after(path_);
  const git_oid target = ref("refs/gg/workspaces/default");
  EXPECT_TRUE(*git_commit_tree_id(after.commit(target).get()) == old_tree);
  EXPECT_TRUE(after.parents(after.resolve(middle)).front() == base);
  EXPECT_TRUE(after.resolve(source) == target);
  EXPECT_TRUE(after.parents(target).front() == after.resolve(middle));
  EXPECT_EQ(invoke({"file", "show", "-r", middle, "source.txt"}).code, 2);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, SquashesAnEntireBranchAsOneOperationAndRestacksSideChildren) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "branch root", "main"}).code, 0);
  write("root.txt", "root\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string root = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "branch tip"}).code, 0);
  write("tip.txt", "tip\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string tip = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "side", root}).code, 0);
  write("side.txt", "side\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string side = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "destination", "main"}).code, 0);
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string destination = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"edit", tip}).code, 0);
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  gg_operation_array operations{};
  ASSERT_EQ(gg_repository_operations(&operations, api, 100), GIT_OK);
  const auto count = operations.count;
  gg_operation_array_dispose(&operations);
  gg_squash_options options = GG_SQUASH_OPTIONS_INIT;
  options.source = tip.c_str();
  options.destination = destination.c_str();
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_squash_ex(&mutation, api, &options, 1, nullptr), GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operations(&operations, api, 100), GIT_OK);
  EXPECT_EQ(operations.count, count + 1);
  gg_operation_array_dispose(&operations);
  gg_repository_free(api);
  detail::Repository after(path_);
  const git_oid target = after.resolve(destination);
  EXPECT_TRUE(after.resolve(root) == target);
  EXPECT_TRUE(after.resolve(tip) == target);
  EXPECT_TRUE(after.parents(target).front() == base);
  EXPECT_TRUE(after.parents(after.resolve(side)).front() == base);
  EXPECT_EQ(invoke({"file", "show", "root.txt"}).output, "root\n");
  EXPECT_EQ(invoke({"file", "show", "tip.txt"}).output, "tip\n");
  EXPECT_EQ(invoke({"file", "show", "destination.txt"}).output, "destination\n");
  EXPECT_EQ(invoke({"file", "show", "-r", side, "root.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "-r", side, "side.txt"}).output, "side\n");
  expect_workspace_coherent();
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(detail::oid_string(ref("refs/gg/workspaces/default")), tip);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "root.txt"}).output, "root\n");
}

TEST_F(RepositoryTest, RebaseToExistingParentPreservesCommitAndDescendantIds) {
  const git_oid base = ref("HEAD");
  detail::Repository repo(path_);
  git_signature* old_signature = nullptr;
  ASSERT_EQ(git_signature_new(&old_signature, "Old", "old@example.test", 1, 0), GIT_OK);
  const git_oid source = repo.create_commit(*git_commit_tree_id(repo.commit(base).get()),
                                            {base}, "source", old_signature, old_signature);
  git_signature_free(old_signature);
  const git_oid child = raw_commit("child", {source});
  set_ref("refs/heads/side", child);
  const Result result = invoke({"rebase", "-s", detail::oid_string(source), "-d", "main"});
  ASSERT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(result.output, "Nothing changed.\n");
  EXPECT_TRUE(ref("refs/heads/side") == child);
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
  EXPECT_NE(invoke({"workspace", "list"}).output.find("(unmanaged)"),
            std::string::npos);
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

TEST_F(RepositoryTest, SquashingIntoAChildPreservesOverlappingEdits) {
  ASSERT_EQ(invoke({"new", "-m", "source", "main"}).code, 0);
  write("tracked.txt", "one\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string source = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "target"}).code, 0);
  write("tracked.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result result = invoke({"squash", "--from", source, "--into", "@"});
  ASSERT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(file(), "two\n");
  detail::Repository repo(path_);
  EXPECT_FALSE(repo.commit_has_conflicts(*repo.workspace()));
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, SquashingConflictingSiblingsKeepsLogicalConflictTerms) {
  ASSERT_EQ(invoke({"new", "-m", "source", "main"}).code, 0);
  write("tracked.txt", "one\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const std::string source = detail::oid_string(ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"new", "-m", "target", "main"}).code, 0);
  write("tracked.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"squash", "--from", source, "--into", "@"}).code, 0);
  detail::Repository repo(path_);
  const auto conflicts = repo.tree_conflicts(*git_commit_tree_id(repo.commit(*repo.workspace()).get()));
  ASSERT_EQ(conflicts.size(), 1U);
  EXPECT_EQ(conflicts.at("tracked.txt").removes.size(), 1U);
  EXPECT_EQ(conflicts.at("tracked.txt").adds.size(), 2U);
  ASSERT_EQ(invoke({"duplicate"}).code, 0);
  detail::Repository duplicated(path_);
  const auto copied = duplicated.tree_conflicts(*git_commit_tree_id(duplicated.commit(*duplicated.workspace()).get()));
  ASSERT_EQ(copied.size(), 1U);
  EXPECT_EQ(copied.at("tracked.txt").removes.size(), 1U);
  EXPECT_EQ(copied.at("tracked.txt").adds.size(), 2U);
  EXPECT_EQ(invoke({"status"}).code, 0);
  detail::Repository snapshotted(path_);
  EXPECT_TRUE(snapshotted.commit_has_conflicts(*snapshotted.workspace()));
}

TEST_F(RepositoryTest, ReorderAfterTargetKeepsUnchangedPrefixIdentity) {
  const git_oid base = ref("HEAD");
  detail::Repository repo(path_);
  git_signature* old_signature = nullptr;
  ASSERT_EQ(git_signature_new(&old_signature, "Old", "old@example.test", 1, 0), GIT_OK);
  const git_oid first = repo.create_commit(*git_commit_tree_id(repo.commit(base).get()),
                                           {base}, "first", old_signature, old_signature);
  git_signature_free(old_signature);
  const git_oid second = raw_commit("second", {first});
  const git_oid third = raw_commit("third", {second});
  const git_oid side = raw_commit("side", {first});
  set_ref("refs/heads/side", side);
  set_ref("refs/heads/first", first);
  set_ref("refs/heads/stack", third);
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  gg_reorder_options options = GG_REORDER_OPTIONS_INIT;
  const std::string first_text = detail::oid_string(first);
  const std::string third_text = detail::oid_string(third);
  options.source = third_text.c_str();
  options.target = first_text.c_str();
  options.placement = GG_REORDER_AFTER;
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_reorder(&mutation, api, &options, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  gg_repository_free(api);
  detail::Repository after(path_);
  EXPECT_TRUE(ref("refs/heads/first") == first);
  EXPECT_TRUE(ref("refs/heads/side") == side);
  EXPECT_TRUE(after.parents(after.resolve(third_text)).front() == first);
  EXPECT_TRUE(after.parents(after.resolve(detail::oid_string(second))).front() == after.resolve(third_text));
}

TEST_F(RepositoryTest, ReorderCopyKeepsDistinctIdentityAndAppendedTipVisible) {
  const git_oid base = ref("HEAD");
  const git_oid first = raw_commit("first", {base});
  const git_oid second = raw_commit("second", {first});
  set_ref("refs/heads/stack", second);
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  const std::string first_text = detail::oid_string(first);
  const std::string second_text = detail::oid_string(second);
  gg_reorder_options options = GG_REORDER_OPTIONS_INIT;
  options.source = second_text.c_str();
  options.target = first_text.c_str();
  options.placement = GG_REORDER_AFTER;
  options.copy = 1;
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_reorder(&mutation, api, &options, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  {
    detail::Repository after(path_);
    const git_oid rewritten = after.resolve(second_text);
    const git_oid copied = after.parents(rewritten).front();
    EXPECT_FALSE(copied == second);
    EXPECT_FALSE(copied == rewritten);
    EXPECT_TRUE(after.parents(copied).front() == first);
  }
  ASSERT_EQ(gg_repository_undo(&mutation, api, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  options.source = first_text.c_str();
  options.target = second_text.c_str();
  ASSERT_EQ(gg_repository_reorder(&mutation, api, &options, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  {
    detail::Repository after(path_);
    EXPECT_TRUE(ref("refs/heads/stack") == second);
    const auto children = after.children(second);
    ASSERT_EQ(children.size(), 1U);
    EXPECT_FALSE(children.front() == first);
    EXPECT_EQ(after.parents(children.front()).size(), 1U);
  }
  gg_repository_free(api);
}

TEST_F(RepositoryTest, SplitsAnExplicitRevisionWithoutAWorkspace) {
  ASSERT_EQ(invoke({"new", "-m", "source", "main"}).code, 0);
  write("one.txt", "one\n");
  write("two.txt", "two\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid source = ref("refs/gg/workspaces/default");
  set_ref("refs/heads/source", source);
  ASSERT_EQ(git_reference_remove(repository_.get(), "refs/gg/workspaces/default"), GIT_OK);
  ASSERT_EQ(invoke_git({"checkout", "-f", "main"}).code, 0);
  const Result result = invoke({"split", "-r", "source", "one.txt"});
  ASSERT_EQ(result.code, 0) << result.error;
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  detail::Repository after(path_);
  const git_oid remainder = after.resolve("source");
  const git_oid selected = after.resolve(detail::oid_string(source));
  EXPECT_TRUE(after.parents(remainder).front() == selected);
  EXPECT_EQ(invoke({"file", "show", "-r", detail::oid_string(selected), "one.txt"}).output, "one\n");
  EXPECT_EQ(invoke({"file", "show", "-r", detail::oid_string(selected), "two.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "-r", "source", "two.txt"}).output, "two\n");
}

TEST_F(RepositoryTest, AbandonRejectsAnotherWorkspaceExactRevision) {
  const git_oid base = ref("HEAD");
  const git_oid source = raw_commit("other workspace", {base});
  set_ref("refs/heads/source", source);
  set_ref("refs/gg/workspaces/other", source);
  const Result result = invoke({"abandon", "source"});
  EXPECT_EQ(result.code, 2);
  EXPECT_NE(result.error.find("active workspace: other"), std::string::npos);
  EXPECT_TRUE(ref("refs/heads/source") == source);
  EXPECT_TRUE(ref("refs/gg/workspaces/other") == source);
}

TEST_F(RepositoryTest, RepeatedNewChangeCreatesDistinctSiblings) {
  const Result first = invoke({"new", "--no-edit", "-m", "identical", "main"});
  ASSERT_EQ(first.code, 0) << first.error;
  const Result second = invoke({"new", "--no-edit", "-m", "identical", "main"});
  ASSERT_EQ(second.code, 0) << second.error;
  const std::string first_id = token_after(first.output, "Created change: ");
  const std::string second_id = token_after(second.output, "Created change: ");
  EXPECT_NE(first_id, second_id);
  detail::Repository repo(path_);
  EXPECT_EQ(repo.children(ref("refs/heads/main")).size(), 2U);
}

TEST_F(RepositoryTest, UnchangedRewriteAndIdentityPlanPreserveHistory) {
  const git_oid base = ref("HEAD");
  detail::Repository repo(path_);
  git_signature* old_signature = nullptr;
  ASSERT_EQ(git_signature_new(&old_signature, "Old", "old@example.test", 1, 0), GIT_OK);
  const git_oid source = repo.create_commit(*git_commit_tree_id(repo.commit(base).get()),
                                            {base}, "source", old_signature, old_signature);
  git_signature_free(old_signature);
  const git_oid child = raw_commit("child", {source});
  set_ref("refs/heads/stack", child);
  EXPECT_TRUE(repo.rewrite_commit(source, {base}) == source);
  const auto plan = repo.descendants({{source, source}});
  EXPECT_TRUE(plan.commits.empty());
  EXPECT_TRUE(plan.updates.empty());
}

TEST_F(RepositoryTest, EditingAndSnapshottingARootKeepsItsIdentityAndTopology) {
  const git_oid root = ref("HEAD");
  ASSERT_EQ(invoke({"edit", detail::oid_string(root)}).code, 0);
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == root);
  write("tracked.txt", "edited root\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid rewritten = ref("refs/gg/workspaces/default");
  EXPECT_FALSE(rewritten == root);
  detail::Repository repo(path_);
  EXPECT_TRUE(repo.parents(rewritten).empty());
  EXPECT_TRUE(ref("HEAD") == rewritten);
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == rewritten);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == root);
  EXPECT_EQ(file(), "base\n");
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == rewritten);
  EXPECT_EQ(file(), "edited root\n");
}

TEST_F(RepositoryTest, ConsumingAnUnnamedEmptyWorkspaceCreatesAFreshReplacement) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid squashed = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const git_oid replacement = ref("refs/gg/workspaces/default");
  EXPECT_FALSE(replacement == squashed);
  detail::Repository after_squash(path_);
  EXPECT_TRUE(after_squash.resolve(detail::oid_string(squashed)) == ref("refs/heads/main"));
  ASSERT_EQ(invoke({"abandon"}).code, 0);
  EXPECT_FALSE(ref("refs/gg/workspaces/default") == replacement);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, CheckoutProtectsSnapshotExcludedFilesButAllowsUnrelatedOnes) {
  const git_oid base = ref("HEAD");
  write("local.txt", "target content\n");
  ASSERT_EQ(invoke_git({"add", "local.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(base)}).code, 0);
  ASSERT_EQ(invoke_git({"config", "snapshot.max-new-file-size", "1"}).code, 0);
  write("local.txt", "precious untracked content\n");
  write("unrelated.txt", "unrelated local content\n");
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  const Result refused = invoke({"edit", detail::oid_string(target)});
  EXPECT_NE(refused.code, 0);
  EXPECT_NE(refused.error.find("overwrite untracked or ignored path: local.txt"), std::string::npos);
  EXPECT_EQ(read_path(path_ / "local.txt"), "precious untracked content\n");
  EXPECT_EQ(read_path(path_ / "unrelated.txt"), "unrelated local content\n");
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  EXPECT_TRUE(ref("HEAD") == base);
  detail::Repository after(path_);
  EXPECT_TRUE(*after.operation() == operation);
  std::filesystem::remove(path_ / "local.txt");
  ASSERT_EQ(invoke({"edit", detail::oid_string(target)}).code, 0);
  EXPECT_EQ(read_path(path_ / "local.txt"), "target content\n");
  EXPECT_EQ(read_path(path_ / "unrelated.txt"), "unrelated local content\n");
}

TEST_F(RepositoryTest, CheckoutProtectsUntrackedChildrenFromDirectoryReplacement) {
  const git_oid base = ref("HEAD");
  write("container", "target file\n");
  ASSERT_EQ(invoke_git({"add", "container"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(base)}).code, 0);
  ASSERT_EQ(invoke_git({"config", "snapshot.max-new-file-size", "1"}).code, 0);
  write("container/local.txt", "precious local content\n");
  const Result refused = invoke({"edit", detail::oid_string(target)});
  EXPECT_NE(refused.code, 0);
  EXPECT_EQ(read_path(path_ / "container/local.txt"), "precious local content\n");
  EXPECT_TRUE(ref("HEAD") == base);
}

TEST_F(RepositoryTest, FailedRootSnapshotPreservesDirtyFilesAndPreviousGraph) {
  const git_oid root = ref("HEAD");
  ASSERT_EQ(invoke({"edit", detail::oid_string(root)}).code, 0);
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  const auto refs = before.data_refs();
  write("tracked.txt", "dirty root edits\n");
  write(".git/HEAD.lock", "locked\n");
  const Result refused = invoke({"status"});
  EXPECT_NE(refused.code, 0);
  EXPECT_EQ(refused.error.find("could not restore"), std::string::npos) << refused.error;
  EXPECT_TRUE(ref("HEAD") == root);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == root);
  EXPECT_EQ(file(), "dirty root edits\n");
  detail::Repository after(path_);
  EXPECT_TRUE(*after.operation() == operation);
  const auto restored = after.data_refs();
  ASSERT_EQ(restored.size(), refs.size());
  for (const auto& [name, target] : refs) {
    ASSERT_TRUE(restored.contains(name));
    EXPECT_TRUE(restored.at(name) == target) << name;
  }
  std::filesystem::remove(path_ / ".git/HEAD.lock");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(file(), "dirty root edits\n");
  EXPECT_FALSE(ref("refs/gg/workspaces/default") == root);
}

TEST_F(RepositoryTest, UndoRedoAndHistoricalEditsDoNotLockUnchangedReferences) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke({"new", "-m", "child", "main"}).code, 0);
  const git_oid child = ref("refs/gg/workspaces/default");
  write(".git/refs/heads/main.lock", "locked\n");
  ASSERT_EQ(invoke({"undo"}).code, 0);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_TRUE(ref("refs/heads/main") == base);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == child);
  ASSERT_EQ(invoke({"new", "-m", "unrelated", "main"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  write(".git/refs/gg/workspaces/default.lock", "locked\n");
  const Result described = invoke({"describe", "-m", "changed child", detail::oid_string(child)});
  ASSERT_EQ(described.code, 0) << described.error;
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == workspace);
  EXPECT_TRUE(ref("refs/heads/main") == base);
}

TEST_F(RepositoryTest, RebaseWithoutWorkspaceKeepsAttachedAndDetachedCheckoutsCoherent) {
  const git_oid base = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "-b", "destination"}).code, 0);
  write("destination.txt", "destination\n");
  ASSERT_EQ(invoke_git({"add", "destination.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "destination"}).code, 0);
  ASSERT_EQ(invoke_git({"checkout", "main"}).code, 0);
  write("source.txt", "source\n");
  ASSERT_EQ(invoke_git({"add", "source.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "source"}).code, 0);
  const git_oid source = ref("HEAD");
  for (const bool detached : {false, true}) {
    SCOPED_TRACE(detached);
    if (detached) {
      ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(source)}).code, 0);
    }
    const Result rebased = invoke({"rebase", "-s", detail::oid_string(source), "-d", "destination"});
    ASSERT_EQ(rebased.code, 0) << rebased.error;
    const git_oid rewritten = ref("HEAD");
    EXPECT_FALSE(rewritten == source);
    EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
    EXPECT_EQ(read_path(path_ / "source.txt"), "source\n");
    EXPECT_EQ(read_path(path_ / "destination.txt"), "destination\n");
    ASSERT_EQ(invoke({"status"}).code, 0);
    EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
    ASSERT_EQ(invoke({"undo"}).code, 0);
    EXPECT_TRUE(ref("HEAD") == source);
    EXPECT_FALSE(std::filesystem::exists(path_ / "destination.txt"));
    ASSERT_EQ(invoke({"redo"}).code, 0);
    EXPECT_TRUE(ref("HEAD") == rewritten);
    EXPECT_EQ(read_path(path_ / "destination.txt"), "destination\n");
    ASSERT_EQ(invoke({"undo"}).code, 0);
    EXPECT_TRUE(ref("HEAD") == source);
  }
  EXPECT_TRUE(detail::Repository(path_).parents(source).front() == base);
}

TEST_F(RepositoryTest, MetadataRewriteAndAbandonFollowDetachedHeadWithoutWorkspace) {
  const git_oid base = ref("HEAD");
  write("tracked.txt", "child\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "child"}).code, 0);
  const git_oid child = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(child)}).code, 0);
  ASSERT_EQ(invoke({"describe", "-m", "described", detail::oid_string(child)}).code, 0);
  const git_oid described = ref("HEAD");
  EXPECT_FALSE(described == child);
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"abandon", detail::oid_string(described)}).code, 0);
  EXPECT_TRUE(ref("HEAD") == base);
  EXPECT_EQ(file(), "base\n");
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(ref("HEAD") == described);
  EXPECT_EQ(file(), "child\n");
}

TEST_F(RepositoryTest, ImportingDirtyAttachedHeadPreservesUndoLineage) {
  const git_oid base = ref("HEAD");
  write("tracked.txt", "dirty imported content\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid imported = ref("refs/gg/workspaces/default");
  detail::Repository repo(path_);
  EXPECT_FALSE(repo.head_state().symbolic);
  EXPECT_TRUE(ref("HEAD") == base);
  const git_oid operation = *repo.operation();
  ASSERT_EQ(invoke({"status"}).code, 0);
  detail::Repository refreshed(path_);
  EXPECT_TRUE(*refreshed.operation() == operation);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  EXPECT_EQ(file(), "base\n");
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_TRUE(ref("refs/gg/workspaces/default") == imported);
  EXPECT_EQ(file(), "dirty imported content\n");
}

TEST_F(RepositoryTest, FailedDirtyImportPreservesWorkingFilesAndAttachedHead) {
  const git_oid base = ref("HEAD");
  detail::Repository repo(path_);
  const git_oid operation = repo.ensure_operation();
  write("tracked.txt", "dirty imported content\n");
  write(".git/HEAD.lock", "locked\n");
  const Result refused = invoke({"status"});
  EXPECT_NE(refused.code, 0);
  EXPECT_EQ(refused.error.find("could not restore"), std::string::npos) << refused.error;
  EXPECT_FALSE(has_ref("refs/gg/workspaces/default"));
  EXPECT_TRUE(ref("HEAD") == base);
  EXPECT_EQ(file(), "dirty imported content\n");
  detail::Repository after(path_);
  EXPECT_TRUE(after.head_state().symbolic);
  EXPECT_TRUE(*after.operation() == operation);
  std::filesystem::remove(path_ / ".git/HEAD.lock");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(file(), "dirty imported content\n");
}

TEST_F(RepositoryTest, FailedRecoveryDoesNotExemptLocalFilesInTheNextOperation) {
  const git_oid base = ref("HEAD");
  write("tracked.txt", "target content\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"checkout", "--detach", detail::oid_string(base)}).code, 0);
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  write(".git/index.lock", "locked\n");
  gg_mutation_result mutation{};
  EXPECT_NE(gg_repository_edit(&mutation, api, detail::oid_string(target).c_str(), nullptr), GIT_OK);
  ASSERT_NE(git_error_last(), nullptr);
  EXPECT_NE(std::string(git_error_last()->message).find("could not restore"), std::string::npos);
  gg_mutation_result_dispose(&mutation);
  std::filesystem::remove(path_ / ".git/index.lock");

  ASSERT_EQ(invoke_git({"rm", "--cached", "tracked.txt"}).code, 0);
  write("tracked.txt", "precious newly untracked content\n");
  EXPECT_NE(gg_repository_edit(&mutation, api, detail::oid_string(base).c_str(), nullptr), GIT_OK);
  ASSERT_NE(git_error_last(), nullptr);
  EXPECT_NE(std::string(git_error_last()->message).find("overwrite untracked or ignored"), std::string::npos);
  EXPECT_EQ(file(), "precious newly untracked content\n");
  gg_mutation_result_dispose(&mutation);
  gg_repository_free(api);
}


}  // namespace gg::test
