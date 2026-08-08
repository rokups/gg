// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>

namespace gg::test {

TEST_F(RepositoryTest, SetsGetsListsAndUnsetsLayeredConfiguration) {
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.color", "\"red\""}).code,
            0);
  EXPECT_EQ(invoke({"config", "get", "ui.color"}).output, "\"red\"\n");
  EXPECT_EQ(invoke({"config", "set", "--workspace", "ui.color", "\"blue\""})
                .code,
            0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.number", "42"}).code, 0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.enabled", "true"}).code,
            0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.disabled", "false"}).code,
            0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.array", "[1, 2]"}).code,
            0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.table", "{ x = 1 }"}).code,
            0);
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.color", "\"green\""}).code,
            0);
  EXPECT_EQ(invoke({"config", "get", "ui.color"}).output, "\"blue\"\n");

  const Result listed = invoke({"config", "list", "ui"});
  EXPECT_NE(listed.output.find("ui.color = \"blue\""), std::string::npos);
  EXPECT_NE(listed.output.find("ui.number = 42"), std::string::npos);
  const Result overridden =
      invoke({"config", "list", "--include-overridden", "ui.color"});
  EXPECT_EQ(std::count(overridden.output.begin(), overridden.output.end(), '\n'),
            2);
  EXPECT_NE(invoke({"config", "list", "--repo", "--include-defaults"})
                .output.find("ui.enabled = true"),
            std::string::npos);

  EXPECT_EQ(invoke({"config", "unset", "--workspace", "ui.color"}).code, 0);
  EXPECT_EQ(invoke({"config", "get", "ui.color"}).output, "\"green\"\n");
  EXPECT_EQ(invoke({"config", "unset", "--repo", "ui.color"}).code, 0);
  EXPECT_EQ(invoke({"config", "get", "ui.color"}).code, 2);
}

TEST_F(RepositoryTest, ReportsConfigPathsAndValidatesRequests) {
  const std::string repo_path =
      invoke({"config", "path", "--repo"}).output;
  EXPECT_NE(repo_path.find("/gg/config.toml\n"), std::string::npos);
  EXPECT_NE(invoke({"config", "path", "--workspace"})
                .output.find("/gg/workspaces/default.toml\n"),
            std::string::npos);
  EXPECT_EQ(invoke({"config", "path"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "name", "\"value\""}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", ".bad", "1"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "bad.", "1"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "", "1"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "bad..key", "1"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "bad$key", "1"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "key", "bare"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "key", "1bad"}).code, 2);
  EXPECT_EQ(invoke({"config", "set", "--repo", "key", " "}).code, 2);
  EXPECT_EQ(invoke({"config", "unset", "--repo", "missing"}).code, 2);
  EXPECT_EQ(invoke({"config", "list", "-T", "name"}).code, 2);
  EXPECT_EQ(invoke({"config", "path", "--repo", "--user"}).code, 2);
}

TEST_F(RepositoryTest, PreservesConfigTextAndReportsFileErrors) {
  std::string path = invoke({"config", "path", "--repo"}).output;
  path.pop_back();
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  {
    std::ofstream output(path);
    output << "\n# preserved\nmalformed\n = 1\nempty = \n"
              "ui.color = \"old\"\nui.color = \"duplicate\"\n";
  }
  EXPECT_EQ(invoke({"config", "set", "--repo", "ui.color", "\"new\""}).code,
            0);
  EXPECT_EQ(invoke({"config", "get", "ui.color"}).output, "\"new\"\n");
  EXPECT_TRUE(invoke({"config", "list", "missing"}).output.empty());
  EXPECT_EQ(invoke({"config", "unset", "--repo", "ui.color"}).code, 0);

  std::filesystem::remove(path);
  std::filesystem::create_directories(path + ".tmp");
  EXPECT_EQ(invoke({"config", "set", "--repo", "key", "1"}).code, 2);
  std::filesystem::remove_all(path + ".tmp");
  std::filesystem::create_directories(path);
  EXPECT_EQ(invoke({"config", "set", "--repo", "key", "1"}).code, 2);
  std::filesystem::remove_all(path);
}

TEST_F(RepositoryTest, UsesUserConfigAndLaunchesAnEditor) {
  const std::filesystem::path config_home = path_ / "config-home";
  const char* old_xdg = std::getenv("XDG_CONFIG_HOME");
  const std::string saved_xdg = old_xdg == nullptr ? "" : old_xdg;
  const char* old_visual = std::getenv("VISUAL");
  const std::string saved_visual = old_visual == nullptr ? "" : old_visual;
  const char* old_editor = std::getenv("EDITOR");
  const std::string saved_editor = old_editor == nullptr ? "" : old_editor;
  ASSERT_EQ(setenv("XDG_CONFIG_HOME", config_home.string().c_str(), 1), 0);
  ASSERT_EQ(setenv("VISUAL", "/bin/true", 1), 0);

  EXPECT_EQ(invoke({"config", "set", "--user", "user.name", "'Test'"}).code,
            0);
  EXPECT_EQ(invoke({"config", "get", "user.name"}).output, "'Test'\n");
  EXPECT_NE(invoke({"config", "path", "--user"})
                .output.find(config_home.string()),
            std::string::npos);
  EXPECT_NE(invoke({"config", "list", "--user"})
                .output.find("user.name = 'Test'"),
            std::string::npos);
  EXPECT_EQ(invoke({"config", "edit", "--user"}).code, 0);

  ASSERT_EQ(setenv("VISUAL", "/missing/gg-editor", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(setenv("VISUAL", "/bin/false", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  unsetenv("VISUAL");
  unsetenv("EDITOR");
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);
  ASSERT_EQ(setenv("VISUAL", "", 1), 0);
  ASSERT_EQ(setenv("EDITOR", "", 1), 0);
  EXPECT_EQ(invoke({"config", "edit", "--repo"}).code, 2);

  const char* old_home = std::getenv("HOME");
  const std::string saved_home = old_home == nullptr ? "" : old_home;
  unsetenv("XDG_CONFIG_HOME");
  unsetenv("HOME");
  EXPECT_EQ(invoke({"config", "path", "--user"}).code, 2);
  setenv("XDG_CONFIG_HOME", "", 1);
  setenv("HOME", "", 1);
  EXPECT_EQ(invoke({"config", "path", "--user"}).code, 2);
  if (saved_home.empty()) {
    unsetenv("HOME");
  } else {
    setenv("HOME", saved_home.c_str(), 1);
  }

  if (saved_xdg.empty()) {
    unsetenv("XDG_CONFIG_HOME");
  } else {
    setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
  }
  if (saved_visual.empty()) {
    unsetenv("VISUAL");
  } else {
    setenv("VISUAL", saved_visual.c_str(), 1);
  }
  if (saved_editor.empty()) {
    unsetenv("EDITOR");
  } else {
    setenv("EDITOR", saved_editor.c_str(), 1);
  }
}

}  // namespace gg::test
