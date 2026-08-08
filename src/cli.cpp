// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "cli.hpp"

#include <CLI/CLI.hpp>

#include <ostream>
#include <utility>

namespace gg::detail {

ParseResult parse_cli(std::span<const std::string_view> arguments,
                      std::ostream& output,
                      std::ostream& error) {
  CLI::App app{"A JJ-shaped interface over ordinary Git repositories", "gg"};
  app.require_subcommand(1);
  app.set_version_flag("--version", "gg 0.1.0");

  std::string repository = ".";
  app.add_option("-R,--repository", repository, "Repository path");

  StatusCommand status_value;
  auto* status = app.add_subcommand("status", "Show the working-copy change");
  status->alias("st");

  LogCommand log_value;
  auto* log = app.add_subcommand("log", "Show revision history");
  log->add_option("-r,--revisions", log_value.revision, "Starting revision");

  NewCommand new_value;
  auto* make_new = app.add_subcommand("new", "Create and edit an empty change");
  make_new->add_option("-m,--message", new_value.message, "Description");
  make_new->add_option("parents", new_value.parents, "Parent revisions");

  DescribeCommand describe_value;
  auto* describe = app.add_subcommand("describe", "Set a change description");
  describe->add_option("-m,--message", describe_value.message, "Description")
      ->required();
  describe->add_option("revision", describe_value.revision, "Revision")
      ->expected(0, 1);

  EditCommand edit_value;
  auto* edit = app.add_subcommand("edit", "Edit an existing change");
  edit->add_option("revision", edit_value.revision, "Revision")->required();

  RebaseCommand rebase_value;
  auto* rebase = app.add_subcommand("rebase", "Move a change and descendants");
  rebase->add_option("-s,--source", rebase_value.source, "Source revision")
      ->required();
  rebase
      ->add_option("-d,--destination", rebase_value.destination,
                   "Destination revision")
      ->required();

  SplitCommand split_value;
  auto* split = app.add_subcommand("split", "Split selected paths");
  split->add_option("-r,--revision", split_value.revision, "Revision");
  split->add_option("-m,--message", split_value.message, "Description");
  split->add_option("paths", split_value.paths, "Repository-relative paths")
      ->required();

  SquashCommand squash_value;
  auto* squash = app.add_subcommand("squash", "Move a change into its parent");
  CLI::Option* squash_revision =
      squash->add_option("-r,--revision", squash_value.revision, "Revision");
  CLI::Option* squash_source =
      squash->add_option("-f,--from", squash_value.source, "Source revision");
  CLI::Option* squash_destination = squash->add_option(
      "-t,--into", squash_value.destination, "Destination revision");
  squash_revision->excludes(squash_source)->excludes(squash_destination);
  squash->add_option("-m,--message", squash_value.message, "Description");

  AbandonCommand abandon_value;
  auto* abandon = app.add_subcommand("abandon", "Abandon a change");
  abandon->add_option("revision", abandon_value.revision, "Revision")
      ->expected(0, 1);

  auto* bookmark = app.add_subcommand("bookmark", "Manage Git-backed bookmarks");
  bookmark->require_subcommand(0, 1);
  BookmarkCommand bookmark_create;
  bookmark_create.action = BookmarkAction::create;
  auto* create = bookmark->add_subcommand("create", "Create a bookmark");
  create->add_option("name", bookmark_create.names, "Bookmark name")
      ->required()
      ->expected(1);
  create->add_option("-r,--revision", bookmark_create.revision, "Revision");
  BookmarkCommand bookmark_set;
  bookmark_set.action = BookmarkAction::set;
  auto* set = bookmark->add_subcommand("set", "Set a bookmark");
  set->add_option("name", bookmark_set.names, "Bookmark name")
      ->required()
      ->expected(1);
  set->add_option("-r,--revision", bookmark_set.revision, "Revision");
  BookmarkCommand bookmark_delete;
  bookmark_delete.action = BookmarkAction::erase;
  auto* erase = bookmark->add_subcommand("delete", "Delete bookmarks");
  erase->add_option("names", bookmark_delete.names, "Bookmark names")
      ->required();
  BookmarkCommand bookmark_list;
  auto* list = bookmark->add_subcommand("list", "List bookmarks");

  GitCloneCommand clone_value;
  auto* clone = app.add_subcommand("clone", "Clone a Git repository");
  clone->add_option("url", clone_value.url, "Repository URL")->required();
  clone->add_option("destination", clone_value.destination, "Destination")
      ->expected(0, 1);
  GitFetchCommand fetch_value;
  auto* fetch = app.add_subcommand("fetch", "Fetch a Git remote");
  fetch->add_option("--remote", fetch_value.remote, "Remote name");
  GitPushCommand push_value;
  auto* push = app.add_subcommand("push", "Push a bookmark");
  push->add_option("-b,--bookmark", push_value.bookmark, "Bookmark name")
      ->required();
  push->add_option("--remote", push_value.remote, "Remote name");

  auto* continue_rewrite =
      app.add_subcommand("continue", "Continue a paused rewrite");
  auto* abort_rewrite = app.add_subcommand("abort", "Abort a paused rewrite");
  auto* undo = app.add_subcommand("undo", "Restore the previous operation");
  auto* redo = app.add_subcommand("redo", "Redo the most recently undone operation");
  auto* operation = app.add_subcommand("operation", "Manage operation history");
  operation->alias("op");
  operation->require_subcommand(1);
  auto* operation_log =
      operation->add_subcommand("log", "Show the operation log");
  OperationRestoreCommand operation_restore_value;
  auto* operation_restore = operation->add_subcommand(
      "restore", "Restore the repository to an earlier operation");
  operation_restore
      ->add_option("operation", operation_restore_value.operation, "Operation ID")
      ->required();
  operation_restore
      ->add_option("--what", operation_restore_value.what,
                   "State to restore: repo or remote-tracking")
      ->check(CLI::IsMember({"repo", "remote-tracking"}));
  auto* help = app.add_subcommand("help", "Print help");
  auto* version = app.add_subcommand("version", "Print version");

  if (arguments.empty()) {
    output << app.help();
    return {0, std::monostate{}};
  }

  std::vector<std::string> storage{"gg"};
  storage.reserve(arguments.size() + 1);
  for (std::string_view argument : arguments) {
    storage.emplace_back(argument);
  }
  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (std::string& argument : storage) {
    argv.push_back(argument.data());
  }

  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::CallForHelp&) {  // GG_COV_EXCL_BRANCH
    output << app.help();
    return {0, std::monostate{}};
  } catch (const CLI::CallForVersion&) {
    output << "gg 0.1.0\n";
    return {0, std::monostate{}};
  } catch (const CLI::ParseError& exception) {
    error << "error: " << exception.what() << '\n';
    return {2, std::monostate{}};
  }

