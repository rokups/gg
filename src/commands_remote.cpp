// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

constexpr std::string_view kUndoPrefix = "undo: restore to operation ";
constexpr std::string_view kRedoPrefix = "redo: restore to operation ";

std::string operation_timestamp(const git_commit* operation) {
  const int offset = git_commit_time_offset(operation);
  const std::chrono::sys_seconds local_time{
      std::chrono::seconds{git_commit_time(operation) + offset * 60}};
  const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(local_time);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss clock{local_time - day};
  const int absolute_offset = std::abs(offset);
  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year())
         << '-' << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(date.day()) << 'T'
         << std::setw(2) << clock.hours().count() << ':' << std::setw(2)
         << clock.minutes().count() << ':' << std::setw(2)
         << clock.seconds().count()
         << (offset < 0 ? '-' : '+')  // GG_COV_EXCL_BRANCH
         << std::setw(2)  // GG_COV_EXCL_BRANCH
         << absolute_offset / 60 << ':' << std::setw(2) << absolute_offset % 60;
  return output.str();
}

}  // namespace

void command_bookmark(Repository& repo,
                      const BookmarkCommand& options,
                      std::ostream& output) {
  repo.sync_workspace();
  if (options.action == BookmarkAction::list) {
    for (const auto& [name, oid] : repo.data_refs()) {
      if (starts_with(name, "refs/heads/")) {
        output << name.substr(std::string_view("refs/heads/").size()) << ": "
               << oid_string(oid, 8) << '\n';
      }
    }
    return;
  }
  if (options.action == BookmarkAction::erase) {
    std::set<std::string> deletes;
    for (const std::string& name : options.names) {
      const std::string reference = "refs/heads/" + name;
      if (!repo.ref_target(reference).has_value()) {
        throw UserError("bookmark not found: " + name);
      }
      deletes.insert(reference);
    }
    repo.record({}, deletes, repo.head_state(), "gg bookmark delete");
    output << "Deleted " << deletes.size() << " bookmark(s).\n";
    return;
  }
  const std::string& name = options.names.front();
  const std::string reference = "refs/heads/" + name;
  int valid = 0;
  check(git_reference_name_is_valid(&valid, reference.c_str()),
        "validate bookmark name");
  if (valid == 0) {
    throw UserError("invalid bookmark name: " + name);
  }
  if (options.action == BookmarkAction::create &&
      repo.ref_target(reference).has_value()) {
    throw UserError("bookmark already exists: " + name);
  }
  const git_oid target =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  repo.record({{reference, target}}, {}, repo.head_state(), "gg bookmark");
  output << (options.action == BookmarkAction::create ? "Created " : "Moved ")
         << name << " at " << oid_string(target, 8) << '\n';
}

int credentials(git_credential** output,
                const char* url,
                const char* username,
                unsigned int allowed,
                void* payload) {
  (void)url;
  (void)payload;
  if ((allowed & GIT_CREDENTIAL_SSH_KEY) != 0 && username != nullptr) {
    return git_credential_ssh_key_from_agent(output, username);
  }
  if ((allowed & GIT_CREDENTIAL_DEFAULT) != 0) {
    return git_credential_default_new(output);
  }
  return GIT_PASSTHROUGH;
}

git_remote_callbacks remote_callbacks() {
  git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
  callbacks.credentials = credentials;
  return callbacks;
}

void command_fetch(Repository& repo,
                   const GitFetchCommand& options,
                   std::ostream& output) {
  repo.sync_workspace();
  git_remote* raw_remote = nullptr;
  const std::string name = options.remote.empty() ? "origin" : options.remote;
  check(git_remote_lookup(&raw_remote, repo.raw(), name.c_str()), "find remote");
  RemotePtr remote(raw_remote);
  git_fetch_options fetch_options = GIT_FETCH_OPTIONS_INIT;
  fetch_options.callbacks = remote_callbacks();
  check(git_remote_fetch(remote.get(), nullptr, &fetch_options, "gg fetch"),
        "fetch remote");
  repo.record({}, {}, repo.head_state(), "gg fetch");
  output << "Fetched " << name << '\n';
}

void command_push(Repository& repo,
                  const GitPushCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  const std::string reference = "refs/heads/" + options.bookmark;
  if (!repo.ref_target(reference).has_value()) {
    throw UserError("bookmark not found: " + options.bookmark);
  }
  const std::string name = options.remote.empty() ? "origin" : options.remote;
  git_remote* raw_remote = nullptr;
  check(git_remote_lookup(&raw_remote, repo.raw(), name.c_str()), "find remote");
  RemotePtr remote(raw_remote);
  const std::string refspec = reference + ":" + reference;
  char* value = const_cast<char*>(refspec.c_str());
  git_strarray refspecs{&value, 1};
  git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
  push_options.callbacks = remote_callbacks();
  check(git_remote_push(remote.get(), &refspecs, &push_options),
        "push bookmark");
  output << "Pushed bookmark " << options.bookmark << " to " << name << '\n';
}

void command_undo(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  const auto current = repo.operation();
  if (!current.has_value()) {
    throw UserError("nothing to undo");
  }
  const git_oid target =
      repo.operation_target(*current, kUndoPrefix).value_or(*current);
  auto previous = repo.operation_previous(target);
  if (!previous.has_value()) {
    throw UserError("nothing to undo");
  }
  previous = repo.operation_target(*previous, kUndoPrefix).value_or(*previous);
  repo.restore_operation(
      *previous, std::string(kUndoPrefix) + oid_string(*previous));
  output << "Undid operation.\n";
}

void command_redo(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  const auto current = repo.operation();
  if (!current.has_value()) {
    throw UserError("nothing to redo");
  }
  const git_oid target =
      repo.operation_target(*current, kRedoPrefix).value_or(*current);
  if (!repo.operation_target(target, kUndoPrefix).has_value()) {
    throw UserError("nothing to redo");
  }
  auto restored = repo.operation_previous(target);
  if (!restored.has_value()) {  // GG_COV_EXCL_BRANCH
    throw GitError("undo operation has no predecessor");
  }
  restored = repo.operation_target(*restored, kRedoPrefix).value_or(*restored);
  repo.restore_operation(
      *restored, std::string(kRedoPrefix) + oid_string(*restored));
  output << "Redid operation.\n";
}

void command_operation_log(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  auto current = repo.operation();
  if (!current.has_value()) {
    output << "No operations.\n";
    return;
  }
  bool first = true;
  while (current.has_value()) {
    CommitPtr operation = repo.commit(*current);
    const std::string description = repo.operation_description(*current);
    const auto previous = repo.operation_previous(*current);
    output << (first ? "@ " : "○ ") << oid_string(*current, 8) << ' '
           << operation_timestamp(operation.get()) << ' ' << description << '\n';
    if (previous.has_value()) {
      output << "│\n";
    }
    first = false;
    current = previous;
  }
}

int clone_command(const GitCloneCommand& options, std::ostream& output) {
  std::string destination = options.destination;
  if (destination.empty()) {
    destination = std::filesystem::path(options.url).filename().string();
    if (destination.ends_with(".git")) {
      destination.resize(destination.size() - 4);
    }
  }
  git_clone_options clone_options = GIT_CLONE_OPTIONS_INIT;
  clone_options.fetch_opts.callbacks = remote_callbacks();
  git_repository* raw_repository = nullptr;
  check(git_clone(&raw_repository, options.url.c_str(), destination.c_str(),
                  &clone_options),
        "clone repository");
  RepositoryPtr repository(raw_repository);
  output << "Cloned into " << destination << '\n';
  return 0;
}

}  // namespace gg::detail
