// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include <cstdlib>

namespace gg::test {

TEST_F(RepositoryTest, CommitsTheWorkingChangeAndStartsANewOne) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  write("tracked.txt", "changed\n");
  write("added.txt", "new\n");

  const Result committed = invoke({"commit", "-m", "final", "."});
  ASSERT_EQ(committed.code, 0) << committed.error;
  EXPECT_NE(committed.output.find("Committed as "), std::string::npos);
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).output,
            "changed\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "added.txt"}).output,
            "new\n");
  EXPECT_NE(invoke({"show", "@-", "--no-patch"})
                .output.find("Description: final"),
            std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("no changes"), std::string::npos);
  const git_oid topic = ref("refs/heads/topic");
  const git_oid parent = commit_parent(ref("refs/gg/workspaces/default"));
  EXPECT_NE(git_oid_equal(&topic, &parent), 0);
}

TEST_F(RepositoryTest, CommitsOnlySelectedPaths) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  write("tracked.txt", "selected\n");
  write("nested/remaining.txt", "remaining\n");

  ASSERT_EQ(invoke({"commit", "-m", "selected", "./tracked.txt"}).code, 0);
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).output,
            "selected\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "nested/remaining.txt"})
                .code,
            2);
  EXPECT_EQ(invoke({"file", "show", "nested/remaining.txt"}).output,
            "remaining\n");
  const std::string status = invoke({"status"}).output;
  EXPECT_NE(status.find("A nested/remaining.txt"), std::string::npos);
  EXPECT_EQ(status.find("tracked.txt"), std::string::npos);
}

TEST_F(RepositoryTest, PreservesOrClearsCommitDescriptions) {
  ASSERT_EQ(invoke({"new", "-m", "keep me", "main"}).code, 0);
  ASSERT_EQ(invoke({"ci"}).code, 0);
  EXPECT_NE(invoke({"show", "@-", "--no-patch"})
                .output.find("Description: keep me"),
            std::string::npos);

  ASSERT_EQ(invoke({"commit", "-m", ""}).code, 0);
  EXPECT_NE(invoke({"show", "@-", "--no-patch"})
                .output.find("Description: (no description set)"),
            std::string::npos);
}

TEST_F(RepositoryTest, EditsCommitDescriptions) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  const std::filesystem::path editor = path_ / "commit-editor";
  {
    std::ofstream script(editor);
    script << "#!/bin/sh\nprintf 'edited commit\\n' > \"$1\"\n";
  }
  std::filesystem::permissions(
      editor, std::filesystem::perms::owner_exec |
                  std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write);
  const char* old_visual = std::getenv("VISUAL");
  const std::string saved_visual = old_visual == nullptr ? "" : old_visual;
  ASSERT_EQ(setenv("VISUAL", editor.string().c_str(), 1), 0);
  const Result committed = invoke({"commit", "--editor", "-m", "seed"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(committed.code, 0) << committed.error;
  EXPECT_NE(invoke({"show", "@-", "--no-patch"})
                .output.find("Description: edited commit"),
            std::string::npos);
}

TEST_F(RepositoryTest, ValidatesCommitRequests) {
  EXPECT_EQ(invoke({"commit", "-m", "message"}).code, 2);
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"commit", "--interactive"}).code, 2);
  EXPECT_EQ(invoke({"commit", "--tool", "meld"}).code, 2);
  EXPECT_EQ(invoke({"commit", ""}).code, 2);
  EXPECT_EQ(invoke({"commit", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"commit", "../outside"}).code, 2);
  EXPECT_EQ(invoke({"commit", "one", "two", "--unknown"}).code, 2);
}

}  // namespace gg::test
