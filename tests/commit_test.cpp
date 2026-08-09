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

TEST_F(RepositoryTest, CommitsFilesetSelections) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  write("tracked.txt", "selected\n");
  write("nested/remaining.txt", "remaining\n");
  write("other.bin", "binary\n");

  const Result committed =
      invoke({"commit", "-m", "selected",
              "glob('*.txt') ~ file('nested/remaining.txt')"});
  ASSERT_EQ(committed.code, 0) << committed.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).output,
            "selected\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "nested/remaining.txt"})
                .code,
            2);
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "other.bin"}).code, 2);
  EXPECT_NE(invoke({"status"}).output.find("nested/remaining.txt"),
            std::string::npos);
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

TEST_F(RepositoryTest, SelectsCommitChangesWithDiffEditors) {
  ASSERT_EQ(invoke({"new", "-m", "draft", "main"}).code, 0);
  write("tracked.txt", "changed\n");
  write("added.txt", "added\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const std::filesystem::path selector = path_ / "select-files";
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
  const Result interactive = invoke({"commit", "--interactive"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(interactive.code, 0) << interactive.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).output,
            "base\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "added.txt"}).output,
            "added\n");
  EXPECT_EQ(invoke({"file", "show", "tracked.txt"}).output, "changed\n");

  write("tool.sh", "#!/bin/sh\n");
  write("nested/tool.txt", "nested\n");
  std::filesystem::permissions(
      path_ / "tool.sh", std::filesystem::perms::owner_exec,
      std::filesystem::perm_options::add);
  std::filesystem::create_symlink("tool.sh", path_ / "tool-link");
  const std::filesystem::path editor = path_ / "diff-editor";
  {
    std::ofstream script(editor);
    script << "#!/bin/sh\ncp \"$1/tracked.txt\" \"$2/tracked.txt\"\n";
  }
  std::filesystem::permissions(
      editor, std::filesystem::perms::owner_exec |
                  std::filesystem::perms::owner_read |
                  std::filesystem::perms::owner_write);
  const Result external = invoke({"commit", "--tool", editor.string()});
  ASSERT_EQ(external.code, 0) << external.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).output,
            "base\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tool.sh"}).output,
            "#!/bin/sh\n");
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tool.sh"),
            GIT_FILEMODE_BLOB_EXECUTABLE);
  EXPECT_EQ(file_mode("refs/gg/workspaces/default", "tool-link"),
            GIT_FILEMODE_LINK);

  write("path-only.txt", "path only\n");
  const Result path_only =
      invoke({"commit", "path-only.txt", "--tool", "/bin/true"});
  ASSERT_EQ(path_only.code, 0) << path_only.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "path-only.txt"}).output,
            "path only\n");

  write("configured.txt", "configured\n");
  const char* configured_old_visual = std::getenv("VISUAL");
  const std::string configured_saved_visual =
      configured_old_visual == nullptr ? "" : configured_old_visual;
  ASSERT_EQ(setenv("VISUAL", "/bin/true", 1), 0);
  const Result configured =
      invoke({"commit", "configured.txt", "--interactive"});
  if (configured_saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", configured_saved_visual.c_str(), 1);
  }
  ASSERT_EQ(configured.code, 0) << configured.error;

  const std::filesystem::path invalid_selector = path_ / "invalid-selector";
  {
    std::ofstream script(invalid_selector);
    script << "#!/bin/sh\nprintf 'unknown.txt\\n' > \"$1\"\n";
  }
  std::filesystem::permissions(
      invalid_selector, std::filesystem::perms::owner_exec |
                            std::filesystem::perms::owner_read |
                            std::filesystem::perms::owner_write);
  ASSERT_EQ(setenv("VISUAL", invalid_selector.string().c_str(), 1), 0);
  EXPECT_EQ(invoke({"commit", "--interactive"}).code, 2);

  const std::filesystem::path crlf_selector = path_ / "crlf-selector";
  {
    std::ofstream script(crlf_selector);
    script << "#!/bin/sh\nprintf '# selected\\n\\ntracked.txt\\r\\n' > \"$1\"\n";
  }
  std::filesystem::permissions(
      crlf_selector, std::filesystem::perms::owner_exec |
                         std::filesystem::perms::owner_read |
                         std::filesystem::perms::owner_write);
  ASSERT_EQ(setenv("VISUAL", crlf_selector.string().c_str(), 1), 0);
  const Result final_selection = invoke({"commit", "--interactive"});
  if (configured_saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", configured_saved_visual.c_str(), 1);
  }
  ASSERT_EQ(final_selection.code, 0) << final_selection.error;

  write("all-paths.txt", "all paths\n");
  const Result all_paths = invoke({"commit", ".", "--tool", "/bin/true"});
  ASSERT_EQ(all_paths.code, 0) << all_paths.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "all-paths.txt"}).output,
            "all paths\n");

  EXPECT_EQ(invoke({"commit", "--tool", ":missing"}).code, 2);
  EXPECT_EQ(invoke({"commit", "--tool", "/bin/false"}).code, 2);
  ASSERT_EQ(invoke({"config", "set", "--repo", "mergetool.false.path",
                    "/bin/false"})
                .code,
            0);
  EXPECT_EQ(invoke({"commit", "--tool", "false"}).code, 2);
}

TEST_F(RepositoryTest, SelectsRenamesAsOneInteractiveChange) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  std::filesystem::rename(path_ / "tracked.txt", path_ / "renamed.txt");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const char* old_visual = std::getenv("VISUAL");
  const std::string saved_visual = old_visual == nullptr ? "" : old_visual;
  ASSERT_EQ(setenv("VISUAL", "/bin/true", 1), 0);
  const Result committed = invoke({"commit", "--interactive"});
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  ASSERT_EQ(committed.code, 0) << committed.error;
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "tracked.txt"}).code, 2);
  EXPECT_EQ(invoke({"file", "show", "-r", "@-", "renamed.txt"}).output,
            "base\n");
}

TEST_F(RepositoryTest, ValidatesCommitRequests) {
  EXPECT_EQ(invoke({"commit", "-m", "message"}).code, 2);
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"commit", "--tool", "missing-gg-diff-editor"}).code, 2);
  EXPECT_EQ(invoke({"commit", ""}).code, 2);
  EXPECT_EQ(invoke({"commit", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"commit", "../outside"}).code, 2);
  EXPECT_EQ(invoke({"commit", "one", "two", "--unknown"}).code, 2);
}

}  // namespace gg::test
