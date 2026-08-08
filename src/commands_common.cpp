// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2/sys/errors.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <utility>

extern char** environ;

namespace gg::detail {

std::vector<git_oid> commit_parents(Repository& repo,
                                    const std::vector<std::string>& revisions) {
  std::vector<git_oid> parents;
  parents.reserve(revisions.size());
  for (const std::string& revision : revisions) {
    parents.push_back(repo.resolve(revision));
  }
  return parents;
}

git_oid combined_tree(Repository& repo, const std::vector<git_oid>& parents) {
  if (parents.empty()) {
    return repo.empty_tree();
  }
  CommitPtr first = repo.commit(parents.front());
  git_oid result = *git_commit_tree_id(first.get());
  for (std::size_t index = 1; index < parents.size(); ++index) {
    git_oid ancestor{};
    const int base = git_merge_base(&ancestor, repo.raw(), &parents.front(),
                                    &parents[index]);
    const git_oid ancestor_tree =
        base == GIT_ENOTFOUND
            ? repo.empty_tree()
            : *git_commit_tree_id(repo.commit(ancestor).get());
    if (base != GIT_ENOTFOUND) {
      check(base, "find merge base");
    } else {
      git_error_clear();
    }
    const git_oid their_tree =
        *git_commit_tree_id(repo.commit(parents[index]).get());
    result = repo.merge_trees(ancestor_tree, result, their_tree);
  }
  return result;
}

void finish_workspace(Repository& repo,
                      const git_oid& workspace,
                      std::map<std::string, git_oid> updates,
                      std::set<std::string> deletes,
                      std::string_view operation) {
  updates[std::string(kWorkspaceRef)] = workspace;
  const HeadState head = repo.head_for_workspace(workspace);
  repo.record(std::move(updates), std::move(deletes), head, operation);
  repo.set_head(head);
  repo.checkout(workspace);
}

void edit_file_with_editor(const std::filesystem::path& path) {
  const char* raw_editor = std::getenv("VISUAL");
  if (raw_editor == nullptr) raw_editor = std::getenv("EDITOR");
  if (raw_editor != nullptr && *raw_editor == '\0') {
    raw_editor = std::getenv("EDITOR");
  }
  if (raw_editor == nullptr || *raw_editor == '\0') {
    throw UserError("VISUAL or EDITOR must name an editor executable");
  }
  std::string editor(raw_editor);
  std::string file = path.string();
  char* arguments[] = {editor.data(), file.data(), nullptr};
  pid_t process = 0;
  const int spawned =
      posix_spawnp(&process, editor.c_str(), nullptr, nullptr, arguments, environ);
  if (spawned != 0) throw UserError("cannot launch editor");
  int status = 0;
  if (waitpid(process, &status, 0) < 0) throw UserError("cannot wait for editor");  // GG_COV_EXCL_BRANCH
  if (!WIFEXITED(status)) throw UserError("editor exited unsuccessfully");  // GG_COV_EXCL_BRANCH
  if (WEXITSTATUS(status) != 0) {
    throw UserError("editor exited unsuccessfully");
  }
}

std::string edit_text(std::string_view initial) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "gg-edit-XXXXXX").string();
  const int descriptor = mkstemp(pattern.data());
  if (descriptor < 0) throw UserError("cannot create editor file");  // GG_COV_EXCL_BRANCH
  close(descriptor);
  std::ofstream(pattern) << initial;
  try {
    edit_file_with_editor(pattern);
    std::ifstream input(pattern);
    std::string result{std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>()};
    std::filesystem::remove(pattern);
    return result;
  } catch (...) {
    std::filesystem::remove(pattern);
    throw;
  }
}

int command_util_exec(const UtilExecCommand& options,
                      const std::filesystem::path& repository) {
  std::vector<std::string> argument_storage{options.command};
  argument_storage.insert(argument_storage.end(), options.arguments.begin(),
                          options.arguments.end());
  std::vector<char*> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (std::string& argument : argument_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);

  std::vector<std::string> environment_storage;
  std::vector<char*> environment;
  char** child_environment = environ;
  git_repository* raw_repository = nullptr;
  const std::string repository_path = repository.string();
  if (git_repository_open_ext(&raw_repository, repository_path.c_str(), 0,
                              nullptr) == 0) {
    RepositoryPtr discovered(raw_repository);
    if (git_repository_is_bare(discovered.get()) == 0) {
      constexpr std::string_view prefix = "GG_WORKSPACE_ROOT=";
      for (char** entry = environ; *entry != nullptr; ++entry) {
        if (!starts_with(*entry, prefix)) {
          environment_storage.emplace_back(*entry);
        }
      }
      environment_storage.push_back(
          std::string(prefix) +
          std::filesystem::weakly_canonical(
              git_repository_workdir(discovered.get()))
              .string());
      environment.reserve(environment_storage.size() + 1);
      for (std::string& entry : environment_storage) {
        environment.push_back(entry.data());
      }
      environment.push_back(nullptr);
      child_environment = environment.data();
    }
  } else {
    git_error_clear();
  }

  pid_t process = 0;
  const int spawned = posix_spawnp(&process, options.command.c_str(), nullptr,
                                   nullptr, arguments.data(),
                                   child_environment);
  if (spawned != 0) {
    throw UserError("cannot execute external command: " + options.command);
  }
  int status = 0;
  if (waitpid(process, &status, 0) < 0) throw UserError("cannot wait for external command");  // GG_COV_EXCL_BRANCH
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  throw UserError("external command terminated by a signal");
}

void command_util_gc(Repository& repo,
                     const UtilGcCommand& options,
                     std::ostream& output) {
  repo.sync_workspace();
  UtilExecCommand command{
      "git",
      {"--git-dir=" + std::string(git_repository_path(repo.raw())), "gc"}};
  if (!options.expire.empty()) {
    command.arguments.push_back("--prune=" + options.expire);
  }
  const int status =
      command_util_exec(command, git_repository_path(repo.raw()));
  if (status != 0) throw UserError("Git garbage collection failed");  // GG_COV_EXCL_BRANCH
  output << "Garbage collection completed.\n";
}

void command_util_snapshot(Repository& repo, std::ostream& output) {
  output << (repo.sync_workspace() ? "Created working-copy snapshot.\n"
                                   : "Nothing changed.\n");
}

void command_workspace(Repository& repo,
                       const WorkspaceCommand& options,
                       std::ostream& output) {
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(git_repository_workdir(repo.raw()));
  const auto workspace = repo.ref_target(kWorkspaceRef);
  if (options.action == WorkspaceAction::root) {
    if (!options.name.empty() && options.name != "default") {
      throw UserError("workspace not found: " + options.name);
    }
    if (!options.name.empty() && !workspace.has_value()) {
      throw UserError("workspace not found: " + options.name);
    }
    output << root.string() << '\n';
    return;
  }
  if (!options.template_value.empty()) {
    throw UserError("workspace templates are not supported yet");
  }
  if (!workspace.has_value()) {
    output << "No workspaces.\n";
    return;
  }
  const auto id = repo.change_id(*workspace);
  output << "default: "
         << (id.has_value() ? repo.short_change_id(*id) : "--------") << ' '
         << oid_string(*workspace, 8) << ' ' << root.string() << '\n';
}

}  // namespace gg::detail
