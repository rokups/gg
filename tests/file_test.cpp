// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, ListsAndShowsFilesFromRevisionTrees) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "working\n");
  write("nested/empty.txt", "");
  write("nested/tool.txt", "alpha\nbeta alpha\nfinal");
  std::filesystem::create_symlink("tool.txt", path_ / "nested/link");
  std::filesystem::permissions(
      path_ / "nested/tool.txt", std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add);

  const std::string all =
      "nested/empty.txt\nnested/link\nnested/tool.txt\ntracked.txt\n";
  EXPECT_EQ(invoke({"file", "list"}).output, all);
  EXPECT_EQ(invoke({"file", "list", "."}).output, all);
  EXPECT_EQ(invoke({"file", "list", "nested"}).output,
            "nested/empty.txt\nnested/link\nnested/tool.txt\n");
  EXPECT_EQ(invoke({"file", "list", "nested/to"}).output, "");
  EXPECT_EQ(invoke({"file", "list", "missing"}).output, "");
  EXPECT_EQ(invoke({"file", "list", "-r", "main"}).output,
            "tracked.txt\n");
  EXPECT_EQ(
      invoke({"file", "list", "nested", "-T",
              "path ++ \" \" ++ file_type ++ \" \" ++ executable ++ \" \" "
              "++ conflict ++ \" \" ++ conflict_side_count ++ \"\\n\""})
          .output,
      "nested/empty.txt file false false 1\n"
      "nested/link symlink false false 1\n"
      "nested/tool.txt file true false 1\n");

  EXPECT_EQ(invoke({"file", "show", "tracked.txt"}).output, "working\n");
  EXPECT_EQ(invoke({"file", "show", "-T", "path ++ ':'", "tracked.txt"})
                .output,
            "tracked.txt:working\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "main", "tracked.txt"}).output,
            "base\n");
  EXPECT_EQ(invoke({"file", "show", "nested/empty.txt"}).output, "");
  EXPECT_EQ(invoke({"file", "show", "nested/tool.txt"}).output,
            "alpha\nbeta alpha\nfinal");
  EXPECT_EQ(invoke({"file", "show", "missing"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "nested/link"}).code, 2);
  EXPECT_EQ(invoke({"file", "list", ""}).code, 2);
  EXPECT_EQ(invoke({"file", "list", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"file", "list", "../outside"}).code, 2);
  EXPECT_EQ(invoke({"file", "list", "-T", "unknown"}).code, 2);
  EXPECT_EQ(
      invoke({"file", "show", "-T", "unknown", "tracked.txt"}).code, 2);
}

TEST_F(RepositoryTest, SearchesRevisionFileContents) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("nested/tool.txt", "alpha\nbeta alpha\nfinal");
  write("nested/empty.txt", "");
  std::filesystem::create_symlink("tool.txt", path_ / "nested/link");

  EXPECT_EQ(invoke({"file", "search", "-p", "alpha"}).output,
            "nested/tool.txt:alpha\nnested/tool.txt:beta alpha\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "alpha", "-n"}).output,
            "nested/tool.txt:1:alpha\nnested/tool.txt:2:beta alpha\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "exact:alpha"}).output,
            "nested/tool.txt:alpha\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "substring:eta"}).output,
            "nested/tool.txt:beta alpha\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "glob:*final"}).output,
            "nested/tool.txt:final\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "glob:f?nal"}).output,
            "nested/tool.txt:final\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "exact:a.b"}).output, "");
  EXPECT_EQ(
      invoke({"file", "search", "-p", "alpha", "--name-only"}).output,
      "nested/tool.txt\n");
  EXPECT_EQ(invoke({"file", "search", "-p", "alpha", "tracked.txt"})
                .output,
            "");
  EXPECT_EQ(invoke({"file", "search", "-p", "alpha", "missing"}).output,
            "");
  EXPECT_EQ(invoke({"file", "search", "-p", "["}).code, 2);
  EXPECT_EQ(invoke({"file", "search", "-p", "unknown:value"}).code, 2);
}

