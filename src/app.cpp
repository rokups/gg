// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "cli.hpp"
#include "commands.hpp"

#include <ostream>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

namespace gg::detail {
namespace {

template <typename... Values>
struct Overloaded : Values... {
  using Values::operator()...;
};

OutputColorMode output_color_mode(std::string_view requested,
                                  std::ostream& output) {
  if (requested == "always") return OutputColorMode::ansi;
  if (requested == "debug") return OutputColorMode::debug;
  if (requested == "auto") {
    const bool terminal_stream = &output == &std::cout;
    const bool terminal = isatty(STDOUT_FILENO) != 0;
    const bool color = terminal_stream && terminal;  // GG_COV_EXCL_BRANCH
    return color ? OutputColorMode::ansi  // GG_COV_EXCL_BRANCH
                 : OutputColorMode::plain;  // GG_COV_EXCL_BRANCH
  }
  return OutputColorMode::plain;
}

bool has_primary_output(const Command& command) {
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, RepositoryCommand>) {
          return std::visit(
              [](const auto& repository_value) {
                using RepositoryValue =
                    std::decay_t<decltype(repository_value)>;
                if constexpr (std::is_same_v<RepositoryValue, StatusCommand> ||
                              std::is_same_v<RepositoryValue, LogCommand> ||
                              std::is_same_v<RepositoryValue, DiffCommand> ||
                              std::is_same_v<RepositoryValue, ShowCommand> ||
                              std::is_same_v<RepositoryValue,
                                             OperationLogCommand>) {
                  return true;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    WorkspaceCommand>) {
                  return repository_value.action == WorkspaceAction::list ||
                         repository_value.action == WorkspaceAction::root;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    SparseCommand>) {
                  return repository_value.action == SparseAction::list;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    FileCommand>) {
                  return repository_value.action == FileAction::list ||
                         repository_value.action == FileAction::show ||
                         repository_value.action == FileAction::search;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    BookmarkCommand>) {
                  return repository_value.action == BookmarkAction::list;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    TagCommand>) {
                  return repository_value.action == TagAction::list;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    ConfigCommand>) {
                  return repository_value.action == ConfigAction::get ||
                         repository_value.action == ConfigAction::list ||
                         repository_value.action == ConfigAction::path;
                } else if constexpr (std::is_same_v<RepositoryValue,
                                                    GitPushCommand>) {
                  return repository_value.dry_run;
                } else {
                  return false;
                }
              },
              value);
        } else if constexpr (std::is_same_v<Value, UtilExecCommand>) {
          return true;
        } else {
          return false;
        }
      },
      command);
}

bool reads_revisions(const Command& command) {
  const auto* repository_command = std::get_if<RepositoryCommand>(&command);
  if (repository_command == nullptr) return false;  // GG_COV_EXCL_BRANCH
  return std::visit(
      [](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, StatusCommand> ||
                      std::is_same_v<Value, LogCommand> ||
                      std::is_same_v<Value, DiffCommand> ||
                      std::is_same_v<Value, ShowCommand>) {
          return true;
        } else if constexpr (std::is_same_v<Value, FileCommand>) {
          return value.action == FileAction::list ||
                 value.action == FileAction::show ||
                 value.action == FileAction::search;
        } else if constexpr (std::is_same_v<Value, BookmarkCommand>) {
          return value.action == BookmarkAction::list;
        } else if constexpr (std::is_same_v<Value, TagCommand>) {
          return value.action == TagAction::list;
        } else {
          return false;
        }
      },
      *repository_command);
}

