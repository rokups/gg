// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <vector>

namespace gg::detail {
namespace {

enum class ConfigScope { user, repository, workspace };

const std::map<std::string, std::string> kDefaultValues{
    {"ui.movement.edit", "false"}};

std::string trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  const std::size_t last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1));
}

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
    return std::filesystem::path(git_repository_path(repo.raw())) / "gg" /
           "config.toml";
  }
  if (scope == ConfigScope::workspace) {
    return std::filesystem::path(git_repository_path(repo.raw())) / "gg" /
           "workspaces" / "default.toml";
  }
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg != nullptr) {
    if (*xdg != '\0') {
      return std::filesystem::path(xdg) / "gg" / "config.toml";
    }
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr) {
    throw UserError("cannot locate user configuration directory");
  }
  if (*home == '\0') {
    throw UserError("cannot locate user configuration directory");
  }
  return std::filesystem::path(home) / ".config" / "gg" / "config.toml";
}

void validate_name(std::string_view name) {
  if (name.empty()) {
    throw UserError("invalid configuration key: " + std::string(name));
  }
  if (name.front() == '.' || name.back() == '.') {
    throw UserError("invalid configuration key: " + std::string(name));
  }
  bool dot = false;
  for (const unsigned char character : name) {
    if (character == '.') {
      if (dot) throw UserError("invalid configuration key: " + std::string(name));
      dot = true;
    } else {
      dot = false;
      const bool allowed = (std::isalnum(character) != 0) |
                           (character == '_') | (character == '-');
      if (!allowed) {
        throw UserError("invalid configuration key: " + std::string(name));
      }
    }
  }
}

void validate_value(std::string_view raw_value) {
  const std::string value = trim(raw_value);
  bool valid = (value == "true") | (value == "false");
  if (value.size() >= 2) {
    valid |= (value.front() == '"') & (value.back() == '"');
    valid |= (value.front() == '\'') & (value.back() == '\'');
    valid |= (value.front() == '[') & (value.back() == ']');
    valid |= (value.front() == '{') & (value.back() == '}');
  }
  char* end = nullptr;
  std::strtod(value.c_str(), &end);
  if (end != value.c_str()) valid |= *end == '\0';
  if (!valid) throw UserError("configuration value must be valid TOML");
}

std::optional<std::pair<std::string, std::string>> parse_assignment(
    std::string_view line) {
  const std::string stripped = trim(line);
  if (stripped.empty()) return std::nullopt;
  if (stripped.front() == '#') return std::nullopt;
  const std::size_t separator = stripped.find('=');
  if (separator == std::string::npos) return std::nullopt;
  const std::string name = trim(std::string_view(stripped).substr(0, separator));
  const std::string value = trim(std::string_view(stripped).substr(separator + 1));
  if (name.empty()) return std::nullopt;
  if (value.empty()) return std::nullopt;
  return std::pair{name, value};
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) lines.push_back(line);
  return lines;
}

std::map<std::string, std::string> read_values(
    const std::filesystem::path& path) {
  std::map<std::string, std::string> values;
  for (const std::string& line : read_lines(path)) {
    if (const auto assignment = parse_assignment(line); assignment.has_value()) {
      values[assignment->first] = assignment->second;
    }
  }
  return values;
}

std::vector<std::pair<std::string, std::string>> runtime_values(
    const Repository& repo) {
  std::vector<std::pair<std::string, std::string>> values;
  for (const std::filesystem::path& path : repo.config_files()) {
    if (!std::filesystem::is_regular_file(path)) {
      throw UserError("cannot read configuration: " + path.string());
    }
    for (const auto& entry : read_values(path)) values.push_back(entry);
  }
  for (const std::string& raw : repo.config_values()) {
    const auto assignment = parse_assignment(raw);
    if (!assignment.has_value()) {
      throw UserError("--config must be NAME=VALUE");
    }
    validate_name(assignment->first);
    const std::string value = trim(assignment->second);
    if (value.front() == '[' || value.front() == '{' || value.front() == '\'' ||
        value.front() == '"') {
      validate_value(value);
    }
    values.push_back(*assignment);
  }
  return values;
}

void write_lines(const std::filesystem::path& path,
                 const std::vector<std::string>& lines) {
  std::filesystem::create_directories(path.parent_path());
  const std::filesystem::path temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw UserError("cannot write configuration: " + path.string());
    for (const std::string& line : lines) output << line << '\n';
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary);
    throw UserError("cannot replace configuration: " + error.message());
  }
}

