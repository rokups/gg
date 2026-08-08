// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <filesystem>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gg::detail {

struct StatusCommand {};
struct LogCommand {
  std::string revision;
};
struct NewCommand {
  std::string message;
  std::vector<std::string> parents;
};
struct DescribeCommand {
  std::string message;
  std::string revision;
};
struct EditCommand {
  std::string revision;
};
struct RebaseCommand {
  std::string source;
  std::string destination;
};
struct SplitCommand {
  std::string revision;
  std::string message;
  std::vector<std::string> paths;
};
struct SquashCommand {
  std::string revision;
  std::string source;
  std::string destination;
  std::string message;
};
struct AbandonCommand {
  std::string revision;
};

enum class BookmarkAction { list, create, set, erase };
struct BookmarkCommand {
  BookmarkAction action{BookmarkAction::list};
  std::string revision;
  std::vector<std::string> names;
};

struct GitCloneCommand {
  std::string url;
  std::string destination;
};
struct GitFetchCommand {
  std::string remote;
};
struct GitPushCommand {
  std::string bookmark;
  std::string remote;
};
struct ContinueCommand {};
struct AbortCommand {};
struct UndoCommand {};
struct RedoCommand {};
struct OperationLogCommand {};

using RepositoryCommand =
    std::variant<StatusCommand, LogCommand, NewCommand, DescribeCommand,
                 EditCommand, RebaseCommand, SplitCommand, SquashCommand,
                 AbandonCommand, BookmarkCommand, GitFetchCommand,
                 GitPushCommand, UndoCommand, RedoCommand,
                 OperationLogCommand>;
using Command = std::variant<RepositoryCommand, GitCloneCommand,
                             ContinueCommand, AbortCommand>;

struct Invocation {
  std::filesystem::path repository{"."};
  Command command;
  std::vector<std::string> replay_arguments;
};

struct ParseResult {
  int exit_code{-1};
  std::variant<std::monostate, Invocation> invocation;
};

ParseResult parse_cli(std::span<const std::string_view> arguments,
                      std::ostream& output,
                      std::ostream& error);
std::vector<std::string> replay_arguments(const Command& command);

}  // namespace gg::detail
