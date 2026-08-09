// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2/sys/errors.h>

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace gg::detail {
namespace {

enum class ConfigScope { user, repository, workspace };
using ConfigPtr = GitPtr<git_config, git_config_free>;
using ConfigIteratorPtr = GitPtr<git_config_iterator, git_config_iterator_free>;

std::optional<ConfigScope> selected_scope(const ConfigCommand& options,
                                          bool required) {
  if (options.user) return ConfigScope::user;
  if (options.repository) return ConfigScope::repository;
  if (options.workspace) return ConfigScope::workspace;
  if (required) {
    throw UserError("configuration scope must be --user, --repo, or --workspace");
  }
  return std::nullopt;
}

std::filesystem::path config_path(Repository& repo, ConfigScope scope) {
  if (scope == ConfigScope::repository) {
    return std::filesystem::path(git_repository_path(repo.raw())) / "config";
  }
  if (scope == ConfigScope::workspace) {
    return std::filesystem::path(git_repository_path(repo.raw())) /
           "config.worktree";
  }
  git_buf path = GIT_BUF_INIT;
  check(git_config_find_global(&path), "locate global Git config");
  const std::filesystem::path result(path.ptr);
  git_buf_dispose(&path);
  return result;
}

ConfigPtr merged_config(Repository& repo) {
  git_config* raw = nullptr;
  check(git_repository_config(&raw, repo.raw()), "open Git config");
  return ConfigPtr(raw);
}

ConfigPtr scoped_config(Repository& repo, ConfigScope scope, bool write) {
  if (scope == ConfigScope::workspace && write) {  // GG_COV_EXCL_BRANCH
    ConfigPtr config = merged_config(repo);
    git_config* raw_local = nullptr;
    check(git_config_open_level(&raw_local, config.get(), GIT_CONFIG_LEVEL_LOCAL),
          "open repository Git config");
    ConfigPtr local(raw_local);
    check(git_config_set_bool(local.get(), "extensions.worktreeConfig", 1),
          "enable worktree Git config");
  }

  const std::filesystem::path path = config_path(repo, scope);
  if (write) std::filesystem::create_directories(path.parent_path());  // GG_COV_EXCL_BRANCH
  git_config* raw = nullptr;
  check(git_config_new(&raw), "create Git config view");
  ConfigPtr config(raw);
  const git_config_level_t level =
      scope == ConfigScope::user       ? GIT_CONFIG_LEVEL_GLOBAL  // GG_COV_EXCL_BRANCH
      : scope == ConfigScope::workspace ? GIT_CONFIG_LEVEL_WORKTREE
                                       : GIT_CONFIG_LEVEL_LOCAL;
  const std::string path_text = path.string();
  check(git_config_add_file_ondisk(config.get(), path_text.c_str(), level,  // GG_COV_EXCL_BRANCH
                                   repo.raw(), write ? 1 : 0),
        "open Git config");
  return config;
}

bool name_matches(std::string_view name, std::string_view filter) {
  return filter.empty() || name == filter ||  // GG_COV_EXCL_BRANCH
         name.starts_with(std::string(filter) + ".");
}

std::string_view level_name(git_config_level_t level) {
  switch (level) { case GIT_CONFIG_LEVEL_PROGRAMDATA: return "programdata"; case GIT_CONFIG_LEVEL_SYSTEM: return "system"; case GIT_CONFIG_LEVEL_XDG: return "xdg"; case GIT_CONFIG_LEVEL_GLOBAL: return "global"; case GIT_CONFIG_LEVEL_LOCAL: return "local"; case GIT_CONFIG_LEVEL_WORKTREE: return "worktree"; case GIT_CONFIG_LEVEL_APP: return "app"; case GIT_CONFIG_HIGHEST_LEVEL: return "unknown"; } return "unknown";  // GG_COV_EXCL_BRANCH
}

void list_config(git_config* config,
                 const ConfigCommand& options,
                 std::ostream& output) {
  git_config_iterator* raw_iterator = nullptr;
  check(git_config_iterator_new(&raw_iterator, config), "list Git config");
  ConfigIteratorPtr iterator(raw_iterator);
  std::vector<std::tuple<std::string, std::string, git_config_level_t,
                         std::string>> entries;
  std::set<std::string> names;
  while (true) {
    git_config_entry* entry = nullptr;
    const int result = git_config_next(&entry, iterator.get());
    if (result == GIT_ITEROVER) break;
    check(result, "read Git config");
    if (!name_matches(entry->name, options.name)) continue;
    entries.emplace_back(entry->name, entry->value, entry->level,
                         entry->origin_path);
    names.insert(entry->name);
  }

  if (options.include_overridden) {
    for (const auto& [name, value, level, origin] : entries) {
      output << level_name(level) << ' ' << origin << ' ' << name << " = "
             << value << '\n';
    }
    return;
  }
  for (const std::string& name : names) {
    git_config_entry* raw_entry = nullptr;
    check(git_config_get_entry(&raw_entry, config, name.c_str()),
          "read Git config value");
    GitPtr<git_config_entry, git_config_entry_free> entry(raw_entry);
    output << name << " = " << entry->value << '\n';
  }
}

}  // namespace

std::optional<std::string> config_value(Repository& repo,
                                        std::string_view name) {
  ConfigPtr config = merged_config(repo);
  git_config_entry* raw_entry = nullptr;
  const int result = git_config_get_entry(&raw_entry, config.get(),
                                          std::string(name).c_str());
  if (result == GIT_ENOTFOUND) {
    git_error_clear();
    return std::nullopt;
  }
  check(result, "read Git config value");
  GitPtr<git_config_entry, git_config_entry_free> entry(raw_entry);
  return std::string(entry->value);
}

void command_config(Repository& repo,
                    const ConfigCommand& options,
                    std::ostream& output) {
  const bool scope_required = options.action == ConfigAction::edit ||
                              options.action == ConfigAction::path ||
                              options.action == ConfigAction::set ||
                              options.action == ConfigAction::unset;
  const std::optional<ConfigScope> scope = selected_scope(options, scope_required);

  if (options.action == ConfigAction::path) {
    output << config_path(repo, *scope).string() << '\n';
    return;
  }
  if (options.action == ConfigAction::edit) {
    if (*scope == ConfigScope::workspace) {
      (void)scoped_config(repo, *scope, true);
    }
    const std::filesystem::path path = config_path(repo, *scope);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::app);
    edit_file_with_editor(repo, path);
    return;
  }

  const bool write = options.action == ConfigAction::set ||
                     options.action == ConfigAction::unset;
  ConfigPtr config = scope.has_value()
                         ? scoped_config(repo, *scope, write)
                         : merged_config(repo);
  if (options.action == ConfigAction::set) {
    check(git_config_set_string(config.get(), options.name.c_str(),
                                options.value.c_str()),
          "set Git config value");
    return;
  }
  if (options.action == ConfigAction::unset) {
    const int result = git_config_delete_entry(config.get(), options.name.c_str());
    if (result == GIT_ENOTFOUND) {
      git_error_clear();
      throw UserError("configuration key not found: " + options.name);
    }
    check(result, "unset Git config value");
    return;
  }
  if (options.action == ConfigAction::get) {
    git_config_entry* raw_entry = nullptr;
    const int result =
        git_config_get_entry(&raw_entry, config.get(), options.name.c_str());
    if (result == GIT_ENOTFOUND) {
      git_error_clear();
      throw UserError("configuration key not found: " + options.name);
    }
    check(result, "read Git config value");
    GitPtr<git_config_entry, git_config_entry_free> entry(raw_entry);
    output << entry->value << '\n';
    return;
  }
  list_config(config.get(), options, output);
}

}  // namespace gg::detail
