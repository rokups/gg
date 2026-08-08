// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>
#include <git2/sys/errors.h>

#include <algorithm>
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
  if (options.action == BookmarkAction::erase ||
      options.action == BookmarkAction::forget) {
    std::set<std::string> deletes;
    for (const std::string& name : options.names) {
      const std::string reference = "refs/heads/" + name;
      if (!repo.ref_target(reference).has_value()) {
        throw UserError("bookmark not found: " + name);
      }
      deletes.insert(reference);
      if (options.include_remotes) {
        const std::string suffix = "/" + name;
        for (const auto& [remote, oid] : repo.data_refs()) {
          (void)oid;
          if (starts_with(remote, "refs/remotes/") &&
              remote.ends_with(suffix)) {
            deletes.insert(remote);
          }
        }
      }
    }
    repo.record({}, deletes, repo.head_state(),
                options.action == BookmarkAction::erase ? "gg bookmark delete"
                                                        : "gg bookmark forget");
    output << (options.action == BookmarkAction::erase ? "Deleted " : "Forgot ")
           << deletes.size() << " bookmark ref(s).\n";
    return;
  }
  if (options.action == BookmarkAction::rename) {
    if (options.names[0] == options.names[1]) {
      throw UserError("bookmark names must differ");
    }
    const std::string old_reference = "refs/heads/" + options.names[0];
    const std::string new_reference = "refs/heads/" + options.names[1];
    const auto target = repo.ref_target(old_reference);
    if (!target.has_value()) {
      throw UserError("bookmark not found: " + options.names[0]);
    }
    int valid = 0;
    check(git_reference_name_is_valid(&valid, new_reference.c_str()),
          "validate bookmark name");
    if (valid == 0) {
      throw UserError("invalid bookmark name: " + options.names[1]);
    }
    if (!options.overwrite_existing &&
        repo.ref_target(new_reference).has_value()) {
      throw UserError("bookmark already exists: " + options.names[1]);
    }
    repo.record({{new_reference, *target}}, {old_reference}, repo.head_state(),
                "gg bookmark rename");
    output << "Renamed " << options.names[0] << " to " << options.names[1]
           << ".\n";
    return;
  }
  const git_oid target =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  std::map<std::string, git_oid> updates;
  for (const std::string& name : options.names) {
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
    const auto current = repo.ref_target(reference);
    if (options.action == BookmarkAction::set && current.has_value() &&
        !(*current == target) && !options.allow_backwards) {
      const int is_descendant =
          git_graph_descendant_of(repo.raw(), &target, &*current);
      check(is_descendant, "check bookmark movement");
      if (is_descendant == 0) {
        throw UserError("refusing to move bookmark backwards: " + name);
      }
    }
    updates.emplace(reference, target);
  }
  repo.record(std::move(updates), {}, repo.head_state(), "gg bookmark");
  for (const std::string& name : options.names) {
    output << (options.action == BookmarkAction::create ? "Created " : "Moved ")
           << name << " at " << oid_string(target, 8) << '\n';
  }
}

