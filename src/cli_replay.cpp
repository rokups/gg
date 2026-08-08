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

template <typename... Values>
struct Overloaded : Values... {
  using Values::operator()...;
};

}  // namespace

std::vector<std::string> repository_replay_arguments(
    const RepositoryCommand& command) {
  return std::visit(
      Overloaded{
          [](const StatusCommand&) { return std::vector<std::string>{"status"}; },
          [](const LogCommand& value) {
            std::vector<std::string> result{"log"};
            add_option(result, "-r", value.revision);
            return result;
          },
          [](const NewCommand& value) {
            std::vector<std::string> result{"new"};
            add_option(result, "-m", value.message);
            result.insert(result.end(), value.parents.begin(), value.parents.end());
            return result;
          },
          [](const DescribeCommand& value) {
            std::vector<std::string> result{"describe", "-m", value.message};
            if (!value.revision.empty()) {
              result.push_back(value.revision);
            }
            return result;
          },
          [](const EditCommand& value) {
            return std::vector<std::string>{"edit", value.revision};
          },
          [](const RebaseCommand& value) {
            return std::vector<std::string>{"rebase", "-s", value.source, "-d",
                                            value.destination};
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
            if (!value.revision.empty()) {
              result.push_back(value.revision);
            }
            return result;
          },
          [](const BookmarkCommand& value) {
            std::vector<std::string> result{"bookmark"};
            switch (value.action) {
              case BookmarkAction::list:
                result.emplace_back("list");
                break;
              case BookmarkAction::create:
                result.emplace_back("create");
                break;
              case BookmarkAction::set:
                result.emplace_back("set");
                break;
              case BookmarkAction::erase:
                result.emplace_back("delete");
                break;
            }
            result.insert(result.end(), value.names.begin(), value.names.end());
            add_option(result, "-r", value.revision);
            return result;
          },
          [](const GitFetchCommand& value) {
            std::vector<std::string> result{"fetch"};
            add_option(result, "--remote", value.remote);
            return result;
          },
          [](const GitPushCommand& value) {
            std::vector<std::string> result{"push", "--bookmark",
                                            value.bookmark};
            add_option(result, "--remote", value.remote);
            return result;
          },
          [](const UndoCommand&) { return std::vector<std::string>{"undo"}; },
          [](const RedoCommand&) { return std::vector<std::string>{"redo"}; },
          [](const OperationLogCommand&) {
            return std::vector<std::string>{"operation", "log"};
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
          [](const UtilSnapshotCommand&) {
            return std::vector<std::string>{"util", "snapshot"};
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
            if (value.no_edit) {
              result.emplace_back("--no-edit");
            }
            if (value.conflict) {
              result.emplace_back("--conflict");
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
            std::vector<std::string> result{"clone", value.url};
            if (!value.destination.empty()) {
              result.push_back(value.destination);
            }
            return result;
          },
          [](const ContinueCommand&) {
            return std::vector<std::string>{"continue"};
          },
          [](const AbortCommand&) { return std::vector<std::string>{"abort"}; }},
      command);
}

}  // namespace gg::detail