TEST_F(RepositoryTest, ChangesExecutableBitsAndRestacksDescendants) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tool.sh", "#!/bin/sh\n");
  ASSERT_EQ(invoke({"file", "list"}).code, 0);
  const git_oid side = raw_commit("side");
  set_ref("refs/heads/side", side);

  EXPECT_EQ(invoke({"file", "chmod", "x", "tool.sh"}).code, 0);
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tool.sh"),
            GIT_FILEMODE_BLOB_EXECUTABLE);
  EXPECT_EQ(invoke({"file", "chmod", "executable", "tool.sh"}).output,
            "Nothing changed.\n");
  EXPECT_EQ(invoke({"file", "chmod", "normal", "tool.sh"}).code, 0);
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tool.sh"),
            GIT_FILEMODE_BLOB);
  EXPECT_EQ(invoke({"file", "chmod", "n", "tool.sh"}).output,
            "Nothing changed.\n");

  EXPECT_EQ(invoke({"file", "chmod", "x", "-r", "side", "tracked.txt"})
                .code,
            0);
  EXPECT_EQ(file_mode("refs/heads/side", "tracked.txt"),
            GIT_FILEMODE_BLOB_EXECUTABLE);
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tracked.txt"),
            GIT_FILEMODE_BLOB);

  EXPECT_EQ(invoke({"file", "chmod", "x", "-r", "main", "tracked.txt"})
                .code,
            0);
  EXPECT_EQ(file_mode("refs/heads/main", "tracked.txt"),
            GIT_FILEMODE_BLOB_EXECUTABLE);
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tracked.txt"),
            GIT_FILEMODE_BLOB_EXECUTABLE);
  EXPECT_EQ(invoke({"file", "chmod", "x", "missing"}).code, 2);

  std::filesystem::create_symlink("tool.sh", path_ / "link");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(invoke({"file", "chmod", "x", "link"}).code, 2);
}

TEST_F(RepositoryTest, PersistsFileTrackingOverrides) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("nested/one.txt", "one\n");
  write("nested/two.txt", "two\n");
  write("nest", "prefix\n");
  write("abc", "different\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  EXPECT_EQ(invoke({"file", "untrack", "nested"}).code, 0);
  EXPECT_EQ(invoke({"file", "list", "nested"}).output, "");

  EXPECT_EQ(invoke({"file", "track", "nest"}).code, 0);
  EXPECT_EQ(invoke({"file", "track", "abc"}).code, 0);
  EXPECT_TRUE(std::filesystem::exists(path_ / "nested/one.txt"));
  write("nested/one.txt", "changed\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(invoke({"file", "list", "nested"}).output, "");

  EXPECT_EQ(invoke({"file", "track", "nested/one.txt"}).code, 0);
  EXPECT_EQ(invoke({"file", "list", "nested"}).output,
            "nested/one.txt\n");
  EXPECT_EQ(invoke({"file", "track", "nested"}).code, 0);
  EXPECT_EQ(invoke({"file", "list", "nested"}).output,
            "nested/one.txt\nnested/two.txt\n");

  EXPECT_EQ(invoke({"file", "untrack", "."}).code, 0);
  EXPECT_EQ(invoke({"file", "list"}).output, "");
  const Result root_track = invoke({"file", "track", "."});
  EXPECT_EQ(root_track.code, 0) << root_track.error;
  EXPECT_NE(invoke({"file", "list"}).output.find("tracked.txt"),
            std::string::npos);

  write(".gitignore", "ignored.txt\n");
  write("ignored.txt", "ignored\n");
  EXPECT_EQ(invoke({"file", "track", "ignored.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "track", "--include-ignored", "ignored.txt"})
                .code,
            0);
  EXPECT_EQ(invoke({"file", "track", "--include-ignored", "ignored.txt"})
                .code,
            0);
  EXPECT_NE(invoke({"file", "list"}).output.find("ignored.txt"),
            std::string::npos);
  EXPECT_EQ(invoke({"file", "untrack", "ignored.txt"}).code, 0);
  EXPECT_EQ(invoke({"file", "list"}).output.find("ignored.txt"),
            std::string::npos);
  EXPECT_EQ(invoke({"file", "track", "missing"}).code, 2);
  EXPECT_EQ(invoke({"file", "track", std::string(5000, 'x')}).code, 2);

  const std::filesystem::path tracking = path_ / ".git/gg/file-tracking";
  std::filesystem::remove(tracking);
  std::filesystem::create_directory(tracking.string() + ".tmp");
  EXPECT_EQ(invoke({"file", "track", "tracked.txt"}).code, 2);
  std::filesystem::remove(tracking.string() + ".tmp");
  std::filesystem::create_directory(tracking);
  EXPECT_EQ(invoke({"file", "track", "tracked.txt"}).code, 2);
}

TEST_F(RepositoryTest, ValidatesFileCommandArguments) {
  EXPECT_EQ(invoke({"file"}).code, 2);
  EXPECT_EQ(invoke({"file", "show"}).code, 2);
  EXPECT_EQ(invoke({"file", "search"}).code, 2);
  EXPECT_EQ(invoke({"file", "search", "-p", "x", "--name-only", "-n"})
                .code,
            2);
  EXPECT_EQ(invoke({"file", "chmod", "bad", "tracked.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "chmod", "x"}).code, 2);
  EXPECT_EQ(invoke({"file", "chmod", "x", "-r", "main", "tracked.txt"})
                .code,
            2);
  EXPECT_EQ(invoke({"file", "track", "tracked.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "track"}).code, 2);
  EXPECT_EQ(invoke({"file", "untrack"}).code, 2);
  EXPECT_EQ(invoke({"file", "list", "extra", "-r", "missing"}).code, 2);
}

}  // namespace gg::test
