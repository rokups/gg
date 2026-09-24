// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, RestoresAllOrSelectedPaths) {
  ASSERT_EQ(invoke({"new", "-m", "work", main_id()}).code, 0);
  const git_oid work = ref("refs/gg/workspaces/default");
  write("tracked.txt", "changed\n");
  write("extra.txt", "extra\n");

  const Result restored = invoke({"restore", "tracked.txt"});
  ASSERT_EQ(restored.code, 0) << restored.error;
  const std::string saved = token_after(restored.output, "saved them as ");
  ASSERT_FALSE(saved.empty());
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "base\n");
  EXPECT_EQ(read_path(path_ / "extra.txt"), "extra\n");
  EXPECT_EQ(invoke({"file", "show", "-r", saved.substr(0, 40), "tracked.txt"})
                .output,
            "changed\n");
  const git_oid unchanged = ref("refs/gg/workspaces/default");
  EXPECT_NE(git_oid_equal(&work, &unchanged), 0);
  EXPECT_EQ(invoke({"restore", "missing"}).output, "Nothing changed.\n");

  ASSERT_EQ(invoke({"restore", "."}).code, 0);
  EXPECT_FALSE(std::filesystem::exists(path_ / "extra.txt"));
  EXPECT_NE(invoke({"status"}).output.find("no changes"), std::string::npos);
  expect_workspace_coherent();
}

TEST_F(RepositoryTest, RestoresBetweenExplicitRevisions) {
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);

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
  ASSERT_EQ(invoke({"new", "-m", "parent", main_id()}).code, 0);
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "child"}).code, 0);
  ASSERT_EQ(invoke({"edit", "@-"}).code, 0);

  ASSERT_EQ(invoke({"restore", "--from", "main", "--restore-descendants"})
                .code,
            0);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "parent.txt"}).output,
            "parent\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "child.txt"}).output,
            "child\n");
}

TEST_F(RepositoryTest, SelectsRestoreChangesWithTheBuiltinEditor) {
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  write("tracked.txt", "changed\n");
  write("extra.txt", "extra\n");
  const std::filesystem::path selector = path_ / "restore-selector";
  {
    std::ofstream script(selector);
    script << "#!/bin/sh\nsed -i '/tracked.txt/d' \"$1\"\n";
  }
  std::filesystem::permissions(
      selector, std::filesystem::perms::owner_exec |
                    std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write);
  const char* old_visual = std::getenv("VISUAL");
  const std::string saved_visual = old_visual == nullptr ? "" : old_visual;
  ASSERT_EQ(setenv("VISUAL", selector.string().c_str(), 1), 0);
  const Result restored = invoke({"restore", "--interactive"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(restored.code, 0) << restored.error;
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "changed\n");
  EXPECT_FALSE(std::filesystem::exists(path_ / "extra.txt"));

  ASSERT_EQ(setenv("VISUAL", "/bin/true", 1), 0);
  const Result selected_path =
      invoke({"restore", "tracked.txt", "--interactive"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(selected_path.code, 0) << selected_path.error;
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "base\n");

  write("tracked.txt", "again\n");
  const Result external =
      invoke({"restore", "tracked.txt", "--tool", "/bin/true"});
  ASSERT_EQ(external.code, 0) << external.error;
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "base\n");
  EXPECT_EQ(invoke({"restore", "missing", "--interactive"}).output,
            "Nothing changed.\n");
}

TEST_F(RepositoryTest, RebasesDescendantChangesByDefaultWhenRestoring) {
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  write("parent.txt", "parent\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  ASSERT_EQ(invoke({"new"}).code, 0);
  write("child.txt", "child\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "child"}).code, 0);
  ASSERT_EQ(invoke({"edit", "@-"}).code, 0);

  ASSERT_EQ(invoke({"restore", "--from", "main"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "parent.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "-r", "child", "child.txt"}).output,
            "child\n");
}

TEST_F(RepositoryTest, ValidatesRestoreRequests) {
  EXPECT_EQ(invoke({"restore", "--from", "main"}).output, "Nothing changed.\n");
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  EXPECT_EQ(invoke({"restore", "--changes-in", "@", "--from", "main"})
                .code,
            2);
  EXPECT_EQ(invoke({"restore", "--tool", "missing-gg-diff-editor"}).code, 2);
  EXPECT_EQ(invoke({"restore", ""}).code, 2);
  EXPECT_EQ(invoke({"restore", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"restore", "../outside"}).code, 2);
}

}  // namespace gg::test
