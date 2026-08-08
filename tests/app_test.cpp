// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>

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
  EXPECT_NE(run({"help", "status"}).output.find("gg status [OPTIONS]"),
            std::string::npos);
  EXPECT_NE(run({"help", "operation", "restore"})
                .output.find("gg operation restore [OPTIONS]"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "bookmarks"}).output.find("# Bookmarks"),
            std::string::npos);
  EXPECT_NE(run({"help", "--keyword", "config"})
                .output.find("# Configuration"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "filesets"}).output.find("# Filesets"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "glossary"}).output.find("# Glossary"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "revsets"})
                .output.find("# Revision selection"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "templates"}).output.find("# Templates"),
            std::string::npos);
  EXPECT_NE(run({"help", "-k", "tutorial"}).output.find("# Tutorial"),
            std::string::npos);
  EXPECT_EQ(run({"help", "-k", "missing"}).code, 2);
  EXPECT_EQ(run({"help", "status", "-k", "tutorial"}).code, 2);
  EXPECT_EQ(run({"help", "missing"}).code, 2);
  EXPECT_EQ(run({"version"}).output, "gg 0.1.0\n");
  EXPECT_EQ(run({"--ignore-immutable", "--no-pager", "version"}).code, 0);
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
  EXPECT_EQ(invoke({"status", "extra"}).code, 0);
  EXPECT_EQ(invoke({"st"}).code, 0);
  EXPECT_EQ(invoke({"log", "--unknown"}).code, 2);
  EXPECT_EQ(invoke({"edit"}).code, 2);
  EXPECT_EQ(invoke({"edit", "main", "-r", "main"}).code, 2);
  EXPECT_EQ(invoke({"next"}).code, 2);
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
  EXPECT_EQ(invoke({"bookmark", "list", "extra"}).code, 0);
  EXPECT_EQ(invoke({"bookmark"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "delete"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "delete", "-r", "@", "topic"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "create"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "move"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "forget"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "rename", "one"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "rename", "one", "two", "three"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "track"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "untrack"}).code, 2);
  EXPECT_EQ(invoke({"tag", "track"}).code, 2);
  EXPECT_EQ(invoke({"tag", "untrack"}).code, 2);
  EXPECT_EQ(invoke({"git"}).code, 2);
  EXPECT_EQ(invoke({"git", "wat"}).code, 2);
  EXPECT_EQ(run({"git", "fetch"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "extra"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "--tracked"}).code, 1);
  EXPECT_EQ(invoke({"fetch", "--tracked", "--branch", "main"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "--tracked", "--tag", "v1"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "--all-remotes", "--remote", "origin"}).code,
            2);
  EXPECT_EQ(invoke({"fetch", "--all-remotes"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "--branch", "bad name"}).code, 1);
  EXPECT_EQ(invoke({"fetch", "--tag", "bad name"}).code, 1);
  EXPECT_EQ(invoke({"fetch", "--remote", "missing"}).code, 2);
  EXPECT_EQ(invoke({"push"}).code, 2);
  EXPECT_EQ(invoke({"push", "-b", "missing"}).code, 2);
  EXPECT_EQ(invoke({"push", "-t", "missing"}).code, 2);
  EXPECT_EQ(invoke({"push", "-r", "missing"}).code, 2);
  EXPECT_EQ(invoke({"push", "-c", "missing"}).code, 2);
  EXPECT_EQ(invoke({"push", "--named", "value"}).code, 2);
  EXPECT_EQ(invoke({"push", "--named", "=@"}).code, 2);
  EXPECT_EQ(invoke({"push", "--named", "value="}).code, 2);
  EXPECT_EQ(invoke({"push", "--named", "bad name=@"}).code, 2);
  EXPECT_EQ(invoke({"push", "--named", "main=@"}).code, 2);
  EXPECT_EQ(invoke({"push", "--deleted", "-b", "main"}).code, 2);
  EXPECT_EQ(invoke({"push", "--deleted", "-t", "v1"}).code, 2);
  EXPECT_EQ(invoke({"push", "--deleted", "-r", "main"}).code, 2);
  EXPECT_EQ(invoke({"push", "--deleted", "-c", "main"}).code, 2);
  EXPECT_EQ(invoke({"push", "--deleted", "--named", "new=main"}).code, 2);
  EXPECT_EQ(invoke({"push", "-b", "missing", "extra"}).code, 2);
  EXPECT_EQ(run({"clone"}).code, 2);
  EXPECT_EQ(run({"clone", "one", "two", "three"}).code, 2);
  EXPECT_EQ(run({"clone", "--depth", "0", "one"}).code, 2);
  EXPECT_EQ(run({"clone", "--object-hash", "invalid", "one"}).code, 2);
  EXPECT_EQ(run({"clone", "--object-hash", "sha256", "one"}).code, 2);
  EXPECT_EQ(run({"clone", "--branch", "bad name", "one"}).code, 2);
  EXPECT_EQ(run({"clone", "--tag", "bad name", "one"}).code, 2);
  EXPECT_EQ(run({"init", "one", "two"}).code, 2);
  EXPECT_EQ(run({"init", "--object-hash", "invalid"}).code, 2);
  EXPECT_EQ(invoke({"undo", "extra"}).code, 2);
  EXPECT_EQ(invoke({"redo", "extra"}).code, 2);
  EXPECT_EQ(invoke({"operation"}).code, 2);
  EXPECT_EQ(invoke({"operation", "log", "extra"}).code, 2);
  EXPECT_EQ(invoke({"op", "log", "extra"}).code, 2);
  EXPECT_EQ(invoke({"operation", "restore"}).code, 2);
  EXPECT_EQ(invoke({"operation", "restore", "--what", "invalid", "@"}).code,
            2);
  EXPECT_EQ(invoke({"operation", "restore", ""}).code, 2);
  EXPECT_EQ(invoke({"operation", "restore", "@", "extra"}).code, 2);
  EXPECT_EQ(invoke({"util"}).code, 2);
  EXPECT_EQ(invoke({"util", "snapshot", "extra"}).code, 2);
  EXPECT_EQ(invoke({"workspace"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "list", "extra"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "root", "extra"}).code, 2);
  EXPECT_EQ(invoke({"next", "0"}).code, 2);
  EXPECT_EQ(invoke({"prev", "word"}).code, 2);
  EXPECT_EQ(invoke({"next", "--edit", "--no-edit"}).code, 2);
  EXPECT_EQ(invoke({"prev", "--conflict", "2"}).code, 2);
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
  EXPECT_EQ(invoke({"redo"}).code, 2);
  EXPECT_EQ(invoke({"operation", "log"}).output, "No operations.\n");
  std::filesystem::remove_all(bare_path);
}

