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
  EXPECT_NE(invoke({"tag", "list", "v1"}).output.find("v1: "),
            std::string::npos);
  EXPECT_EQ(invoke({"tag", "list", "missing"}).output, "");
  EXPECT_NE(invoke({"tag", "list", "-r", "main"}).output.find("base: "),
            std::string::npos);
  EXPECT_NE(invoke({"tag", "list", "-r", "@"}).output.find("v1: "),
            std::string::npos);

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
  EXPECT_EQ(invoke({"tag", "list", "--all-remotes"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--remote", "origin"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--tracked"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--conflicted"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "-T", "name"}).code, 2);
  EXPECT_EQ(invoke({"tag", "list", "--sort", "name"}).code, 2);
}

}  // namespace gg::test
