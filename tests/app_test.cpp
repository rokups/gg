// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <optional>

namespace gg::test {
namespace {

std::size_t occurrences(std::string_view text, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t position = 0;
       (position = text.find(needle, position)) != std::string_view::npos;
       position += needle.size()) {
    ++count;
  }
  return count;
}

}  // namespace

TEST(AppTest, PrintsHelpAndVersion) {
  EXPECT_EQ(run({}).code, 0);
  EXPECT_EQ(run({"-h"}).code, 0);
  EXPECT_EQ(run({"--version"}).code, 0);
  EXPECT_EQ(run({"--help"}).code, 0);
  EXPECT_NE(run({"--help"}).output.find("gg [OPTIONS] SUBCOMMAND"),
            std::string::npos);
  EXPECT_EQ(run({"--help"}).output.find("  help "), std::string::npos);
  EXPECT_EQ(run({"--help"}).output.find("  version "), std::string::npos);
  EXPECT_EQ(run({"status", "--help"}).code, 0);
  EXPECT_NE(run({"status", "--help"}).output.find("gg status [OPTIONS]"),
            std::string::npos);
  EXPECT_EQ(run({"status", "--help"}).output.find("DETAILS:"),
            std::string::npos);
  EXPECT_EQ(run({}).output.find("WORKING MODEL:"), std::string::npos);
  EXPECT_EQ(run({"help"}).code, 2);
  EXPECT_EQ(run({"version"}).code, 2);
  EXPECT_EQ(run({"--ignore-immutable", "status"}).code, 2);
  EXPECT_EQ(run({"--no-pager", "status"}).code, 2);
  const Result doc = run({"config", "set", "--doc"});
  EXPECT_EQ(doc.code, 0);
  EXPECT_NE(doc.output.find("gg config set — Set a configuration value"),
            std::string::npos);
  EXPECT_NE(doc.output.find("name TEXT REQUIRED"), std::string::npos);
  const Result root_doc = run({"--doc"});
  EXPECT_EQ(root_doc.code, 0);
  EXPECT_NE(root_doc.output.find("gg — A JJ-shaped Git interface"),
            std::string::npos);
  const Result group_doc = run({"bookmark", "--doc"});
  EXPECT_EQ(group_doc.code, 0);
  EXPECT_NE(group_doc.output.find("gg bookmark — Manage bookmarks"),
            std::string::npos);
  const Result rooted_doc =
      run({"-R", ".", "config", "set", "--doc"});
  EXPECT_EQ(rooted_doc.code, 0);
  EXPECT_NE(rooted_doc.output.find("gg config set —"), std::string::npos);
  EXPECT_NE(run({"op", "--doc"}).output.find("gg operation —"),
            std::string::npos);
}

TEST(AppTest, ResolvesFullDocumentationInEitherArgumentOrder) {
  const Result nested = run({"config", "set", "--doc"});
  const Result prefixed = run({"--doc", "config", "set"});
  ASSERT_EQ(nested.code, 0);
  EXPECT_EQ(prefixed.code, 0);
  EXPECT_EQ(prefixed.output, nested.output);

  const Result alias = run({"op", "restore", "--doc"});
  const Result prefixed_alias = run({"--doc", "op", "restore"});
  ASSERT_EQ(alias.code, 0);
  EXPECT_EQ(prefixed_alias.output, alias.output);
  EXPECT_NE(alias.output.find("gg operation restore —"), std::string::npos);

  EXPECT_EQ(run({"--doc", "ci"}).output, run({"commit", "--doc"}).output);
}

TEST(AppTest, RootDocumentationExplainsTheAgentWorkingModel) {
  const Result doc = run({"--doc"});
  ASSERT_EQ(doc.code, 0);
  for (std::string_view section :
       {"WORKING MODEL:", "WORKFLOW:", "SELECTORS:", "SAFETY:",
        "EXIT STATUS:", "gg status", "gg new -m", "@-", "gg COMMAND --doc",
        "gg --doc COMMAND", "gg util markdown-help"}) {
    EXPECT_NE(doc.output.find(section), std::string::npos) << section;
  }
  EXPECT_EQ(occurrences(doc.output, "A JJ-shaped Git interface"), 1u);
}

TEST(AppTest, FullDocumentationBypassesRepositoriesAndRequiredArguments) {
  const Result outside = run({"-R", "/", "--doc", "config", "set"});
  EXPECT_EQ(outside.code, 0);
  EXPECT_NE(outside.output.find("gg config set —"), std::string::npos);
  EXPECT_EQ(run({"--doc", "split"}).code, 0);
  EXPECT_EQ(run({"clone", "--doc"}).code, 0);
}

