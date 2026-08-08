// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, DiffsRevisionTreesInSeveralFormats) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", "changed\n");
  write("added.txt", "new\n");
  std::filesystem::create_symlink("added.txt", path_ / "link");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result patch = invoke({"diff"});
  ASSERT_EQ(patch.code, 0) << patch.error;
  EXPECT_NE(patch.output.find("diff --git a/added.txt b/added.txt"),
            std::string::npos);
  EXPECT_NE(patch.output.find("-base"), std::string::npos);
  EXPECT_NE(patch.output.find("+changed"), std::string::npos);
  EXPECT_EQ(invoke({"diff", "--summary"}).output,
            "A added.txt\nA link\nM tracked.txt\n");
  EXPECT_EQ(invoke({"diff", "--types"}).output,
            "-F added.txt\n-L link\nFF tracked.txt\n");
  EXPECT_EQ(invoke({"diff", "--name-only"}).output,
            "added.txt\nlink\ntracked.txt\n");
  EXPECT_NE(invoke({"diff", "--stat"}).output.find("3 files changed"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "--summary", "--git"})
                .output.find("diff --git"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "--color-words", "--context", "0"})
                .output.find("@@ -1 +1 @@"),
            std::string::npos);

  EXPECT_EQ(invoke({"diff", "--from", "@"}).output, "");
  EXPECT_NE(invoke({"diff", "--to", "main"}).output.find("-changed"),
            std::string::npos);
  EXPECT_EQ(invoke({"diff", "--from", "main", "--to", "@", "file:tracked.txt"})
                .output.find("added.txt"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "-r", "@", "glob:*.txt"})
                .output.find("tracked.txt"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "."}).output.find("added.txt"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "-r", "main"}).output.find("+base"),
            std::string::npos);
}

TEST_F(RepositoryTest, DetectsRenamedAndDeletedFiles) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  std::filesystem::rename(path_ / "tracked.txt", path_ / "renamed.txt");
  EXPECT_EQ(invoke({"diff", "--summary"}).output,
            "R {tracked.txt => renamed.txt}\n");
  EXPECT_EQ(invoke({"diff", "--types"}).output,
            "FF {tracked.txt => renamed.txt}\n");

  std::filesystem::remove(path_ / "renamed.txt");
  EXPECT_EQ(invoke({"diff", "--summary"}).output, "D tracked.txt\n");
  EXPECT_EQ(invoke({"diff", "--types"}).output, "F- tracked.txt\n");
}

TEST_F(RepositoryTest, IgnoresRequestedWhitespaceChanges) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  write("tracked.txt", " b a s e \n");
  EXPECT_EQ(invoke({"diff", "-w"}).output, "");
  write("tracked.txt", "base   \n");
  EXPECT_EQ(invoke({"diff", "-b"}).output, "");
}

TEST_F(RepositoryTest, ShowsRevisionMetadataAndPatches) {
  ASSERT_EQ(invoke({"new", "-m", "work", "main"}).code, 0);
  write("tracked.txt", "changed\n");

  const Result shown = invoke({"show"});
  ASSERT_EQ(shown.code, 0) << shown.error;
  EXPECT_NE(shown.output.find("Commit ID: "), std::string::npos);
  EXPECT_NE(shown.output.find("Change ID: "), std::string::npos);
  EXPECT_NE(shown.output.find("Description: work"), std::string::npos);
  EXPECT_NE(shown.output.find("diff --git"), std::string::npos);

  const Result plain = invoke({"show", "main", "--no-patch"});
  EXPECT_NE(plain.output.find("Bookmarks: main"), std::string::npos);
  EXPECT_EQ(plain.output.find("diff --git"), std::string::npos);

  const Result ordered = invoke({"show", "main", "@", "--no-patch"});
  const Result reversed =
      invoke({"show", "main", "@", "--reversed", "--no-patch"});
  EXPECT_NE(ordered.output, reversed.output);
  EXPECT_EQ(invoke({"show", "-r", "main", "-r", "@", "--no-patch"}).code,
            0);
  EXPECT_NE(invoke({"show", "--summary"}).output.find("M tracked.txt"),
            std::string::npos);
}

TEST_F(RepositoryTest, ValidatesDiffAndShowRequests) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_NE(invoke({"show", "--no-patch"})
                .output.find("Description: (no description set)"),
            std::string::npos);
  EXPECT_EQ(invoke({"diff", "-r", "@", "--from", "main"}).code, 2);
  EXPECT_EQ(invoke({"diff", "--context", "-1"}).code, 2);
  EXPECT_EQ(invoke({"diff", ""}).code, 2);
  EXPECT_EQ(invoke({"diff", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"diff", "../outside"}).code, 2);
  EXPECT_EQ(invoke({"diff", "-T", "path"}).code, 2);
  EXPECT_EQ(invoke({"diff", "--tool", "meld"}).code, 2);
  EXPECT_EQ(invoke({"show", "-T", "description"}).code, 2);
  EXPECT_EQ(invoke({"show", "--no-patch", "--summary"}).code, 2);

  const git_oid untracked = raw_commit("untracked");
  const Result raw_show =
      invoke({"show", git_oid_tostr_s(&untracked), "--no-patch"});
  EXPECT_EQ(raw_show.code, 0);
  EXPECT_EQ(raw_show.output.find("Change ID:"), std::string::npos);

  const git_oid side = raw_commit("side");
  set_ref("refs/heads/side", side);
  ASSERT_EQ(invoke({"new", "main", "side"}).code, 0);
  EXPECT_EQ(invoke({"diff"}).code, 2);
}

}  // namespace gg::test
