// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, RestoresAllOrSelectedPaths) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("tracked.txt", "changed\n");
  write("extra.txt", "extra\n");

  ASSERT_EQ(invoke({"restore", "tracked.txt"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "tracked.txt"}).output, "base\n");
  EXPECT_EQ(invoke({"file", "show", "extra.txt"}).output, "extra\n");
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find("Description: work"),
            std::string::npos);
  EXPECT_EQ(invoke({"restore", "missing"}).output, "Nothing changed.\n");

  ASSERT_EQ(invoke({"restore", "."}).code, 0);
  EXPECT_NE(invoke({"status"}).output.find("no changes"), std::string::npos);
}

TEST_F(RepositoryTest, RestoresBetweenExplicitRevisions) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const git_oid side = raw_commit("side");
  set_ref("refs/heads/side", side);
  ASSERT_EQ(invoke({"restore", "--from", "@", "--into", "side"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "side", "tracked.txt"}).output,
            "source\n");

  ASSERT_EQ(invoke({"restore", "--from", "@", "--into", "main"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "main", "tracked.txt"}).output,
            "source\n");
  EXPECT_EQ(invoke({"restore", "--from", "@"}).output,
            "Nothing changed.\n");
  EXPECT_EQ(invoke({"restore", "--to", "@"}).output, "Nothing changed.\n");

  ASSERT_EQ(invoke({"restore", "--changes-in", "main"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "main", "tracked.txt"}).code, 2);
}

TEST_F(RepositoryTest, CanPreserveDescendantContentsWhileRestacking) {
  ASSERT_EQ(invoke({"new", "-m", "parent", "main"}).code, 0);
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "child"}).code, 0);
  ASSERT_EQ(invoke({"edit", "@-"}).code, 0);

  ASSERT_EQ(invoke({"restore", "--from", "main", "--restore-descendants"})
                .code,
            0);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "parent.txt"}).output,
            "parent\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "child.txt"}).output,
            "child\n");
}

TEST_F(RepositoryTest, RebasesDescendantChangesByDefaultWhenRestoring) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"new"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "child"}).code, 0);
  ASSERT_EQ(invoke({"edit", "@-"}).code, 0);

  ASSERT_EQ(invoke({"restore", "--from", "main"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "parent.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "child.txt"}).output,
            "child\n");
}

TEST_F(RepositoryTest, ValidatesRestoreRequests) {
  EXPECT_EQ(invoke({"restore", "--from", "main"}).code, 2);
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"restore", "--changes-in", "@", "--from", "main"})
                .code,
            2);
  EXPECT_EQ(invoke({"restore", "--interactive"}).code, 2);
  EXPECT_EQ(invoke({"restore", "--tool", "meld"}).code, 2);
  EXPECT_EQ(invoke({"restore", ""}).code, 2);
  EXPECT_EQ(invoke({"restore", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"restore", "../outside"}).code, 2);
}

}  // namespace gg::test
