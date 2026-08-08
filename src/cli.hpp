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

struct StatusCommand {
  std::vector<std::string> paths;
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
struct LogCommand {
  std::string revision;
  std::vector<std::string> paths;
  std::uint64_t limit{std::numeric_limits<std::uint64_t>::max()};
  std::string template_value;
  DiffFormatOptions format;
  bool reversed{false};
  bool no_graph{false};
  bool patch{false};
  bool count{false};
};
struct NewCommand {
  std::string message;
  std::vector<std::string> parents;
  std::string insert_after;
  std::string insert_before;
  bool no_edit{false};
};
struct DescribeCommand {
  std::string message;
  std::string revision;
  bool stdin_value{false};
  bool editor{false};
  bool message_provided{false};
};
struct EditCommand {
  std::string revision;
};
struct MetaeditCommand {
  std::vector<std::string> revisions;
  std::vector<std::string> revision_options;
  std::string message;
  std::string author;
  std::string author_timestamp;
  bool update_change_id{false};
  bool update_author_timestamp{false};
  bool update_author{false};
  bool force_rewrite{false};
  bool message_provided{false};
  bool author_provided{false};
  bool author_timestamp_provided{false};
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

enum class BookmarkAction {
  list,
  advance,
  create,
  set,
  move,
  erase,
  forget,
  rename
};
struct BookmarkCommand {
  BookmarkAction action{BookmarkAction::list};
  std::string revision;
  std::vector<std::string> names;
  std::vector<std::string> from;
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
struct UtilExecCommand {
  std::string command;
  std::vector<std::string> arguments;
};
struct UtilGcCommand {
  std::string expire;
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

enum class ConfigAction { edit, get, list, path, set, unset };
struct ConfigCommand {
  ConfigAction action{ConfigAction::list};
  std::string name;
  std::string value;
  std::string template_value;
  bool user{false};
  bool repository{false};
  bool workspace{false};
  bool include_defaults{false};
  bool include_overridden{false};
};

using RepositoryCommand =
    std::variant<StatusCommand, LogCommand, NewCommand, DescribeCommand,
                 EditCommand, MetaeditCommand, RebaseCommand, SplitCommand,
                 SquashCommand,
                 AbandonCommand, CommitCommand, RestoreCommand,
                 SimplifyParentsCommand, FileCommand, DiffCommand, ShowCommand,
                 BookmarkCommand, TagCommand,
                 GitFetchCommand, GitPushCommand, UndoCommand, RedoCommand,
                 OperationLogCommand, OperationRestoreCommand,
                 UtilGcCommand, UtilSnapshotCommand, WorkspaceCommand,
                 MovementCommand,
                 ConfigCommand>;
using Command = std::variant<RepositoryCommand, GitCloneCommand, GitInitCommand,
                             UtilExecCommand, ContinueCommand, AbortCommand>;

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