TEST_F(RepositoryTest, ExecutesUtilitiesWithExactArgumentsAndWorkspaceRoot) {
  const std::filesystem::path result_path = path_ / "exec-result";
  const char* previous_raw = std::getenv("GG_WORKSPACE_ROOT");
  const std::optional<std::string> previous =
      previous_raw == nullptr ? std::nullopt
                              : std::optional<std::string>(previous_raw);
  ASSERT_EQ(setenv("GG_WORKSPACE_ROOT", "wrong", 1), 0);
  const Result result = invoke(
      {"util", "exec", "--", "/bin/sh", "-c",
       "printf '%s\\n' \"$GG_WORKSPACE_ROOT\" \"$1\" \"$2\" > \"$3\"; "
       "exit \"$4\"",
       "gg-script", "a b", "", result_path.string(), "7"});
  if (previous.has_value()) {
    ASSERT_EQ(setenv("GG_WORKSPACE_ROOT", previous->c_str(), 1), 0);
  } else {
    ASSERT_EQ(unsetenv("GG_WORKSPACE_ROOT"), 0);
  }
  EXPECT_EQ(result.code, 7) << result.error;
  std::ifstream input(result_path);
  std::ostringstream contents;
  contents << input.rdbuf();
  EXPECT_EQ(contents.str(),
            std::filesystem::weakly_canonical(path_).string() +
                "\na b\n\n");

  EXPECT_EQ(invoke({"util", "exec", "definitely-not-a-gg-test-command"})
                .code,
            2);
  EXPECT_EQ(invoke({"util", "exec", "--", "/bin/sh", "-c",
                    "kill -TERM $$"})
                .code,
            2);

  const auto bare_path =
      path_.parent_path() / (path_.filename().string() + "-exec-bare");
  std::filesystem::remove_all(bare_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, bare_path.string().c_str(), 1), 0);
  git_repository_free(bare);
  EXPECT_EQ(run({"-R", bare_path.string(), "util", "exec", "/bin/true"})
                .code,
            0);
  std::filesystem::remove_all(bare_path);
  EXPECT_EQ(run({"-R", "/", "util", "exec", "/bin/true"}).code, 0);
}

