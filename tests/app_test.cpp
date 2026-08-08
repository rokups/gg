// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST(AppTest, PrintsHelpAndVersion) {
  EXPECT_EQ(run({}).code, 0);
  EXPECT_EQ(run({"help"}).code, 0);
  EXPECT_EQ(run({"-h"}).code, 0);
  EXPECT_EQ(run({"--version"}).code, 0);
  EXPECT_EQ(run({"--help"}).code, 0);
  EXPECT_NE(run({"--help"}).output.find("gg [OPTIONS] SUBCOMMAND"),
            std::string::npos);
  EXPECT_EQ(run({"status", "--help"}).code, 0);
  EXPECT_NE(run({"status", "--help"}).output.find("gg status [OPTIONS]"),
            std::string::npos);
  EXPECT_EQ(run({"version"}).output, "gg 0.1.0\n");
}

TEST(AppTest, ReportsUserAndGitErrors) {
  EXPECT_EQ(run({"wat"}).code, 2);
  EXPECT_NE(run({"wat"}).error.find("subcommand is required"),
            std::string::npos);
  EXPECT_EQ(run({"-R", "."}).code, 2);
}

TEST_F(RepositoryTest, ValidatesCommandArgumentsAndRevisionShapes) {
  EXPECT_EQ(invoke({"log"}).code, 0);
  EXPECT_EQ(invoke({"log", "-r", "main"}).code, 0);
  EXPECT_EQ(invoke({"status", "extra"}).code, 2);
  EXPECT_EQ(invoke({"st"}).code, 0);
  EXPECT_EQ(invoke({"log", "extra"}).code, 2);
  EXPECT_EQ(invoke({"edit"}).code, 2);
  EXPECT_EQ(invoke({"describe"}).code, 2);
  EXPECT_EQ(invoke({"describe", "-m", ""}).code, 2);
  EXPECT_EQ(invoke({"describe", "-m", "x", "main", "other"}).code, 2);
  EXPECT_EQ(invoke({"new", "-m"}).code, 2);
  EXPECT_EQ(invoke({"log", "-r", "@"}).code, 2);
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"log", "-r", "@x"}).code, 2);
  EXPECT_EQ(invoke({"log", "-r", "@-x"}).code, 2);
  EXPECT_EQ(invoke({"log", "-r", "@--"}).code, 2);
  EXPECT_EQ(invoke({"log", "-r", "missing"}).code, 2);
  EXPECT_EQ(invoke({"rebase"}).code, 2);
  EXPECT_EQ(invoke({"rebase", "-s", "@"}).code, 2);
  EXPECT_EQ(invoke({"rebase", "-s", "@", "-d", "main", "extra"}).code, 2);
  EXPECT_EQ(invoke({"rebase", "-s", "@", "-d", "@"}).code, 2);
  EXPECT_EQ(invoke({"split"}).code, 2);
  EXPECT_EQ(invoke({"split", "/absolute"}).code, 2);
  EXPECT_EQ(invoke({"split", ""}).code, 2);
  EXPECT_EQ(invoke({"split", "tracked.txt"}).code, 2);
  EXPECT_EQ(invoke({"squash", "-r", "@", "--from", "@"}).code, 2);
  EXPECT_EQ(invoke({"squash", "-r", "@", "--into", "main"}).code, 2);
  EXPECT_EQ(invoke({"squash", "extra"}).code, 2);
  EXPECT_EQ(invoke({"squash", "--into", "main"}).code, 0);
  EXPECT_EQ(invoke({"abandon", "@", "main"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "list", "extra"}).code, 2);
  EXPECT_EQ(invoke({"bookmark"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "delete"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "delete", "-r", "@", "topic"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "create"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "move", "x"}).code, 2);
  EXPECT_EQ(invoke({"git"}).code, 2);
  EXPECT_EQ(invoke({"git", "wat"}).code, 2);
  EXPECT_EQ(invoke({"git", "fetch", "extra"}).code, 2);
  EXPECT_EQ(invoke({"git", "push"}).code, 2);
  EXPECT_EQ(invoke({"git", "push", "-b", "missing"}).code, 2);
  EXPECT_EQ(invoke({"git", "push", "-b", "missing", "extra"}).code, 2);
  EXPECT_EQ(run({"git", "clone"}).code, 2);
  EXPECT_EQ(run({"git", "clone", "one", "two", "three"}).code, 2);
  EXPECT_EQ(invoke({"undo", "extra"}).code, 2);
  EXPECT_EQ(invoke({"unknown"}).code, 2);
  EXPECT_EQ(invoke({"continue"}).code, 2);
  EXPECT_EQ(invoke({"abort"}).code, 2);
}

TEST_F(RepositoryTest, ReportsBareRepositoriesAndEmptyUndoHistory) {
  const auto bare_path = path_.parent_path() / (path_.filename().string() + "-bare");
  std::filesystem::remove_all(bare_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, bare_path.string().c_str(), 1), 0);
  git_repository_free(bare);
  EXPECT_EQ(run({"-R", bare_path.string(), "status"}).code, 2);
  EXPECT_EQ(invoke({"undo"}).code, 2);
  std::filesystem::remove_all(bare_path);
}

}  // namespace gg::test
