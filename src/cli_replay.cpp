// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "cli.hpp"

#include <string_view>

namespace gg::detail {
namespace {

void add_option(std::vector<std::string>& arguments,
                std::string_view name,
                const std::string& value) {
  if (!value.empty()) {
    arguments.emplace_back(name);
    arguments.push_back(value);
  }
}

void add_diff_format(std::vector<std::string>& arguments,
                     const DiffFormatOptions& value) {
  if (value.summary) arguments.emplace_back("--summary");
  if (value.stat) arguments.emplace_back("--stat");
  if (value.types) arguments.emplace_back("--types");
  if (value.name_only) arguments.emplace_back("--name-only");
  if (value.git) arguments.emplace_back("--git");
  if (value.color_words) arguments.emplace_back("--color-words");
  add_option(arguments, "--tool", value.tool);
  if (value.context != 3) {
    arguments.emplace_back("--context");
    arguments.push_back(std::to_string(value.context));
  }
  if (value.ignore_all_space) arguments.emplace_back("--ignore-all-space");
  if (value.ignore_space_change) {
    arguments.emplace_back("--ignore-space-change");
  }
}

template <typename... Values>
struct Overloaded : Values... {
  using Values::operator()...;
};

}  // namespace

std::vector<std::string> repository_replay_arguments(
    const RepositoryCommand& command) {
  return std::visit(
      Overloaded{
          [](const StatusCommand& value) {
            std::vector<std::string> result{"status"};
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            return result;
          },
          [](const LogCommand& value) {
            std::vector<std::string> result{"log"};
            add_option(result, "-r", value.revision);
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            if (value.limit != std::numeric_limits<std::uint64_t>::max()) {
              result.emplace_back("--limit");
              result.push_back(std::to_string(value.limit));
            }
            if (value.reversed) result.emplace_back("--reversed");
            if (value.no_graph) result.emplace_back("--no-graph");
            if (value.patch) result.emplace_back("--patch");
            if (value.count) result.emplace_back("--count");
            add_diff_format(result, value.format);
            return result;
          },
          [](const NewCommand& value) {
            std::vector<std::string> result{"new"};
            add_option(result, "-m", value.message);
            result.insert(result.end(), value.parents.begin(), value.parents.end());
            for (const std::string& revision : value.insert_after) {
              add_option(result, "--insert-after", revision);
            }
            for (const std::string& revision : value.insert_before) {
              add_option(result, "--insert-before", revision);
            }
            if (value.no_edit) result.emplace_back("--no-edit");
            return result;
          },
          [](const DescribeCommand& value) {
            std::vector<std::string> result{"describe"};
            if (value.message_provided) {
              result.emplace_back("--message");
              result.push_back(value.message);
            }
            if (value.stdin_value) result.emplace_back("--stdin");
            if (value.editor) result.emplace_back("--editor");
            result.insert(result.end(), value.revisions.begin(),
                          value.revisions.end());
            result.insert(result.end(), value.revision_options.begin(),
                          value.revision_options.end());
            return result;
          },
          [](const EditCommand& value) {
            return std::vector<std::string>{"edit", value.revision};
          },
          [](const MetaeditCommand& value) {
            std::vector<std::string> result{"metaedit"};
            result.insert(result.end(), value.revisions.begin(),
                          value.revisions.end());
            for (const std::string& revision : value.revision_options) {
              result.emplace_back("-r");
              result.push_back(revision);
            }
            if (value.message_provided) {
              result.emplace_back("--message");
              result.push_back(value.message);
            }
            if (value.update_author_timestamp) {
              result.emplace_back("--update-author-timestamp");
            }
            if (value.update_author) result.emplace_back("--update-author");
            if (value.author_provided) {
              result.emplace_back("--author");
              result.push_back(value.author);
            }
            if (value.author_timestamp_provided) {
              result.emplace_back("--author-timestamp");
              result.push_back(value.author_timestamp);
            }
            if (value.force_rewrite) result.emplace_back("--force-rewrite");
            return result;
          },
          [](const RebaseCommand& value) {
            return std::vector<std::string>{"rebase", "-s", value.source, "-d",
                                            value.destination};
          },
          [](const DuplicateCommand& value) {
            std::vector<std::string> result{"duplicate", "-r", value.revision};
            if (value.descendants) result.emplace_back("--descendants");
            return result;
          },
          [](const SplitCommand& value) {
            std::vector<std::string> result{"split"};
            add_option(result, "-r", value.revision);
            add_option(result, "-m", value.message);
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            return result;
          },
          [](const SquashCommand& value) {
            std::vector<std::string> result{"squash"};
            add_option(result, "-r", value.revision);
            add_option(result, "--from", value.source);
            add_option(result, "--into", value.destination);
            add_option(result, "-m", value.message);
            return result;
          },
          [](const AbandonCommand& value) {
            std::vector<std::string> result{"abandon"};
            result.insert(result.end(), value.revisions.begin(),
                          value.revisions.end());
            result.insert(result.end(), value.revision_options.begin(),
                          value.revision_options.end());
            if (value.retain_bookmarks) {
              result.emplace_back("--retain-bookmarks");
            }
            if (value.restore_descendants) {
              result.emplace_back("--restore-descendants");
            }
            return result;
          },
          [](const CommitCommand& value) {
            std::vector<std::string> result{"commit"};
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            if (value.message_provided) {
              result.emplace_back("--message");
              result.push_back(value.message);
            }
            add_option(result, "--tool", value.tool);
            if (value.interactive) result.emplace_back("--interactive");
            if (value.editor) result.emplace_back("--editor");
            return result;
          },
          [](const RestoreCommand& value) {
            std::vector<std::string> result{"restore"};
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            add_option(result, "--from", value.from);
            add_option(result, "--into", value.into);
            add_option(result, "--changes-in", value.changes_in);
            add_option(result, "--tool", value.tool);
            if (value.interactive) result.emplace_back("--interactive");
            if (value.restore_descendants) {
              result.emplace_back("--restore-descendants");
            }
            return result;
          },
          [](const SimplifyParentsCommand& value) {
            std::vector<std::string> result{"simplify-parents"};
            for (const std::string& source : value.sources) {
              result.emplace_back("--source");
              result.push_back(source);
            }
            for (const std::string& revision : value.revisions) {
              result.emplace_back("--revision");
              result.push_back(revision);
            }
            return result;
          },
          [](const FileCommand& value) {
            std::vector<std::string> result{"file"};
            switch (value.action) {
              case FileAction::list:
                result.emplace_back("list");
                break;
              case FileAction::show:
                result.emplace_back("show");
                break;
              case FileAction::search:
                result.emplace_back("search");
                break;
              case FileAction::chmod:
                result.emplace_back("chmod");
                result.push_back(value.mode);
                break;
              case FileAction::track:
                result.emplace_back("track");
                break;
              case FileAction::untrack:
                result.emplace_back("untrack");
                break;
            }
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            if (value.revision != "@") {
              add_option(result, "-r", value.revision);
            }
            add_option(result, "--pattern", value.pattern);
            if (value.name_only) {
              result.emplace_back("--name-only");
            }
            if (value.line_number) {
              result.emplace_back("--line-number");
            }
            if (value.include_ignored) {
              result.emplace_back("--include-ignored");
            }
            return result;
          },
          [](const DiffCommand& value) {
            std::vector<std::string> result{"diff"};
            result.insert(result.end(), value.paths.begin(), value.paths.end());
            add_option(result, "--revisions", value.revisions);
            add_option(result, "--from", value.from);
            add_option(result, "--to", value.to);
            add_diff_format(result, value.format);
            return result;
          },
          [](const ShowCommand& value) {
            std::vector<std::string> result{"show"};
            result.insert(result.end(), value.revisions.begin(),
                          value.revisions.end());
            for (const std::string& revision : value.revision_options) {
              result.emplace_back("-r");
              result.push_back(revision);
            }
            if (value.reversed) result.emplace_back("--reversed");
            add_diff_format(result, value.format);
            if (value.no_patch) result.emplace_back("--no-patch");
            return result;
          },
          [](const BookmarkCommand& value) {
            std::vector<std::string> result{"bookmark"};
            switch (value.action) {
              case BookmarkAction::list:
                result.emplace_back("list");
                break;
              case BookmarkAction::advance:
                result.emplace_back("advance");
                break;
              case BookmarkAction::create:
                result.emplace_back("create");
                break;
              case BookmarkAction::set:
                result.emplace_back("set");
                break;
              case BookmarkAction::move:
                result.emplace_back("move");
                break;
              case BookmarkAction::erase:
                result.emplace_back("delete");
                break;
              case BookmarkAction::forget:
                result.emplace_back("forget");
                break;
              case BookmarkAction::rename:
                result.emplace_back("rename");
                break;
              case BookmarkAction::track:
                result.emplace_back("track");
                break;
              case BookmarkAction::untrack:
                result.emplace_back("untrack");
                break;
            }
            result.insert(result.end(), value.names.begin(), value.names.end());
            for (const std::string& remote : value.remotes) {
              result.emplace_back("--remote");
              result.push_back(remote);
            }
            for (const std::string& revision : value.revisions) {
              result.emplace_back("--revision");
              result.push_back(revision);
            }
            for (const std::string& key : value.sort) {
              result.emplace_back("--sort");
              result.push_back(key);
            }
            add_option(result,
                       value.action == BookmarkAction::advance ||
                               value.action == BookmarkAction::move
                           ? "-t"
                           : "-r",
                       value.revision);
            for (const std::string& revision : value.from) {
              result.emplace_back("--from");
              result.push_back(revision);
            }
            if (value.allow_backwards) {
              result.emplace_back("--allow-backwards");
            }
            if (value.all_remotes) result.emplace_back("--all-remotes");
            if (value.tracked) result.emplace_back("--tracked");
            if (value.conflicted) result.emplace_back("--conflicted");
            if (value.include_remotes) {
              result.emplace_back("--include-remotes");
            }
            if (value.overwrite_existing) {
              result.emplace_back("--overwrite-existing");
            }
            return result;
          },
          [](const TagCommand& value) {
            std::vector<std::string> result{"tag"};
            if (value.action == TagAction::set) {
              result.emplace_back("set");
            } else if (value.action == TagAction::erase) {
              result.emplace_back("delete");
            } else if (value.action == TagAction::track) {
              result.emplace_back("track");
            } else if (value.action == TagAction::untrack) {
              result.emplace_back("untrack");
            } else {
              result.emplace_back("list");
            }
            result.insert(result.end(), value.names.begin(), value.names.end());
            add_option(result, "--revision", value.revision);
            for (const std::string& remote : value.remotes) {
              result.emplace_back("--remote");
              result.push_back(remote);
            }
            for (const std::string& revision : value.revisions) {
              result.emplace_back("--revision");
              result.push_back(revision);
            }
            for (const std::string& key : value.sort) {
              result.emplace_back("--sort");
              result.push_back(key);
            }
            if (value.allow_move) result.emplace_back("--allow-move");
            if (value.all_remotes) result.emplace_back("--all-remotes");
            if (value.tracked) result.emplace_back("--tracked");
            if (value.conflicted) result.emplace_back("--conflicted");
            return result;
          },
          [](const GitFetchCommand& value) {
            std::vector<std::string> result{"fetch"};
            for (const std::string& branch : value.branches) {
              result.emplace_back("--branch");
              result.push_back(branch);
            }
            for (const std::string& tag : value.tags) {
              result.emplace_back("--tag");
              result.push_back(tag);
            }
            for (const std::string& remote : value.remotes) {
              result.emplace_back("--remote");
              result.push_back(remote);
            }
            if (value.tracked) result.emplace_back("--tracked");
            if (value.all_remotes) result.emplace_back("--all-remotes");
            return result;
          },
          [](const GitPushCommand& value) {
            std::vector<std::string> result{"push"};
            for (const std::string& bookmark : value.bookmarks) {
              result.emplace_back("--bookmark");
              result.push_back(bookmark);
            }
            for (const std::string& tag : value.tags) {
              result.emplace_back("--tag");
              result.push_back(tag);
            }
            for (const std::string& revision : value.revisions) {
              result.emplace_back("--revision");
              result.push_back(revision);
            }
            for (const std::string& option : value.options) {
              result.emplace_back("--option");
              result.push_back(option);
            }
            add_option(result, "--remote", value.remote);
            if (value.all) result.emplace_back("--all");
            if (value.tracked) result.emplace_back("--tracked");
            if (value.deleted) result.emplace_back("--deleted");
            if (value.allow_empty_description) {
              result.emplace_back("--allow-empty-description");
            }
            if (value.dry_run) result.emplace_back("--dry-run");
            return result;
          },
          [](const UndoCommand&) { return std::vector<std::string>{"undo"}; },
          [](const RedoCommand&) { return std::vector<std::string>{"redo"}; },
          [](const OperationLogCommand& value) {
            std::vector<std::string> result{"operation", "log"};
            if (value.limit != std::numeric_limits<std::uint64_t>::max()) {
              result.emplace_back("--limit");
              result.push_back(std::to_string(value.limit));
            }
            if (value.reversed) result.emplace_back("--reversed");
            if (value.no_graph) result.emplace_back("--no-graph");
            if (value.op_diff) result.emplace_back("--op-diff");
            if (value.patch) result.emplace_back("--patch");
            add_diff_format(result, value.format);
            add_option(result, "--show-changes-in", value.show_changes_in);
            return result;
          },
          [](const OperationRestoreCommand& value) {
            std::vector<std::string> result{"operation", "restore"};
            for (const std::string& what : value.what) {
              result.emplace_back("--what");
              result.push_back(what);
            }
            result.push_back(value.operation);
            return result;
          },
          [](const UtilGcCommand& value) {
            std::vector<std::string> result{"util", "gc"};
            add_option(result, "--expire", value.expire);
            return result;
          },
          [](const UtilSnapshotCommand&) {
            return std::vector<std::string>{"util", "snapshot"};
          },
          [](const UtilInstallGitHooksCommand&) {
            return std::vector<std::string>{"util", "install-git-hooks"};
          },
          [](const UtilCheckPushConflictsCommand&) {
            return std::vector<std::string>{"util", "check-push-conflicts"};
          },
          [](const WorkspaceCommand& value) {
            std::vector<std::string> result{"workspace"};
            if (value.action == WorkspaceAction::list) {
              result.emplace_back("list");
            } else if (value.action == WorkspaceAction::root) {
              result.emplace_back("root");
              add_option(result, "--name", value.name);
            } else if (value.action == WorkspaceAction::add) {
              result.emplace_back("add");
              add_option(result, "--name", value.name);
              add_option(result, "--revision", value.revision);
              add_option(result, "--message", value.message);
              if (value.sparse_patterns != "copy") {
                add_option(result, "--sparse-patterns", value.sparse_patterns);
              }
              result.push_back(value.destination);
            } else if (value.action == WorkspaceAction::forget) {
              result.emplace_back("forget");
              result.insert(result.end(), value.names.begin(),
                            value.names.end());
            } else {
              result.emplace_back("rename");
              result.push_back(value.name);
            }
            return result;
          },
          [](const SparseCommand& value) {
            return std::vector<std::string>{
                "sparse",
                value.action == SparseAction::list ? "list" : "reset"};
          },
          [](const MovementCommand& value) {
            std::vector<std::string> result{
                value.direction == MovementDirection::next ? "next" : "prev"};
            if (value.offset != 1) {
              result.push_back(std::to_string(value.offset));
            }
            if (value.edit) {
              result.emplace_back("--edit");
            }
            if (value.conflict) {
              result.emplace_back("--conflict");
            }
            return result;
          },
          [](const ConfigCommand& value) {
            std::vector<std::string> result{"config"};
            switch (value.action) {
              case ConfigAction::edit:
                result.emplace_back("edit");
                break;
              case ConfigAction::get:
                result.emplace_back("get");
                break;
              case ConfigAction::list:
                result.emplace_back("list");
                break;
              case ConfigAction::path:
                result.emplace_back("path");
                break;
              case ConfigAction::set:
                result.emplace_back("set");
                break;
              case ConfigAction::unset:
                result.emplace_back("unset");
                break;
            }
            if (!value.name.empty()) result.push_back(value.name);
            if (value.action == ConfigAction::set) result.push_back(value.value);
            if (value.user) result.emplace_back("--user");
            if (value.repository) result.emplace_back("--repo");
            if (value.workspace) result.emplace_back("--workspace");
            if (value.include_overridden) {
              result.emplace_back("--include-overridden");
            }
            return result;
          }},
      command);
}

std::vector<std::string> replay_arguments(const Command& command) {
  return std::visit(
      Overloaded{
          [](const RepositoryCommand& value) {
            return repository_replay_arguments(value);
          },
          [](const GitCloneCommand& value) {
            std::vector<std::string> result{"clone"};
            if (value.remote != "origin") {
              result.emplace_back("--remote");
              result.push_back(value.remote);
            }
            if (value.depth != 0) {
              result.emplace_back("--depth");
              result.push_back(std::to_string(value.depth));
            }
            for (const std::string& branch : value.branches) {
              result.emplace_back("--branch");
              result.push_back(branch);
            }
            for (const std::string& tag : value.tags) {
              result.emplace_back("--tag");
              result.push_back(tag);
            }
            if (value.object_hash != "sha1") {
              result.emplace_back("--object-hash");
              result.push_back(value.object_hash);
            }
            result.push_back(value.url);
            if (!value.destination.empty()) {
              result.push_back(value.destination);
            }
            return result;
          },
          [](const GitInitCommand& value) {
            std::vector<std::string> result{"init"};
            if (value.object_hash != "sha1") {
              result.emplace_back("--object-hash");
              result.push_back(value.object_hash);
            }
            if (!value.destination.empty()) {
              result.push_back(value.destination);
            }
            return result;
          },
          [](const GitPullCommand& value) {
            std::vector<std::string> result{"pull"};
            result.insert(result.end(), value.arguments.begin(),
                          value.arguments.end());
            return result;
          },
          [](const UtilExecCommand& value) {
            std::vector<std::string> result{"util", "exec", "--",
                                            value.command};
            result.insert(result.end(), value.arguments.begin(),
                          value.arguments.end());
            return result;
          }},
      command);
}

}  // namespace gg::detail
