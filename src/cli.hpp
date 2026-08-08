// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <limits>
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
  bool retain_bookmarks{false};
  bool restore_descendants{false};
};
struct CommitCommand {
  std::vector<std::string> paths;
  std::string message;
  std::string tool;
  bool interactive{false};
  bool editor{false};
  bool message_provided{false};
};
struct RestoreCommand {
  std::vector<std::string> paths;
  std::string from;
  std::string into;
  std::string changes_in;
  std::string tool;
  bool interactive{false};
  bool restore_descendants{false};
};
struct SimplifyParentsCommand {
  std::vector<std::string> sources;
  std::vector<std::string> revisions;
};

enum class FileAction { list, show, search, chmod };
struct FileCommand {
  FileAction action{FileAction::list};
  std::string revision{"@"};
  std::vector<std::string> paths;
  std::string template_value;
  std::string pattern;
  std::string mode;
  bool name_only{false};
  bool line_number{false};
};

struct DiffFormatOptions {
  bool summary{false};
  bool stat{false};
  bool types{false};
  bool name_only{false};
  bool git{false};
  bool color_words{false};
  bool ignore_all_space{false};
  bool ignore_space_change{false};
  std::uint32_t context{3};
  std::string tool;
};
struct DiffCommand {
  std::vector<std::string> paths;
  std::string revisions;
  std::string from;
  std::string to;
  std::string template_value;
  DiffFormatOptions format;
};
struct ShowCommand {
  std::vector<std::string> revisions;
  std::vector<std::string> revision_options;
  std::string template_value;
  DiffFormatOptions format;
  bool reversed{false};
  bool no_patch{false};
};

enum class BookmarkAction { list, create, set, erase, forget, rename };
struct BookmarkCommand {
  BookmarkAction action{BookmarkAction::list};
  std::string revision;
  std::vector<std::string> names;
  bool allow_backwards{false};
  bool include_remotes{false};
  bool overwrite_existing{false};
};

enum class TagAction { list, set, erase };
struct TagCommand {
  TagAction action{TagAction::list};
  std::vector<std::string> names;
  std::string revision;
  std::string remote;
  std::string template_value;
  std::string sort;
  bool allow_move{false};
  bool all_remotes{false};
  bool tracked{false};
  bool conflicted{false};
};

struct GitCloneCommand {
  std::string url;
  std::string destination;
};
struct GitInitCommand {
  std::string destination;
  std::string object_hash{"sha1"};
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
struct OperationLogCommand {
  std::uint64_t limit{std::numeric_limits<std::uint64_t>::max()};
  bool reversed{false};
  bool no_graph{false};
  std::string template_value;
};
struct OperationRestoreCommand {
  std::string operation;
  std::vector<std::string> what;
};
struct UtilSnapshotCommand {};

enum class WorkspaceAction { list, root };
struct WorkspaceCommand {
  WorkspaceAction action{WorkspaceAction::list};
  std::string name;
  std::string template_value;
};

enum class MovementDirection { next, previous };
struct MovementCommand {
  MovementDirection direction{MovementDirection::next};
  std::uint64_t offset{1};
  bool edit{false};
  bool no_edit{false};
  bool conflict{false};
};

using RepositoryCommand =
    std::variant<StatusCommand, LogCommand, NewCommand, DescribeCommand,
                 EditCommand, RebaseCommand, SplitCommand, SquashCommand,
                 AbandonCommand, CommitCommand, RestoreCommand,
                 SimplifyParentsCommand, FileCommand, DiffCommand, ShowCommand,
                 BookmarkCommand, TagCommand,
                 GitFetchCommand, GitPushCommand, UndoCommand, RedoCommand,
                 OperationLogCommand, OperationRestoreCommand,
                 UtilSnapshotCommand, WorkspaceCommand, MovementCommand>;
using Command = std::variant<RepositoryCommand, GitCloneCommand, GitInitCommand,
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