TEST(AppTest, DocumentsDefaultsAndSideEffectsAcrossCommandFamilies) {
  const std::vector<std::pair<std::vector<std::string>, std::string_view>> cases{
      {{"status", "--doc"}, "Snapshots tracked working-copy files"},
      {{"new", "--doc"}, "Uses @ as the parent"},
      {{"split", "--doc"}, "Both halves must be non-empty"},
      {{"file", "list", "--doc"}, "Lists all files in @"},
      {{"diff", "--doc"}, "Compares @ with its parent"},
      {{"bookmark", "set", "--doc"}, "move forward only"},
      {{"fetch", "--doc"}, "Fetches and prunes origin"},
      {{"push", "--doc"}, "Advances the closest bookmark"},
      {{"operation", "restore", "--doc"},
       "Restores repository and remote-tracking state"},
      {{"workspace", "add", "--doc"}, "copying current sparse patterns"},
      {{"config", "--doc"}, "--workspace is per-worktree config"},
      {{"util", "gc", "--doc"}, "runs native `git gc`"}};
  for (const auto& [arguments, expected] : cases) {
    const Result doc = run(arguments);
    ASSERT_EQ(doc.code, 0) << expected;
    EXPECT_NE(doc.output.find(expected), std::string::npos) << expected;
  }
}

TEST(AppTest, ReportsUserAndGitErrors) {
  EXPECT_EQ(run({"wat"}).code, 2);
  EXPECT_NE(run({"wat"}).error.find("subcommand is required"),
            std::string::npos);
  EXPECT_EQ(run({"-R", "."}).code, 2);
}

TEST(AppTest, ShowsAllTopLevelCommands) {
  const Result help = run({});
  const Result markdown = run({"util", "markdown-help"});
  const Result completion = run({"util", "completion", "bash"});

  ASSERT_EQ(help.code, 0);
  for (std::string_view command :
       {"status", "diff", "show", "clone", "init", "pull", "sparse"}) {
    EXPECT_NE(help.output.find("  " + std::string(command)), std::string::npos)
        << command;
    EXPECT_NE(markdown.output.find("## `gg " + std::string(command)),
              std::string::npos)
        << command;
    EXPECT_NE(completion.output.find(" " + std::string(command) + " "),
              std::string::npos)
        << command;
  }
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
  EXPECT_EQ(invoke({"push", "--dry-run"}).code, 0);
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
  EXPECT_EQ(invoke({"workspace", "add"}).code, 2);
  EXPECT_EQ(invoke({"next", "0"}).code, 2);
  EXPECT_EQ(invoke({"prev", "word"}).code, 2);
  EXPECT_EQ(invoke({"next", "--no-edit"}).code, 2);
  EXPECT_EQ(invoke({"prev", "--conflict", "2"}).code, 2);
  EXPECT_EQ(invoke({"unknown"}).code, 2);
  EXPECT_EQ(invoke({"continue"}).code, 2);
  EXPECT_EQ(invoke({"abort"}).code, 2);
}

