// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {
namespace {

unsigned int parent_count(git_repository* repository, const git_oid& oid) {
  git_commit* commit = nullptr;
  if (git_commit_lookup(&commit, repository, &oid) != 0) {
    throw std::runtime_error("commit not found");
  }
  const unsigned int result = git_commit_parentcount(commit);
  git_commit_free(commit);
  return result;
}

std::string oid_text(const git_oid& oid) { return git_oid_tostr_s(&oid); }

}  // namespace

TEST_F(RepositoryTest, SimplifiesSelectedParentsAndPreservesContents) {
  ASSERT_EQ(invoke({"new", "-m", "a", "main"}).code, 0);
  const git_oid a = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "b"}).code, 0);
  const git_oid b = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "c", oid_text(a), oid_text(b)})
                .code,
            0);
  const git_oid c = ref("refs/gg/workspaces/default");
  ASSERT_EQ(parent_count(repository_.get(), c), 2U);

  const Result simplified = invoke({"simplify-parents", "-r", "@"});
  ASSERT_EQ(simplified.code, 0) << simplified.error;
  EXPECT_EQ(simplified.output,
            "Removed 1 edges from 1 out of 1 commits.\n");
  const git_oid rewritten = ref("refs/gg/workspaces/default");
  EXPECT_EQ(parent_count(repository_.get(), rewritten), 1U);
  const git_oid rewritten_parent = commit_parent(rewritten);
  EXPECT_NE(git_oid_equal(&rewritten_parent, &b), 0);
  EXPECT_EQ(file(), "base\n");
  EXPECT_EQ(invoke({"undo"}).code, 0);
  const git_oid restored = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&restored, &c), 0);
}

TEST_F(RepositoryTest, SourceSimplifiesDescendantsAndReparentsTheirChildren) {
  const git_oid base = ref("HEAD");
  const git_oid a = raw_commit("a", {base});
  const git_oid b = raw_commit("b", {a});
  const git_oid c = raw_commit("c", {a, b});
  const git_oid d = raw_commit("d", {c});
  const git_oid e = raw_commit("e", {d});
  const git_oid f = raw_commit("f", {d, e});
  set_ref("refs/heads/stack", f);
  ASSERT_EQ(invoke({"new", "-m", "work", "stack"}).code, 0);

  const Result simplified =
      invoke({"simplify-parents", "--source", oid_text(c)});
  ASSERT_EQ(simplified.code, 0) << simplified.error;
  EXPECT_EQ(simplified.output,
            "Removed 2 edges from 2 out of 5 commits.\n"
            "Rebased 3 descendant commits.\n");
  const git_oid rewritten_f = ref("refs/heads/stack");
  EXPECT_EQ(parent_count(repository_.get(), rewritten_f), 1U);
  EXPECT_EQ(file(), "base\n");
}

TEST_F(RepositoryTest, DefaultsToReachableParentsAndHandlesNoChanges) {
  ASSERT_EQ(invoke({"simplify-parents"}).code, 2);
  ASSERT_EQ(invoke({"new", "-m", "a", "main"}).code, 0);
  const git_oid a = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "b"}).code, 0);
  const git_oid b = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"new", "-m", "c", oid_text(a), oid_text(b)}).code, 0);
  EXPECT_EQ(invoke({"simplify-parents"}).output,
            "Removed 1 edges from 1 out of 4 commits.\n");
  EXPECT_EQ(invoke({"simplify-parents", "--revisions", "@",
                    "--revision", "@-"})
                .output,
            "Nothing changed.\n");
  EXPECT_EQ(invoke({"simplify-parents"}).output, "Nothing changed.\n");
}

TEST_F(RepositoryTest, SimplifiesAnUnrelatedStackAndDuplicateParents) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const git_oid workspace = ref("refs/gg/workspaces/default");
  const git_oid base = ref("HEAD");
  const git_oid a = raw_commit("a", {base});
  const git_oid b = raw_commit("b", {a});
  const git_oid merge = raw_commit("merge", {a, a, b});
  const git_oid child = raw_commit("child", {merge});
  set_ref("refs/heads/side", child);

  const Result simplified =
      invoke({"simplify-parents", "--revision", oid_text(merge)});
  ASSERT_EQ(simplified.code, 0) << simplified.error;
  EXPECT_EQ(simplified.output,
            "Removed 2 edges from 1 out of 1 commits.\n"
            "Rebased 1 descendant commits.\n");
  EXPECT_EQ(parent_count(repository_.get(),
                         commit_parent(ref("refs/heads/side"))),
            1U);
  const git_oid unchanged_workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&unchanged_workspace, &workspace), 0);

  const git_oid left = raw_commit("left", {base});
  const git_oid right = raw_commit("right", {base});
  const git_oid diamond = raw_commit("diamond", {left, right});
  set_ref("refs/heads/diamond", diamond);
  EXPECT_EQ(invoke({"simplify-parents", "-r", oid_text(diamond)}).output,
            "Nothing changed.\n");
}

}  // namespace gg::test