void command_tag(Repository& repo,
                 const TagCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
  const auto tag_target = [&](const std::string& name) -> std::optional<git_oid> {
    git_object* raw_object = nullptr;
    const std::string reference = "refs/tags/" + name;
    const int lookup =
        git_revparse_single(&raw_object, repo.raw(), reference.c_str());
    if (lookup == GIT_ENOTFOUND) {
      git_error_clear();
      return std::nullopt;
    }
    check(lookup, "read tag");
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
          "resolve tag");
    ObjectPtr commit(raw_commit);
    return *git_object_id(commit.get());
  };

  if (options.action == TagAction::list) {
    if (options.all_remotes || !options.remote.empty() || options.tracked ||
        options.conflicted) {
      throw UserError("remote tag state is not supported yet");
    }
    if (!options.template_value.empty()) {
      throw UserError("tag templates are not supported yet");
    }
    if (!options.sort.empty()) {
      throw UserError("tag sort keys are not supported yet");
    }
    std::optional<git_oid> revision;
    if (!options.revision.empty()) {
      revision = repo.resolve(options.revision);
    }
    for (const auto& [reference, ref_oid] : repo.data_refs()) {
      (void)ref_oid;
      if (!starts_with(reference, "refs/tags/")) {
        continue;
      }
      const std::string name =
          reference.substr(std::string_view("refs/tags/").size());
      if (!options.names.empty() &&
          std::find(options.names.begin(), options.names.end(), name) ==
              options.names.end()) {
        continue;
      }
      const std::optional<git_oid> target = tag_target(name);
      if (revision.has_value() && !(*revision == *target)) {
        continue;
      }
      output << name << ": " << oid_string(*target, 8) << '\n';
    }
    return;
  }

  if (options.action == TagAction::erase) {
    std::set<std::string> deletes;
    for (const std::string& name : options.names) {
      const std::string reference = "refs/tags/" + name;
      if (!repo.ref_target(reference).has_value()) {
        throw UserError("tag not found: " + name);
      }
      deletes.insert(reference);
    }
    repo.record({}, deletes, repo.head_state(), "gg tag delete");
    output << "Deleted " << deletes.size() << " tag(s).\n";
    return;
  }

  const git_oid target =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  std::map<std::string, git_oid> updates;
  for (const std::string& name : options.names) {
    const std::string reference = "refs/tags/" + name;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, reference.c_str()),
          "validate tag name");
    if (valid == 0) {
      throw UserError("invalid tag name: " + name);
    }
    const std::optional<git_oid> current = tag_target(name);
    if (current.has_value() && !(*current == target) && !options.allow_move) {
      throw UserError("tag already exists at a different revision: " + name);
    }
    updates.emplace(reference, target);
  }
  repo.record(std::move(updates), {}, repo.head_state(), "gg tag set");
  for (const std::string& name : options.names) {
    output << "Set " << name << " at " << oid_string(target, 8) << '\n';
  }
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

void command_operation_log(Repository& repo,
                           const OperationLogCommand& options,
                           std::ostream& output) {
  repo.sync_workspace();
  auto current = repo.operation();
  if (!current.has_value()) {
    output << "No operations.\n";
    return;
  }
  if (!options.template_value.empty()) {
    throw UserError("operation templates are not supported yet");
  }
  const git_oid newest = *current;
  std::vector<git_oid> operations;
  while (current.has_value() && operations.size() < options.limit) {
    operations.push_back(*current);
    current = repo.operation_previous(*current);
  }
  if (options.reversed) {
    std::reverse(operations.begin(), operations.end());
  }
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const git_oid& oid = operations[index];
    CommitPtr operation = repo.commit(oid);
    const std::string description = repo.operation_description(oid);
    if (!options.no_graph) {
      output << (oid == newest ? "@ " : "○ ");
    }
    output << oid_string(oid, 8) << ' ' << operation_timestamp(operation.get())
           << ' ' << description << '\n';
    if (!options.no_graph && index + 1 < operations.size()) {
      output << "│\n";
    }
  }
}

void command_operation_restore(Repository& repo,
                               const OperationRestoreCommand& options,
                               std::ostream& output) {
  repo.sync_workspace();
  const git_oid operation = repo.resolve_operation(options.operation);
  const bool all = options.what.empty();
  const bool restore_repository =
      all || std::find(options.what.begin(), options.what.end(), "repo") !=
                 options.what.end();
  const bool restore_remote_tracking =
      all || std::find(options.what.begin(), options.what.end(),
                       "remote-tracking") != options.what.end();
  repo.restore_operation(operation,
                         "restore to operation " + oid_string(operation),
                         restore_repository, restore_remote_tracking);
  output << "Restored operation " << oid_string(operation, 8) << ".\n";
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

int init_command(const GitInitCommand& options, std::ostream& output) {
  git_repository_init_options init_options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  init_options.flags = GIT_REPOSITORY_INIT_MKPATH;
  if (options.object_hash == "sha256") {
    throw UserError("gg does not support SHA-256 repositories");
  }
  const std::filesystem::path destination =
      options.destination.empty() ? "." : options.destination;
  git_repository* raw_repository = nullptr;
  check(git_repository_init_ext(&raw_repository, destination.string().c_str(),
                                &init_options),
        "initialize repository");
  RepositoryPtr initialized(raw_repository);
  initialized.reset();

  Repository repo(destination);
  output << "Initialized repository at "
         << std::filesystem::weakly_canonical(destination).string() << '\n';
  if (!repo.ref_target(kWorkspaceRef).has_value()) {
    command_new(repo, NewCommand{}, output);
  }
  return 0;
}

}  // namespace gg::detail
