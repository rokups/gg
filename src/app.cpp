// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "gg/app.hpp"

#include "cli.hpp"
#include "commands.hpp"

#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace gg::detail {
namespace {

template <typename... Values>
struct Overloaded : Values... {
  using Values::operator()...;
};

std::vector<std::string_view> views(const std::vector<std::string>& values) {
  std::vector<std::string_view> result;
  result.reserve(values.size());
  for (const std::string& value : values) {
    result.push_back(value);
  }
  return result;
}

int execute(Repository& repository,
            const RepositoryCommand& command,
            const std::vector<std::string>& replay,
            std::ostream& output) {
  try {
    std::visit(
        Overloaded{
            [&](const StatusCommand& value) {
              command_status(repository, value, output);
            },
            [&](const LogCommand& value) {
              command_log(repository, value, output);
            },
            [&](const NewCommand& value) {
              command_new(repository, value, output);
            },
            [&](const DescribeCommand& value) {
              command_describe(repository, value, output);
            },
            [&](const EditCommand& value) {
              command_edit(repository, value, output);
            },
            [&](const RebaseCommand& value) {
              command_rebase(repository, value, output);
            },
            [&](const SplitCommand& value) {
              command_split(repository, value, output);
            },
            [&](const SquashCommand& value) {
              command_squash(repository, value, output);
            },
            [&](const AbandonCommand& value) {
              command_abandon(repository, value, output);
            },
            [&](const CommitCommand& value) {
              command_commit(repository, value, output);
            },
            [&](const RestoreCommand& value) {
              command_restore(repository, value, output);
            },
            [&](const SimplifyParentsCommand& value) {
              command_simplify_parents(repository, value, output);
            },
            [&](const FileCommand& value) {
              command_file(repository, value, output);
            },
            [&](const DiffCommand& value) {
              command_diff(repository, value, output);
            },
            [&](const ShowCommand& value) {
              command_show(repository, value, output);
            },
            [&](const BookmarkCommand& value) {
              command_bookmark(repository, value, output);
            },
            [&](const TagCommand& value) {
              command_tag(repository, value, output);
            },
            [&](const GitFetchCommand& value) {
              command_fetch(repository, value, output);
            },
            [&](const GitPushCommand& value) {
              command_push(repository, value, output);
            },
            [&](const UndoCommand&) { command_undo(repository, output); },
            [&](const RedoCommand&) { command_redo(repository, output); },
            [&](const OperationLogCommand& value) {
              command_operation_log(repository, value, output);
            },
            [&](const OperationRestoreCommand& value) {
              command_operation_restore(repository, value, output);
            },
            [&](const UtilSnapshotCommand&) {
              command_util_snapshot(repository, output);
            },
            [&](const WorkspaceCommand& value) {
              command_workspace(repository, value, output);
            },
            [&](const MovementCommand& value) {
              command_move(repository, value, output);
            },
            [&](const ConfigCommand& value) {
              command_config(repository, value, output);
            }},
        command);
    return 0;
  } catch (MergeConflict& conflict) {
    const std::vector<std::string_view> replay_views = views(replay);
    repository.pause(replay_views, conflict);
    output << "Rewrite stopped because of conflicts.\n";
    repository.pending_status(output);
    return 1;
  }
}

int dispatch(std::span<const std::string_view> arguments,
             std::ostream& output,
             std::ostream& error) {
  ParseResult parsed = parse_cli(arguments, output, error);
  if (parsed.exit_code >= 0) {
    return parsed.exit_code;
  }
  Invocation invocation =
      std::move(std::get<Invocation>(parsed.invocation));
  if (const auto* clone = std::get_if<GitCloneCommand>(&invocation.command)) {
    return clone_command(*clone, output);
  }
  if (const auto* init = std::get_if<GitInitCommand>(&invocation.command)) {
    return init_command(*init, output);
  }
  if (const auto* util_exec =
          std::get_if<UtilExecCommand>(&invocation.command)) {
    return command_util_exec(*util_exec, invocation.repository);
  }

  Repository repository(invocation.repository);
  if (repository.pending().has_value()) {
    const auto* repository_command =
        std::get_if<RepositoryCommand>(&invocation.command);
    if (repository_command != nullptr &&
        std::holds_alternative<StatusCommand>(*repository_command)) {
      repository.pending_status(output);
      return 0;
    }
    if (std::holds_alternative<AbortCommand>(invocation.command)) {
      repository.abort_rewrite();
      output << "Aborted rewrite.\n";
      return 0;
    }
    if (std::holds_alternative<ContinueCommand>(invocation.command)) {
      std::vector<std::string> stored = repository.prepare_continue();
      const std::vector<std::string_view> stored_views = views(stored);
      ParseResult resumed = parse_cli(stored_views, output, error);
      Invocation resumed_invocation =
          std::move(std::get<Invocation>(resumed.invocation));
      const int result = execute(
          repository,
          std::get<RepositoryCommand>(resumed_invocation.command), stored,
          output);
      if (result == 0) {
        repository.finish_rewrite();
      }
      return result;
    }
    throw UserError("a rewrite is in progress; run `gg continue` or `gg abort`");
  }
  if (std::holds_alternative<ContinueCommand>(invocation.command) ||
      std::holds_alternative<AbortCommand>(invocation.command)) {
    throw UserError("no rewrite is in progress");
  }
  return execute(repository, std::get<RepositoryCommand>(invocation.command),
                 invocation.replay_arguments, output);
}

}  // namespace
}  // namespace gg::detail

namespace gg {

int run(std::span<const std::string_view> arguments,
        std::ostream& output,
        std::ostream& error) {
  try {
    detail::Libgit2 libgit2;
    return detail::dispatch(arguments, output, error);
  } catch (const detail::UserError& exception) {  // GG_COV_EXCL_BRANCH
    error << "error: " << exception.what() << '\n';
    return 2;
  } catch (const std::exception& exception) {
    error << "error: " << exception.what() << '\n';
    return 1;
  }
}

}  // namespace gg