TEST_F(RepositoryTest, GeneratesMarkdownAndManPageHelp) {
  const Result markdown = run({"util", "markdown-help"});
  ASSERT_EQ(markdown.code, 0) << markdown.error;
  EXPECT_NE(markdown.output.find("# gg command reference"), std::string::npos);
  EXPECT_NE(markdown.output.find("## `gg bookmark move`"), std::string::npos);
  EXPECT_NE(markdown.output.find("```text\n"), std::string::npos);
  EXPECT_EQ(run({"util", "markdown-help"}).output, markdown.output);

  const std::filesystem::path destination = path_ / "manual";
  const Result installed =
      run({"util", "install-man-pages", destination.string()});
  ASSERT_EQ(installed.code, 0) << installed.error;
  EXPECT_TRUE(std::filesystem::is_regular_file(destination / "man1/gg.1"));
  const std::filesystem::path move_page =
      destination / "man1/gg-bookmark-move.1";
  ASSERT_TRUE(std::filesystem::is_regular_file(move_page));
  std::ifstream input(move_page);
  std::ostringstream content;
  content << input.rdbuf();
  EXPECT_NE(content.str().find(".TH \"GG-BOOKMARK-MOVE\" \"1\""),
            std::string::npos);
  EXPECT_NE(content.str().find(".SH SYNOPSIS"), std::string::npos);
  EXPECT_EQ(run({"util", "install-man-pages"}).code, 2);
}

TEST(AppTest, GeneratesShellCompletionsFromTheCommandSchema) {
  const std::vector<std::pair<std::string, std::string>> shells{
      {"bash", "complete -F _gg gg"},
      {"elvish", "edit:completion:arg-completer[gg]"},
      {"fish", "complete -c gg"},
      {"nushell", "nu-complete gg"},
      {"power-shell", "Register-ArgumentCompleter"},
      {"powershell", "Register-ArgumentCompleter"},
      {"zsh", "#compdef gg"}};
  for (const auto& [shell, marker] : shells) {
    const Result completion = run({"util", "completion", shell});
    ASSERT_EQ(completion.code, 0) << shell << ": " << completion.error;
    EXPECT_NE(completion.output.find(marker), std::string::npos) << shell;
    EXPECT_NE(completion.output.find("bookmark"), std::string::npos) << shell;
    EXPECT_NE(completion.output.find("--repository"), std::string::npos)
        << shell;
  }
  EXPECT_EQ(run({"util", "completion"}).code, 2);
  EXPECT_EQ(run({"util", "completion", "tcsh"}).code, 2);
}

TEST(AppTest, PrintsTheConfigurationSchema) {
  const Result schema = run({"util", "config-schema"});
  ASSERT_EQ(schema.code, 0) << schema.error;
  EXPECT_NE(schema.output.find("https://json-schema.org/draft/2020-12/schema"),
            std::string::npos);
  EXPECT_NE(schema.output.find("patternProperties"), std::string::npos);
  EXPECT_NE(schema.output.find("additionalProperties\": false"),
            std::string::npos);
  EXPECT_EQ(run({"util", "config-schema"}).output, schema.output);
  EXPECT_EQ(run({"util", "config-schema", "extra"}).code, 2);
}

TEST_F(RepositoryTest, PrunesUnreachableGitObjects) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  git_oid orphan{};
  ASSERT_EQ(git_blob_create_from_buffer(&orphan, repository_.get(),
                                        "unreachable", 11),
            0);
  git_odb* raw_odb = nullptr;
  ASSERT_EQ(git_repository_odb(&raw_odb, repository_.get()), 0);
  std::unique_ptr<git_odb, decltype(&git_odb_free)> odb(raw_odb, git_odb_free);
  EXPECT_NE(git_odb_exists(odb.get(), &orphan), 0);
  const std::string orphan_text = git_oid_tostr_s(&orphan);
  const std::filesystem::path orphan_path =
      path_ / ".git/objects" / orphan_text.substr(0, 2) /
      orphan_text.substr(2);
  ASSERT_TRUE(std::filesystem::is_regular_file(orphan_path));
  std::filesystem::last_write_time(
      orphan_path, std::filesystem::file_time_type::clock::now() -
                       std::chrono::hours(1));

  const Result ordinary = invoke({"util", "gc"});
  ASSERT_EQ(ordinary.code, 0) << ordinary.error;
  EXPECT_NE(ordinary.output.find("Garbage collection completed."),
            std::string::npos);
  EXPECT_NE(git_odb_exists(odb.get(), &orphan), 0);

  const Result immediate = invoke({"util", "gc", "--expire", "now"});
  ASSERT_EQ(immediate.code, 0) << immediate.error;
  EXPECT_FALSE(std::filesystem::exists(orphan_path));
  EXPECT_NE(invoke({"operation", "log"}).output.find("gg new"),
            std::string::npos);
  EXPECT_EQ(invoke({"util", "gc", "--expire", "yesterday"}).code, 2);
}

}  // namespace gg::test
