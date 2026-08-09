// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include <cstdlib>

namespace gg::test {

TEST_F(RepositoryTest, WrapsRepositoryAndWorkspaceGitConfiguration) {
  EXPECT_EQ(invoke({"config", "get", "demo.value"}).code, 2);
  ASSERT_EQ(invoke({"config", "set", "--repo", "demo.value", "repository"})
                .code,
            0);
  EXPECT_EQ(invoke({"config", "get", "demo.value"}).output,
            "repository\n");
  ASSERT_EQ(
      invoke({"config", "set", "--workspace", "demo.value", "workspace"})
          .code,
      0);
  EXPECT_EQ(invoke({"config", "get", "demo.value"}).output,
            "workspace\n");

  const Result effective = invoke({"config", "list", "demo"});
  EXPECT_EQ(effective.output, "demo.value = workspace\n");
  const Result overridden =
      invoke({"config", "list", "--include-overridden", "demo.value"});
  EXPECT_NE(overridden.output.find("local "), std::string::npos);
  EXPECT_NE(overridden.output.find("worktree "), std::string::npos);

  ASSERT_EQ(invoke({"config", "unset", "--workspace", "demo.value"}).code,
            0);
  EXPECT_EQ(invoke({"config", "get", "demo.value"}).output,
            "repository\n");
  ASSERT_EQ(invoke({"config", "unset", "--repo", "demo.value"}).code, 0);
  EXPECT_EQ(invoke({"config", "get", "demo.value"}).code, 2);
}

TEST_F(RepositoryTest, ReportsNativeGitConfigPathsAndScopes) {
  EXPECT_EQ(invoke({"config", "path"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "demo.value", "value"}).code, 2);
  EXPECT_EQ(invoke({"config", "unset", "--repo", "missing.value"}).code,
            2);
  EXPECT_EQ(invoke({"config", "path", "--repo"}).output,
            (path_ / ".git/config").string() + "\n");
  EXPECT_EQ(invoke({"config", "path", "--workspace"}).output,
            (path_ / ".git/config.worktree").string() + "\n");
  EXPECT_FALSE(invoke({"config", "path", "--user"}).output.empty());
  EXPECT_EQ(invoke({"config", "path", "--repo", "--user"}).code, 2);
}

TEST_F(RepositoryTest, UsesGitEditorResolutionOrder) {
  const char* previous_git_editor = std::getenv("GIT_EDITOR");
  const std::string saved_git_editor =
      previous_git_editor == nullptr ? "" : previous_git_editor;
  const char* previous_visual = std::getenv("VISUAL");
  const std::string saved_visual = previous_visual == nullptr ? "" : previous_visual;
  const char* previous_editor = std::getenv("EDITOR");
  const std::string saved_editor = previous_editor == nullptr ? "" : previous_editor;

  ASSERT_EQ(setenv("GIT_EDITOR", "/bin/true", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 0);
  ASSERT_EQ(unsetenv("GIT_EDITOR"), 0);
  ASSERT_EQ(invoke({"config", "set", "--repo", "core.editor", "/bin/true"})
                .code,
            0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 0);
  ASSERT_EQ(invoke({"config", "unset", "--repo", "core.editor"}).code, 0);
  ASSERT_EQ(setenv("VISUAL", "/bin/true", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 0);
  ASSERT_EQ(setenv("GIT_EDITOR", "/missing/gg-editor", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(unsetenv("GIT_EDITOR"), 0);
  ASSERT_EQ(unsetenv("VISUAL"), 0);
  ASSERT_EQ(setenv("EDITOR", "/bin/true", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--workspace"}).code, 0);
  ASSERT_EQ(setenv("EDITOR", "/bin/false", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(setenv("EDITOR", "''", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(setenv("EDITOR", " ", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(unsetenv("EDITOR"), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);

  if (saved_git_editor.empty()) unsetenv("GIT_EDITOR");
  else setenv("GIT_EDITOR", saved_git_editor.c_str(), 1);
  if (saved_visual.empty()) unsetenv("VISUAL");
  else setenv("VISUAL", saved_visual.c_str(), 1);
  if (saved_editor.empty()) unsetenv("EDITOR");
  else setenv("EDITOR", saved_editor.c_str(), 1);
}

}  // namespace gg::test