TEST_F(RepositoryTest, SupportsQuietAndDebugOutputModes) {
  const Result debug = invoke({"--debug", "status"});
  EXPECT_FALSE(debug.output.empty());
  EXPECT_NE(debug.error.find("debug: repository " + path_.string()),
            std::string::npos);
  EXPECT_NE(debug.error.find("debug: command status"), std::string::npos);

  const Result quiet_status = invoke({"--quiet", "status"});
  EXPECT_FALSE(quiet_status.output.empty());
  const Result quiet_new = invoke({"--quiet", "new", "main"});
  EXPECT_EQ(quiet_new.code, 0) << quiet_new.error;
  EXPECT_TRUE(quiet_new.output.empty());
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

TEST_F(RepositoryTest, ReportsOneTimeRepositoryInitialization) {
  const std::string message =
      "Initializing gg for this repository; this may take a moment...\n";
  const Result first = invoke({"log", "--limit", "1"});
  ASSERT_EQ(first.code, 0) << first.error;
  EXPECT_EQ(first.error, message);
  const Result second = invoke({"log", "--limit", "1"});
  ASSERT_EQ(second.code, 0) << second.error;
  EXPECT_TRUE(second.error.empty());
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

TEST_F(RepositoryTest, PullPassesArgumentsAndExitStatusToGit) {
  const char* previous_path = std::getenv("PATH");
  ASSERT_NE(previous_path, nullptr);
  const std::string saved_path = previous_path;
  write("fake-bin/git",
        "#!/bin/sh\nprintf '%s\\n' \"$@\" > "
        "\"$GG_WORKSPACE_ROOT/pull-arguments\"\nexit 7\n");
  const std::filesystem::path git = path_ / "fake-bin/git";
  std::filesystem::permissions(
      git, std::filesystem::perms::owner_exec |
               std::filesystem::perms::owner_read |
               std::filesystem::perms::owner_write);
  ASSERT_EQ(setenv("PATH", (git.parent_path().string() + ":" + saved_path).c_str(),
                   1),
            0);
  const Result result =
      invoke({"--debug", "pull", "-h", "--rebase", "origin", "main"});
  ASSERT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);

  EXPECT_EQ(result.code, 7) << result.error;
  EXPECT_NE(result.error.find("debug: command pull -h --rebase origin main"),
            std::string::npos);
  std::ifstream input(path_ / "pull-arguments");
  std::ostringstream arguments;
  arguments << input.rdbuf();
  EXPECT_EQ(arguments.str(),
            "-C\n" + path_.string() +
                "\npull\n-h\n--rebase\norigin\nmain\n");
}

TEST_F(RepositoryTest, GeneratesMarkdownAndManPageHelp) {
  const Result markdown = run({"util", "markdown-help"});
  ASSERT_EQ(markdown.code, 0) << markdown.error;
  EXPECT_NE(markdown.output.find("# gg command reference"), std::string::npos);
  EXPECT_NE(markdown.output.find("## `gg bookmark move`"), std::string::npos);
  EXPECT_NE(markdown.output.find("```text\n"), std::string::npos);
  EXPECT_NE(markdown.output.find("Backwards or sideways movement requires"),
            std::string::npos);
  EXPECT_EQ(markdown.output.find("TOML"), std::string::npos);
  EXPECT_EQ(run({"util", "markdown-help"}).output, markdown.output);

  std::size_t documented = 0;
  for (std::size_t heading = markdown.output.find("## `gg");
       heading != std::string::npos;) {
    const std::size_t next = markdown.output.find("\n## `gg", heading + 1);
    const std::string_view section(markdown.output.data() + heading,
                                   (next == std::string::npos
                                        ? markdown.output.size()
                                        : next) -
                                       heading);
    const bool semantic =
        section.find("DETAILS:") != std::string_view::npos ||
        section.find("DEFAULTS:") != std::string_view::npos ||
        section.find("WORKING MODEL:") != std::string_view::npos;
    EXPECT_TRUE(semantic) << section.substr(0, section.find('\n'));
    ++documented;
    heading = next == std::string::npos ? next : next + 1;
  }
  EXPECT_GT(documented, 60u);

  const std::size_t move_start = markdown.output.find("## `gg bookmark move`");
  const std::size_t move_end = markdown.output.find("\n## `gg", move_start + 1);
  ASSERT_NE(move_start, std::string::npos);
  EXPECT_EQ(occurrences(
                std::string_view(markdown.output).substr(
                    move_start, move_end == std::string::npos
                                    ? move_end
                                    : move_end - move_start),
                "Move existing bookmarks"),
            1u);

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
  EXPECT_NE(content.str().find("Backwards or sideways movement requires"),
            std::string::npos);
  EXPECT_EQ(occurrences(content.str(), "Move existing bookmarks"), 1u);
  EXPECT_EQ(run({"util", "install-man-pages"}).code, 2);
}

TEST(AppTest, ConfigDocumentationUsesNativeGitValues) {
  const Result doc = run({"config", "set", "--doc"});
  ASSERT_EQ(doc.code, 0);
  EXPECT_NE(doc.output.find("Native Git configuration value"),
            std::string::npos);
  EXPECT_NE(doc.output.find("verbatim as a native Git configuration string"),
            std::string::npos);
  EXPECT_EQ(doc.output.find("TOML"), std::string::npos);
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

TEST(AppTest, RejectsRemovedConfigurationAndTemplateInterfaces) {
  EXPECT_EQ(run({"--config", "demo.value=x", "status"}).code, 2);
  EXPECT_EQ(run({"--config-file", "config.toml", "status"}).code, 2);
  EXPECT_EQ(run({"util", "config-schema"}).code, 2);
  EXPECT_EQ(run({"log", "--template", "description"}).code, 2);
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

  const Result default_gc = invoke({"util", "gc"});
  ASSERT_EQ(default_gc.code, 0) << default_gc.error;
  EXPECT_NE(default_gc.output.find("Garbage collection completed."),
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