int execute(Repository& repository,
            const RepositoryCommand& command,
            const std::vector<std::string>&,
            std::ostream& output,
            std::ostream& status_output) {
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
            [&](const MetaeditCommand& value) {
              command_metaedit(repository, value, output);
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
            [&](const UtilGcCommand& value) {
              command_util_gc(repository, value, output);
            },
            [&](const UtilSnapshotCommand&) {
              command_util_snapshot(repository, output);
            },
            [&](const UtilInstallGitHooksCommand&) {
              command_util_install_git_hooks(repository, output);
            },
            [&](const UtilCheckPushConflictsCommand&) {
              command_util_check_push_conflicts(repository, std::cin);
            },
            [&](const WorkspaceCommand& value) {
              command_workspace(repository, value, output);
            },
            [&](const SparseCommand& value) {
              command_sparse(repository, value, output);
            },
            [&](const MovementCommand& value) {
              command_move(repository, value, output);
            },
            [&](const ConfigCommand& value) {
              command_config(repository, value, output);
            }},
        command);
  const auto workspace = repository.workspace();
  if (!std::holds_alternative<UtilCheckPushConflictsCommand>(command) &&
      workspace.has_value() && repository.commit_has_conflicts(*workspace)) {
    status_output << "Warning: the working-copy change has unresolved "
                     "conflicts.\n";
  }
  return 0;
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
  const OutputColorMode color = output_color_mode(invocation.color, output);
  set_output_color_mode(output, color);
  set_output_color_mode(error, color);
  if (invocation.debug) {
    error << "debug: repository " << invocation.repository.string() << '\n';
    error << "debug: command";
    for (const std::string& argument : invocation.replay_arguments) {
      error << ' ' << argument;
    }
    error << '\n';
  }
  std::ostringstream discarded;
  const bool primary_output = has_primary_output(invocation.command);
  std::ostream& command_output =
      invocation.quiet && !primary_output
          ? static_cast<std::ostream&>(discarded)
          : output;
  if (!invocation.at_operation.empty()) {
    const auto* repository_command =
        std::get_if<RepositoryCommand>(&invocation.command);
    if (repository_command == nullptr || !primary_output) {
      throw UserError("--at-operation only supports read-only commands");
    }
  }
  if (const auto* clone = std::get_if<GitCloneCommand>(&invocation.command)) {
    return clone_command(*clone, command_output);
  }
  if (const auto* init = std::get_if<GitInitCommand>(&invocation.command)) {
    return init_command(*init, command_output);
  }
  if (const auto* pull = std::get_if<GitPullCommand>(&invocation.command)) {
    UtilExecCommand git_pull{"git",
                             {"-C", invocation.repository.string(), "pull"}};
    git_pull.arguments.insert(git_pull.arguments.end(),
                              pull->arguments.begin(), pull->arguments.end());
    const int result = command_util_exec(git_pull, invocation.repository);
    if (result != 0) return result;
    Repository repository(invocation.repository,
                          invocation.ignore_working_copy);
    repository.enable_ref_cache();
    repository.sync_for_command();
    return 0;
  }
  if (const auto* util_exec =
          std::get_if<UtilExecCommand>(&invocation.command)) {
    return command_util_exec(*util_exec, invocation.repository);
  }

  Repository repository(invocation.repository,
                        invocation.ignore_working_copy ||
                            !invocation.at_operation.empty());
  repository.enable_ref_cache();
  if (!invocation.at_operation.empty()) {
    repository.view_at_operation(invocation.at_operation);
  }
  if (repository.has_legacy_rewrite()) {
    throw UserError("legacy paused rewrite found; use the previous gg version "
                    "to abort it before upgrading");
  }
  if (invocation.at_operation.empty() && reads_revisions(invocation.command)) {
    repository.import_git_history(&error);
  }
  return execute(repository, std::get<RepositoryCommand>(invocation.command),
                 invocation.replay_arguments, command_output, output);
}

}  // namespace

int run_cli(std::span<const std::string_view> arguments,
            std::ostream& output,
            std::ostream& error) {
  try {
    Libgit2 libgit2;
    return dispatch(arguments, output, error);
  } catch (const UserError& exception) {  // GG_COV_EXCL_BRANCH
    error << styled(error, "error:", OutputStyle::removed)
          << ' ' << exception.what() << '\n';
    return 2;
  } catch (const std::exception& exception) {
    error << styled(error, "error:", OutputStyle::removed)
          << ' ' << exception.what() << '\n';
    return 1;
  }
}

}  // namespace gg::detail
