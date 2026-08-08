// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, SetsListsMovesAndDeletesTags) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "v1", "stable"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "base", "-r", "main"}).code, 0);
  const std::string listed = invoke({"tag", "list"}).output;
  EXPECT_NE(listed.find("stable: "), std::string::npos);
  EXPECT_NE(listed.find("v1: "), std::string::npos);
  const Result colored =
      invoke({"--color", "debug", "tag", "list", "v1"});
  EXPECT_NE(colored.output.find("<<tag::v1>>"), std::string::npos);
  EXPECT_NE(colored.output.find("<<commit_id::"), std::string::npos);
  EXPECT_NE(invoke({"tag", "list", "v1"}).output.find("v1: "),
            std::string::npos);
  EXPECT_EQ(invoke({"tag", "list", "missing"}).output, "");
  EXPECT_NE(invoke({"tag", "list", "-r", "main"}).output.find("base: "),
            std::string::npos);
  EXPECT_NE(invoke({"tag", "list", "-r", "@"}).output.find("v1: "),
            std::string::npos);
  const Result unioned = invoke({"tag", "list", "base", "-r", "@"});
  EXPECT_NE(unioned.output.find("base: "), std::string::npos);
  EXPECT_NE(unioned.output.find("stable: "), std::string::npos);
  EXPECT_NE(unioned.output.find("v1: "), std::string::npos);
  EXPECT_EQ(invoke({"tag", "list", "-r", "main", "-r", "@"}).code, 0);

  const Result descending = invoke({"tag", "list", "--sort", "name-"});
  EXPECT_LT(descending.output.find("v1: "),
            descending.output.find("stable: "));
  const std::string all_sort_keys =
      "name,name-,author-name,author-name-,author-email,author-email-,"
      "author-date,author-date-,committer-name,committer-name-,"
      "committer-email,committer-email-,committer-date,committer-date-";
  EXPECT_EQ(invoke({"tag", "list", "--sort", all_sort_keys}).code, 0);

  const git_oid base = ref("refs/tags/base");
  const git_oid current = ref("refs/tags/v1");
  set_ref("refs/gg/remotes/origin/tags/v1", current);
  set_ref("refs/gg/remotes/backup/tags/base", base);
  EXPECT_EQ(invoke({"tag", "list"}).output.find("@origin"),
            std::string::npos);
  const Result all = invoke({"tag", "list", "--all-remotes"});
  EXPECT_NE(all.output.find("v1:"), std::string::npos);
  EXPECT_NE(all.output.find("v1@origin:"), std::string::npos);
  EXPECT_NE(all.output.find("base@backup:"), std::string::npos);
  const Result origin = invoke({"tag", "list", "--remote", "origin"});
  EXPECT_NE(origin.output.find("v1@origin:"), std::string::npos);
  EXPECT_EQ(origin.output.find("base@backup:"), std::string::npos);
  EXPECT_EQ(origin.output.find("v1:"), std::string::npos);
  const Result tracked = invoke({"tag", "list", "--tracked"});
  EXPECT_NE(tracked.output.find("v1@origin:"), std::string::npos);
  EXPECT_NE(tracked.output.find("base@backup:"), std::string::npos);
  EXPECT_EQ(tracked.output.find("v1:"), std::string::npos);
  const Result tracked_origin =
      invoke({"tag", "list", "--tracked", "--remote", "origin"});
  EXPECT_NE(tracked_origin.output.find("v1@origin:"), std::string::npos);
  EXPECT_EQ(tracked_origin.output.find("base@backup:"), std::string::npos);

  ASSERT_EQ(invoke({"tag", "set", "v1"}).code, 0);
  ASSERT_EQ(invoke({"new"}).code, 0);
  EXPECT_EQ(invoke({"tag", "set", "v1"}).code, 2);
  EXPECT_EQ(invoke({"tag", "set", "v1", "--allow-move"}).code, 0);
  const git_oid tag = ref("refs/tags/v1");
  const git_oid workspace = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&tag, &workspace), 0);

  EXPECT_EQ(invoke({"tag", "delete", "missing"}).code, 2);
  ASSERT_EQ(invoke({"tag", "delete", "v1", "stable"}).code, 0);
  EXPECT_FALSE(has_ref("refs/tags/v1"));
  EXPECT_FALSE(has_ref("refs/tags/stable"));
}

TEST_F(RepositoryTest, RestoresTagsThroughOperationHistory) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "release"}).code, 0);
  EXPECT_TRUE(has_ref("refs/tags/release"));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_FALSE(has_ref("refs/tags/release"));
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_TRUE(has_ref("refs/tags/release"));
}

TEST_F(RepositoryTest, ValidatesTagRequestsAndUnsupportedFilters) {
  EXPECT_EQ(invoke({"tag"}).code, 2);
  EXPECT_EQ(invoke({"tag", "set"}).code, 2);
  EXPECT_EQ(invoke({"tag", "delete"}).code, 2);
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"tag", "set", "valid", "bad name"}).code, 2);
  EXPECT_FALSE(has_ref("refs/tags/valid"));
  EXPECT_EQ(invoke({"tag", "list", "--all-remotes"}).code, 0);
  EXPECT_EQ(invoke({"tag", "list", "--remote", "origin"}).code, 0);
  EXPECT_EQ(invoke({"tag", "list", "--tracked"}).code, 0);
  const Result conflicted = invoke({"tag", "list", "--conflicted"});
  EXPECT_EQ(conflicted.code, 0);
  EXPECT_TRUE(conflicted.output.empty());
  EXPECT_EQ(invoke({"tag", "list", "-T", "name"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--sort", "unknown"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--all-remotes", "--remote", "origin"})
                .code,
            2);
  EXPECT_EQ(invoke({"tag", "list", "--all-remotes", "--tracked"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--all-remotes", "--conflicted"})
                .code,
            2);
}

}  // namespace gg::test