void set_value(const std::filesystem::path& path,
               const std::string& name,
               const std::string& value) {
  const std::vector<std::string> old_lines = read_lines(path);
  std::vector<std::string> lines;
  bool replaced = false;
  for (const std::string& line : old_lines) {
    const auto assignment = parse_assignment(line);
    if (assignment.has_value()) {
      if (assignment->first == name) {
        if (!replaced) lines.push_back(name + " = " + value);
        replaced = true;
        continue;
      }
    }
    lines.push_back(line);
  }
  if (!replaced) lines.push_back(name + " = " + value);
  write_lines(path, lines);
}

void unset_value(const std::filesystem::path& path, const std::string& name) {
  const std::vector<std::string> old_lines = read_lines(path);
  std::vector<std::string> lines;
  for (const std::string& line : old_lines) {
    const auto assignment = parse_assignment(line);
    if (!assignment.has_value()) {
      lines.push_back(line);
    } else if (assignment->first != name) {
      lines.push_back(line);
    }
  }
  if (lines.size() == old_lines.size()) {
    throw UserError("configuration key not found: " + name);
  }
  write_lines(path, lines);
}

bool name_matches(std::string_view name, std::string_view filter) {
  if (filter.empty()) return true;
  if (name == filter) return true;
  return name.starts_with(std::string(filter) + ".");
}

}  // namespace

std::optional<std::string> config_value(Repository& repo,
                                        std::string_view name) {
  std::map<std::string, std::string> values = kDefaultValues;
  const std::array<ConfigScope, 3> scopes{
      ConfigScope::user, ConfigScope::repository, ConfigScope::workspace};
  for (const ConfigScope scope : scopes) {
    for (const auto& entry : read_values(config_path(repo, scope))) {
      values[entry.first] = entry.second;
    }
  }
  for (const auto& entry : runtime_values(repo)) {
    values[entry.first] = entry.second;
  }
  const auto value = values.find(std::string(name));
  if (value == values.end()) return std::nullopt;
  return value->second;
}

void command_config(Repository& repo,
                    const ConfigCommand& options,
                    std::ostream& output) {
  const bool scope_required = (options.action == ConfigAction::edit) |
                              (options.action == ConfigAction::path) |
                              (options.action == ConfigAction::set) |
                              (options.action == ConfigAction::unset);
  const std::optional<ConfigScope> scope = selected_scope(options, scope_required);
  if (!options.template_value.empty()) {
    throw UserError("config templates are not supported yet");
  }
  const bool name_required = (options.action == ConfigAction::get) |
                             (options.action == ConfigAction::set) |
                             (options.action == ConfigAction::unset);
  if (name_required || !options.name.empty()) validate_name(options.name);

  if (options.action == ConfigAction::path) {
    output << config_path(repo, *scope).string() << '\n';
    return;
  }
  if (options.action == ConfigAction::edit) {
    const std::filesystem::path path = config_path(repo, *scope);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::app);
    edit_file_with_editor(path);
    return;
  }
  if (options.action == ConfigAction::set) {
    validate_value(options.value);
    set_value(config_path(repo, *scope), options.name, options.value);
    return;
  }
  if (options.action == ConfigAction::unset) {
    unset_value(config_path(repo, *scope), options.name);
    return;
  }
  if (options.action == ConfigAction::get) {
    const auto value = config_value(repo, options.name);
    if (!value.has_value()) {
      throw UserError("configuration key not found: " + options.name);
    }
    output << *value << '\n';
    return;
  }

  const std::array<ConfigScope, 3> scopes{
      ConfigScope::user, ConfigScope::repository, ConfigScope::workspace};
  std::map<std::string, std::string> merged;
  std::vector<std::pair<std::string, std::string>> layered;
  if (options.include_defaults) {
    merged = kDefaultValues;
    layered.insert(layered.end(), merged.begin(), merged.end());
  }
  for (const ConfigScope layer : scopes) {
    if (scope.has_value()) {
      if (*scope != layer) continue;
    }
    for (const auto& entry : read_values(config_path(repo, layer))) {
      merged[entry.first] = entry.second;
      layered.push_back(entry);
    }
  }
  if (!scope.has_value()) {
    for (const auto& entry : runtime_values(repo)) {
      merged[entry.first] = entry.second;
      layered.push_back(entry);
    }
  }
  const auto print = [&](const auto& entries) {
    for (const auto& [name, value] : entries) {
      if (name_matches(name, options.name)) {
        output << name << " = " << value << '\n';
      }
    }
  };
  if (options.include_overridden) {
    print(layered);
  } else {
    print(merged);
  }
}

}  // namespace gg::detail