  if (help->parsed()) {
    output << app.help();
    return {0, std::monostate{}};
  }
  if (version->parsed()) {
    output << "gg 0.1.0\n";
    return {0, std::monostate{}};
  }

  Command command = RepositoryCommand{status_value};
  if (status->parsed()) {
    command = RepositoryCommand{status_value};
  } else if (log->parsed()) {
    command = RepositoryCommand{std::move(log_value)};
  } else if (make_new->parsed()) {
    command = RepositoryCommand{std::move(new_value)};
  } else if (describe->parsed()) {
    command = RepositoryCommand{std::move(describe_value)};
  } else if (edit->parsed()) {
    command = RepositoryCommand{std::move(edit_value)};
  } else if (rebase->parsed()) {
    command = RepositoryCommand{std::move(rebase_value)};
  } else if (split->parsed()) {
    command = RepositoryCommand{std::move(split_value)};
  } else if (squash->parsed()) {
    command = RepositoryCommand{std::move(squash_value)};
  } else if (abandon->parsed()) {
    command = RepositoryCommand{std::move(abandon_value)};
  } else if (create->parsed()) {
    command = RepositoryCommand{std::move(bookmark_create)};
  } else if (set->parsed()) {
    command = RepositoryCommand{std::move(bookmark_set)};
  } else if (erase->parsed()) {
    command = RepositoryCommand{std::move(bookmark_delete)};
  } else if (list->parsed() || bookmark->parsed()) {
    command = RepositoryCommand{std::move(bookmark_list)};
  } else if (clone->parsed()) {
    command = std::move(clone_value);
  } else if (fetch->parsed()) {
    command = RepositoryCommand{std::move(fetch_value)};
  } else if (push->parsed()) {
    command = RepositoryCommand{std::move(push_value)};
  } else if (continue_rewrite->parsed()) {
    command = ContinueCommand{};
  } else if (abort_rewrite->parsed()) {
    command = AbortCommand{};
  } else if (undo->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{UndoCommand{}};
  } else if (redo->parsed()) {
    command = RepositoryCommand{RedoCommand{}};
  } else if (operation_log->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{OperationLogCommand{}};
  } else if (operation_restore->parsed()) {  // GG_COV_EXCL_BRANCH
    command = RepositoryCommand{std::move(operation_restore_value)};
  }

  Invocation invocation{repository, std::move(command), {}};
  invocation.replay_arguments = replay_arguments(invocation.command);
  return {-1, std::move(invocation)};
}

}  // namespace gg::detail
