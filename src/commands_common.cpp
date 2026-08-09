// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <CLI/CLI.hpp>
#include <git2/sys/errors.h>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fnmatch.h>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <utility>

extern char** environ;

namespace gg::detail {
namespace {

enum GraphLink : std::uint16_t {
  graph_horizontal = 1 << 0,
  graph_vertical = 1 << 1,
  graph_left_fork = 1 << 2,
  graph_right_fork = 1 << 3,
  graph_left_merge = 1 << 4,
  graph_right_merge = 1 << 5,
  graph_child = 1 << 6,
};

bool has_link(std::uint16_t value, GraphLink link) {
  return (value & link) != 0;
}

std::string_view graph_link_glyph(std::uint16_t link, bool merge) {
  const bool fork = has_link(link, graph_left_fork) ||
                    has_link(link, graph_right_fork);
  const bool joins = has_link(link, graph_left_merge) ||
                     has_link(link, graph_right_merge);
  if (has_link(link, graph_horizontal)) {
    if (has_link(link, graph_child) || (fork && joins) ||
        (fork && has_link(link, graph_vertical) && !merge)) {
      return "┼─";
    }
    if (fork) return "┬─";
    if (joins) return "┴─";
    return "──";
  }
  if (has_link(link, graph_vertical) && !merge) {
    const bool left = has_link(link, graph_left_merge) ||
                      has_link(link, graph_left_fork);
    const bool right = has_link(link, graph_right_merge) ||
                       has_link(link, graph_right_fork);
    if (left && right) return "┼─";
    if (left) return "┤ ";
    if (right) return "├─";
    return "│ ";
  }
  if (has_link(link, graph_vertical) && !fork) {
    const bool left = has_link(link, graph_left_merge);
    const bool right = has_link(link, graph_right_merge);
    if (left && right) return "┼─";
    if (left) return "┤ ";
    if (right) return "├─";
    return "│ ";
  }
  if (has_link(link, graph_left_fork) &&
      (has_link(link, graph_left_merge) || has_link(link, graph_child))) {
    return "┤ ";
  }
  if (has_link(link, graph_right_fork) &&
      (has_link(link, graph_right_merge) || has_link(link, graph_child))) {
    return "├─";
  }
  if (has_link(link, graph_left_merge) &&
      has_link(link, graph_right_merge)) {
    return "┴─";
  }
  if (has_link(link, graph_left_fork) &&
      has_link(link, graph_right_fork)) {
    return "┬─";
  }
  if (has_link(link, graph_left_fork)) return "╮ ";
  if (has_link(link, graph_left_merge)) return "╯ ";
  if (has_link(link, graph_right_fork)) return "╭─";
  if (has_link(link, graph_right_merge)) return "╰─";
  return "  ";
}

void trim_graph_line(std::string& line) {
  while (!line.empty() && line.back() == ' ') {  // GG_COV_EXCL_BRANCH
    line.pop_back();
  }
}

struct StyleSpec {
  std::string_view ansi;
  std::string_view label;
};

constexpr std::array<StyleSpec, 23> kStyles{{
    {"\x1b[1;38;5;2m", "working_copy"},
    {"\x1b[38;5;5m", "change_id"},
    {"\x1b[1;38;5;5m", "change_id shortest prefix"},
    {"\x1b[38;5;8m", "change_id shortest rest"},
    {"\x1b[1;38;5;13m", "working_copy change_id"},
    {"\x1b[1;38;5;13m", "working_copy change_id shortest prefix"},
    {"\x1b[38;5;8m", "working_copy change_id shortest rest"},
    {"\x1b[38;5;4m", "commit_id"},
    {"\x1b[1;38;5;4m", "commit_id shortest prefix"},
    {"\x1b[38;5;8m", "commit_id shortest rest"},
    {"\x1b[1;38;5;12m", "working_copy commit_id"},
    {"\x1b[1;38;5;12m", "working_copy commit_id shortest prefix"},
    {"\x1b[38;5;8m", "working_copy commit_id shortest rest"},
    {"\x1b[38;5;5m", "bookmark"},
    {"\x1b[38;5;5m", "tag"},
    {"\x1b[38;5;4m", "operation_id"},
    {"\x1b[1;38;5;12m", "current_operation_id"},
    {"\x1b[38;5;6m", "timestamp"},
    {"\x1b[38;5;2m", "added"},
    {"\x1b[38;5;1m", "removed"},
    {"\x1b[38;5;3m", "modified"},
    {"\x1b[1m", "heading"},
    {"\x1b[38;5;6m", "hunk"},
}};

std::string_view trim_template(std::string_view value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos) return {};
  return value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}

std::vector<std::string_view> template_atoms(std::string_view expression) {
  std::vector<std::string_view> atoms;
  std::size_t begin = 0;
  char quote = '\0';
  bool escaped = false;
  for (std::size_t index = 0; index < expression.size(); ++index) {
    const char character = expression[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quote != '\0' && character == '\\') {
      escaped = true;
    } else if (character == '\'' || character == '"') {
      if (quote == character) {
        quote = '\0';
      } else if (quote == '\0') {
        quote = character;
      }
    } else if (quote == '\0' && character == '+' &&
               index + 1 < expression.size() && expression[index + 1] == '+') {
      atoms.push_back(trim_template(expression.substr(begin, index - begin)));
      begin = index + 2;
      ++index;
    }
  }
  if (quote != '\0') throw UserError("unterminated template string");
  atoms.push_back(trim_template(expression.substr(begin)));
  return atoms;
}

std::string template_literal(std::string_view atom) {
  if (atom.back() != atom.front()) {
    throw UserError("invalid template string literal");
  }
  std::string result;
  for (std::size_t index = 1; index + 1 < atom.size(); ++index) {
    if (atom[index] != '\\') {
      result += atom[index];
      continue;
    }
    ++index;
    switch (atom[index]) {
      case 'n':
        result += '\n';
        break;
      case 'r':
        result += '\r';
        break;
      case 't':
        result += '\t';
        break;
      case '\\':
        result += '\\';
        break;
      case '\'':
        result += '\'';
        break;
      case '"':
        result += '"';
        break;
      default:
        throw UserError("invalid template escape");
    }
  }
  return result;
}

std::string template_keyword(
    std::string_view atom,
    const std::map<std::string, std::string>& values) {
  std::string_view name = atom;
  enum class Method { none, shorten, first_line };
  Method method = Method::none;
  std::size_t short_length = 8;
  if (atom.ends_with(".first_line()")) {
    name = atom.substr(0, atom.size() - std::string_view(".first_line()").size());
    method = Method::first_line;
  } else if (const std::size_t short_method = atom.find(".short(");
             short_method != std::string_view::npos && atom.back() == ')') {
    name = atom.substr(0, short_method);
    const std::string_view argument = atom.substr(
        short_method + std::string_view(".short(").size(),
        atom.size() - short_method - std::string_view(".short(").size() - 1);
    if (!argument.empty()) {
      const auto parsed =
          std::from_chars(argument.begin(), argument.end(), short_length);
      if (parsed.ec != std::errc{} || parsed.ptr != argument.end()) {
        throw UserError("template short length must be an integer");
      }
    }
    method = Method::shorten;
  }
  if (name.starts_with("self.")) name.remove_prefix(5);
  const auto value = values.find(std::string(name));
  if (value == values.end()) {
    throw UserError("unknown template keyword: " + std::string(name));
  }
  if (method == Method::shorten) {
    return value->second.substr(0, short_length);
  }
  if (method == Method::first_line) return first_line(value->second.c_str());
  return value->second;
}

int color_mode_index() {
  static const int index = std::ios_base::xalloc();  // GG_COV_EXCL_BRANCH
  return index;
}

}  // namespace

std::string_view graph_link_glyph_for_test(std::uint16_t link, bool merge) {
  return graph_link_glyph(link, merge);
}

void GraphRenderer::add(std::ostream& output,
                        const git_oid& node,
                        std::span<const git_oid> parents,
                        std::string_view marker,
                        std::string_view content) {
  const auto same_oid = [](const std::optional<git_oid>& value,
                           const git_oid& expected) {
    return value.has_value() && *value == expected;
  };
  auto current = std::find_if(columns_.begin(), columns_.end(),
                              [&](const auto& value) {
                                return same_oid(value, node);
                              });
  if (current == columns_.end()) {
    current = std::find(columns_.begin(), columns_.end(), std::nullopt);
    if (current == columns_.end()) {
      columns_.emplace_back();
      current = std::prev(columns_.end());
    }
  }
  const std::size_t column = std::distance(columns_.begin(), current);
  columns_[column].reset();

  std::vector<bool> node_line(columns_.size());
  std::vector<std::uint16_t> link_line(columns_.size());
  for (std::size_t index = 0; index < columns_.size(); ++index) {
    node_line[index] = columns_[index].has_value();
    if (columns_[index].has_value()) link_line[index] = graph_vertical;
  }
  std::map<std::size_t, git_oid> parent_columns;
  for (const git_oid& parent : parents) {
    auto assigned = std::find_if(columns_.begin(), columns_.end(),
                                 [&](const auto& value) {
                                   return same_oid(value, parent);
                                 });
    if (assigned == columns_.end()) {
      if (!columns_[column].has_value()) {
        assigned = columns_.begin() + column;
      } else {
        assigned = std::find(columns_.begin(), columns_.end(), std::nullopt);
      }
      if (assigned == columns_.end()) {
        columns_.emplace_back();
        node_line.push_back(false);
        link_line.push_back(0);
        assigned = std::prev(columns_.end());
      }
      *assigned = parent;
    }
    parent_columns.emplace(std::distance(columns_.begin(), assigned), parent);
  }

  bool needs_link_line = false;
  if (parents.size() == 1 && !parent_columns.empty()) {  // GG_COV_EXCL_BRANCH
    const std::size_t parent_column = parent_columns.begin()->first;
    if (parent_column > column) {
      std::swap(columns_[column], columns_[parent_column]);
      const git_oid parent = parent_columns.begin()->second;
      parent_columns.clear();
      parent_columns.emplace(column, parent);
      link_line[column] |= graph_right_fork;
      for (std::size_t index = column + 1; index < parent_column; ++index) {
        link_line[index] |= graph_horizontal;
      }
      link_line[parent_column] = graph_left_merge;
      needs_link_line = true;
    }
  }

  if (!parent_columns.empty()) {
    const std::size_t first = std::min(column, parent_columns.begin()->first);
    const std::size_t last =
        std::max(column, parent_columns.rbegin()->first);
    for (std::size_t index = first + 1; index < last; ++index) {
      if (index != column) link_line[index] |= graph_horizontal;
      needs_link_line |= index != column;
    }
    if (last > column) {
      link_line[column] |= graph_right_merge;
      needs_link_line = true;
    }
    if (first < column) {
      link_line[column] |= graph_left_merge;
      needs_link_line = true;
    }
    for (const auto& [index, parent] : parent_columns) {
      (void)parent;
      if (index < column) {
        link_line[index] |= graph_right_fork;
      } else if (index == column) {
        link_line[index] |= graph_child | graph_vertical;
      } else {
        link_line[index] |= graph_left_fork;
      }
    }
  }

  std::vector<std::string> message_lines;
  std::istringstream messages{std::string(content)};
  std::string message;
  while (std::getline(messages, message)) message_lines.push_back(message);
  std::size_t message_index = 0;
  std::string rendered;
  for (std::size_t index = 0; index < node_line.size(); ++index) {
    if (index == column) {
      rendered += marker;
      rendered += ' ';
    } else {
      rendered += node_line[index] ? "│ " : "  ";
    }
  }
  if (message_index < message_lines.size()) {
    rendered += ' ';
    rendered += message_lines[message_index++];
  }
  trim_graph_line(rendered);
  output << rendered << '\n';

  if (needs_link_line) {
    rendered.clear();
    for (const std::uint16_t link : link_line) {
      rendered += graph_link_glyph(link, parents.size() > 1);
    }
    if (message_index < message_lines.size()) {
      rendered += ' ';
      rendered += message_lines[message_index++];
    }
    trim_graph_line(rendered);
    output << rendered << '\n';
  }

  while (message_index < message_lines.size()) {
    rendered.clear();
    for (const auto& value : columns_) {
      rendered += value.has_value() ? "│ " : "  ";
    }
    rendered += ' ';
    rendered += message_lines[message_index++];
    trim_graph_line(rendered);
    output << rendered << '\n';
  }
  while (!columns_.empty() && !columns_.back().has_value()) {
    columns_.pop_back();
  }
}

std::string render_template(
    std::string_view expression,
    const std::map<std::string, std::string>& values) {
  std::string result;
  for (const std::string_view atom : template_atoms(expression)) {
    if (atom.empty()) throw UserError("template expression is empty");
    if (atom.front() == '\'' || atom.front() == '"') {
      result += template_literal(atom);
    } else {
      result += template_keyword(atom, values);
    }
  }
  return result;
}

std::map<std::string, std::string> revision_template_values(
    Repository& repo,
    const git_oid& oid) {
  CommitPtr commit = repo.commit(oid);
  const git_signature* author = git_commit_author(commit.get());
  const git_signature* committer = git_commit_committer(commit.get());
  const char* raw_description = git_commit_message(commit.get());
  const std::string description =
      raw_description == nullptr ? "" : raw_description;  // GG_COV_EXCL_BRANCH
  const std::optional<std::string> change_id = repo.change_id(oid);
  std::ostringstream bookmarks;
  for (const std::string& bookmark : repo.bookmarks(oid)) {
    if (bookmarks.tellp() != 0) bookmarks << ' ';
    bookmarks << bookmark;
  }
  const auto workspace = repo.workspace();
  return {{"commit_id", oid_string(oid)},
          {"change_id", change_id.value_or("")},
          {"description", description},
          {"subject", first_line(description.c_str())},
          {"author.name", author->name},
          {"author.email", author->email},
          {"committer.name", committer->name},
          {"committer.email", committer->email},
          {"bookmarks", bookmarks.str()},
          {"conflict", repo.commit_has_conflicts(oid) ? "true" : "false"},  // GG_COV_EXCL_BRANCH
          {"working_copy",
           workspace.has_value() && *workspace == oid ? "true" : "false"}};
}

void set_output_color_mode(std::ostream& output, OutputColorMode mode) {
  output.iword(color_mode_index()) = static_cast<long>(mode);
}

OutputColorMode output_color_mode(std::ostream& output) {
  return static_cast<OutputColorMode>(output.iword(color_mode_index()));
}

std::string styled(std::ostream& output,
                   std::string_view value,
                   OutputStyle style) {
  const auto mode = output_color_mode(output);
  if (mode == OutputColorMode::plain) return std::string(value);
  const StyleSpec& spec = kStyles[static_cast<std::size_t>(style)];
  std::string result(spec.ansi);
  if (mode == OutputColorMode::debug) {
    result += "<<";
    result += spec.label;
    result += "::";
  }
  result += value;
  if (mode == OutputColorMode::debug) result += ">>";
  result += "\x1b[0m";
  return result;
}

std::string styled_short_id(std::ostream& output,
                            const ShortId& id,
                            OutputStyle prefix_style,
                            OutputStyle rest_style) {
  return styled(output, id.value.substr(0, id.prefix_length), prefix_style) +
         styled(output, id.value.substr(id.prefix_length), rest_style);
}

std::string styled_short_change_id(Repository& repo,
                                   std::ostream& output,
                                   std::string_view id,
                                   bool working) {
  return styled_short_id(
      output, repo.short_change_id_parts(id),
      working ? OutputStyle::working_change_id_prefix
              : OutputStyle::change_id_prefix,
      working ? OutputStyle::working_change_id_rest
              : OutputStyle::change_id_rest);
}

std::string styled_short_commit_id(Repository& repo,
                                   std::ostream& output,
                                   const git_oid& oid,
                                   bool working) {
  return styled_short_id(
      output, repo.short_commit_id(oid),
      working ? OutputStyle::working_commit_id_prefix
              : OutputStyle::commit_id_prefix,
      working ? OutputStyle::working_commit_id_rest
              : OutputStyle::commit_id_rest);
}

bool string_pattern_matches(std::string_view pattern,
                            std::string_view value,
                            std::string_view default_kind) {
  const std::size_t separator = pattern.find(':');
  const std::string_view kind = separator == std::string_view::npos
                                    ? default_kind
                                    : pattern.substr(0, separator);
  const std::string_view body = separator == std::string_view::npos
                                    ? pattern
                                    : pattern.substr(separator + 1);
  if (kind == "exact") return value == body;
  if (kind == "substring") return value.find(body) != std::string_view::npos;
  if (kind == "glob") {
    return fnmatch(std::string(body).c_str(), std::string(value).c_str(), 0) ==
           0;
  }
  if (kind == "regex") {
    try {
      return std::regex_search(value.begin(), value.end(),
                               std::regex(std::string(body)));
    } catch (const std::regex_error&) {  // GG_COV_EXCL_BRANCH
      throw UserError("invalid regex string pattern: " + std::string(pattern));
    }
  }
  throw UserError("invalid string pattern kind: " + std::string(kind));
}

bool any_string_pattern_matches(const std::vector<std::string>& patterns,
                                std::string_view value,
                                std::string_view default_kind) {
  return std::ranges::any_of(patterns, [&](const std::string& pattern) {
    return string_pattern_matches(pattern, value, default_kind);
  });
}

std::vector<git_oid> commit_parents(Repository& repo,
                                    const std::vector<std::string>& revisions) {
  return resolve_revision_arguments(repo, revisions);
}

std::vector<git_oid> resolve_revision_arguments(
    Repository& repo, const std::vector<std::string>& revisions) {
  std::vector<git_oid> result;
  std::set<git_oid, OidLess> seen;
  for (const std::string& revision : revisions) {
    for (const git_oid& oid : repo.resolve_set(revision)) {
      if (seen.insert(oid).second) result.push_back(oid);
    }
  }
  return result;
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
  updates[repo.workspace_ref_name()] = workspace;
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
  std::vector<std::string> argument_storage =
      CLI::detail::split_up(raw_editor);
  CLI::detail::remove_quotes(argument_storage);
  if (argument_storage.empty()) {
    throw UserError("VISUAL or EDITOR must name an editor executable");
  }
  argument_storage.push_back(path.string());
  std::vector<char*> arguments;
  arguments.reserve(argument_storage.size() + 1);
  for (std::string& argument : argument_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  pid_t process = 0;
  const int spawned = posix_spawnp(&process, argument_storage.front().c_str(),
                                   nullptr, nullptr, arguments.data(), environ);
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
  if (status != 0) throw UserError("garbage collection failed");  // GG_COV_EXCL_BRANCH
  output << "Garbage collection completed.\n";
}

void command_util_snapshot(Repository& repo, std::ostream& output) {
  output << (repo.sync_workspace() ? "Created working-copy snapshot.\n"
                                   : "Nothing changed.\n");
}

namespace {

std::filesystem::path hooks_directory(Repository& repo) {
  git_config* raw_config = nullptr;
  check(git_repository_config(&raw_config, repo.raw()), "read Git config");
  GitPtr<git_config, git_config_free> config(raw_config);
  git_buf configured = GIT_BUF_INIT;
  const int result = git_config_get_path(&configured, config.get(),
                                         "core.hookspath");
  if (result == GIT_ENOTFOUND) {
    git_error_clear();
    return std::filesystem::path(git_repository_commondir(repo.raw())) /
           "hooks";
  }
  check(result, "read core.hooksPath");
  std::filesystem::path path(configured.ptr);
  git_buf_dispose(&configured);
  if (path.empty()) throw UserError("core.hooksPath must not be empty");  // GG_COV_EXCL_BRANCH
  if (path.is_relative()) {  // GG_COV_EXCL_BRANCH
    path = std::filesystem::path(git_repository_workdir(repo.raw())) / path;
  }
  return path.lexically_normal();
}

bool path_exists(const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  if (error) throw UserError("cannot inspect Git hook: " + error.message());  // GG_COV_EXCL_BRANCH
  return status.type() != std::filesystem::file_type::not_found;
}

constexpr std::string_view kManagedPrePush = "# gg managed pre-push hook v1";

bool managed_hook(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string first;
  std::string second;
  return std::getline(input, first) && std::getline(input, second) &&  // GG_COV_EXCL_BRANCH
         second == kManagedPrePush;
}

std::string pre_push_hook() {
  return R"HOOK(#!/bin/sh
# gg managed pre-push hook v1
set -eu
hook_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
input=$(mktemp "${TMPDIR:-/tmp}/gg-pre-push.XXXXXX")
trap 'rm -f "$input"' EXIT HUP INT TERM
cat >"$input"
if [ -x "$hook_dir/pre-push.gg-user" ]; then
  "$hook_dir/pre-push.gg-user" "$@" <"$input"
fi
root=$(git rev-parse --show-toplevel)
gg_executable=${GG_EXECUTABLE:-gg}
"$gg_executable" --ignore-working-copy -R "$root" util check-push-conflicts <"$input"
)HOOK";
}

}  // namespace

void command_util_install_git_hooks(Repository& repo, std::ostream& output) {
  const std::filesystem::path directory = hooks_directory(repo);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) throw UserError("cannot create Git hooks directory: " + error.message());  // GG_COV_EXCL_BRANCH
  const std::filesystem::path hook = directory / "pre-push";
  const std::filesystem::path backup = directory / "pre-push.gg-user";
  if (path_exists(hook) && managed_hook(hook)) {  // GG_COV_EXCL_BRANCH
    std::filesystem::permissions(
        hook, std::filesystem::perms::owner_exec |
                  std::filesystem::perms::group_exec |
                  std::filesystem::perms::others_exec,
        std::filesystem::perm_options::add, error);
    if (error) throw UserError("cannot make Git hook executable: " + error.message());  // GG_COV_EXCL_BRANCH
    output << "Git pre-push hook is already installed.\n";
    return;
  }
  if (path_exists(hook) && path_exists(backup)) {  // GG_COV_EXCL_BRANCH
    throw UserError("cannot preserve pre-push hook: " + backup.string() +
                    " already exists");
  }
  std::string temporary_template =
      (directory / "pre-push.gg-new-XXXXXX").string();
  const int descriptor = mkstemp(temporary_template.data());
  if (descriptor < 0) throw UserError("cannot create Git pre-push hook");  // GG_COV_EXCL_BRANCH
  close(descriptor);
  const std::filesystem::path temporary(temporary_template);
  {
    std::ofstream file(temporary, std::ios::trunc);
    if (!file || !(file << pre_push_hook())) { file.close(); std::filesystem::remove(temporary); throw UserError("cannot write Git pre-push hook"); }  // GG_COV_EXCL_BRANCH
  }
  std::filesystem::permissions(
      temporary, std::filesystem::perms::owner_exec |
                     std::filesystem::perms::group_exec |
                     std::filesystem::perms::others_exec,
      std::filesystem::perm_options::add, error);
  if (error) { std::filesystem::remove(temporary); throw UserError("cannot make Git hook executable: " + error.message()); }  // GG_COV_EXCL_BRANCH
  const bool preserve = path_exists(hook);
  if (preserve) {  // GG_COV_EXCL_BRANCH
    std::filesystem::rename(hook, backup, error);
    if (error) { std::filesystem::remove(temporary); throw UserError("cannot preserve pre-push hook: " + error.message()); }  // GG_COV_EXCL_BRANCH
  }
  std::filesystem::rename(temporary, hook, error);
  if (error) { if (preserve) { std::error_code ignored; std::filesystem::rename(backup, hook, ignored); } std::filesystem::remove(temporary); throw UserError("cannot install Git pre-push hook: " + error.message()); }  // GG_COV_EXCL_BRANCH
  output << "Installed Git pre-push hook at " << hook.string() << ".\n";
}

void command_util_check_push_conflicts(Repository& repo, std::istream& input) {
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty()) continue;  // GG_COV_EXCL_BRANCH
    std::istringstream fields(line);
    std::string local_ref;
    std::string local_oid;
    std::string remote_ref;
    std::string remote_oid;
    std::string extra;
    if (!(fields >> local_ref >> local_oid >> remote_ref >> remote_oid) ||  // GG_COV_EXCL_BRANCH
        fields >> extra) {  // GG_COV_EXCL_BRANCH
      throw UserError("invalid Git pre-push input");
    }
    git_oid oid{};
    check(git_oid_fromstr(&oid, local_oid.c_str()), "parse pushed object");
    if (git_oid_is_zero(&oid)) continue;
    git_object* raw_object = nullptr;
    check(git_object_lookup(&raw_object, repo.raw(), &oid, GIT_OBJECT_ANY),
          "read pushed object");
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
          "resolve pushed commit");
    ObjectPtr commit(raw_commit);
    if (repo.history_has_conflicts(*git_object_id(commit.get()))) {
      throw UserError("refusing to push conflicted history: " + local_ref +
                      " -> " + remote_ref);
    }
  }
}

void command_workspace(Repository& repo,
                       const WorkspaceCommand& options,
                       std::ostream& output) {
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(git_repository_workdir(repo.raw()));
  const auto workspace_reference = repo.workspace_ref();
  const auto workspace = repo.workspace();
  const std::string workspace_name = repo.workspace_name();
  const auto roots = repo.workspace_roots();
  if (options.action == WorkspaceAction::root) {
    if (options.name.empty()) {
      output << root.string() << '\n';
      return;
    }
    const std::string reference =
        std::string(kWorkspacePrefix) + options.name;
    const auto found = roots.find(options.name);
    if (!repo.ref_target(reference).has_value() || found == roots.end()) {
      throw UserError("workspace not found: " + options.name);
    }
    output << std::filesystem::weakly_canonical(found->second).string() << '\n';
    return;
  }
  if (options.action == WorkspaceAction::add) {
    repo.sync_workspace();
    if (!options.name.empty()) {
      int valid = 0;
      const std::string reference =
          std::string(kWorkspacePrefix) + options.name;
      check(git_reference_name_is_valid(&valid, reference.c_str()),
            "validate workspace name");
      if (valid == 0) {
        throw UserError("invalid workspace name: " + options.name);
      }
    }
    if (!options.name.empty() && roots.contains(options.name)) {
      throw UserError("workspace already exists: " + options.name);
    }
    if (!options.name.empty() &&
        repo.ref_target(std::string(kWorkspacePrefix) + options.name)
            .has_value()) {
      throw UserError("workspace already exists: " + options.name);
    }
    const std::filesystem::path destination =
        std::filesystem::absolute(options.destination).lexically_normal();
    const std::string revision =
        options.revision.empty() ? (workspace.has_value() ? "@" : "HEAD")
                                 : options.revision;
    const git_oid parent = repo.resolve(revision);
    const CommitPtr parent_commit = repo.commit(parent);
    const git_oid working = repo.create_commit(
        *git_commit_tree_id(parent_commit.get()), {parent}, options.message);
    UtilExecCommand command{
        "git",
        {"--git-dir=" + std::string(git_repository_commondir(repo.raw())),
         "worktree", "add", "--quiet", "--detach", destination.string(),
         oid_string(parent)}};
    if (command_util_exec(command, git_repository_path(repo.raw())) != 0) {
      throw UserError("cannot create linked workspace");
    }
    Repository linked(destination);
    if (!options.name.empty()) linked.set_workspace_name(options.name);
    const std::string name = linked.workspace_name();
    const std::string id = repo.new_change_id();
    repo.record({{std::string(kWorkspacePrefix) + name, working},
                 {std::string(kChangePrefix) + id, working}},
                {}, repo.head_state(), "gg workspace add " + name, true);
    output << "Created workspace " << name << " at " << destination.string()
           << ".\n";
    return;
  }
  if (options.action == WorkspaceAction::rename) {
    if (!workspace_reference.has_value()) {
      throw UserError("this command requires a working-copy change");
    }
    if (options.name == workspace_name) {
      output << "Nothing changed.\n";
      return;
    }
    const std::string renamed = std::string(kWorkspacePrefix) + options.name;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, renamed.c_str()),
          "validate workspace name");
    if (valid == 0) throw UserError("invalid workspace name: " + options.name);
    if (roots.contains(options.name) || repo.ref_target(renamed).has_value()) {
      throw UserError("workspace already exists: " + options.name);
    }
    (void)repo.ensure_operation();
    repo.set_workspace_name(options.name);
    try {
      repo.record({{renamed, *workspace}}, {*workspace_reference},
                  repo.head_state(),
                  "gg workspace rename " + workspace_name + " " +
                      options.name,
                  true);
    } catch (...) {
      repo.set_workspace_name(workspace_name);
      throw;
    }
    return;
  }
  if (options.action == WorkspaceAction::forget) {
    const std::vector<std::string> names =
        options.names.empty()
            ? std::vector<std::string>{workspace_reference.has_value()
                                           ? workspace_name
                                           : "default"}
            : options.names;
    std::set<std::string> deletes;
    for (const std::string& name : names) {
      const std::string reference = std::string(kWorkspacePrefix) + name;
      if (repo.ref_target(reference).has_value()) {
        deletes.insert(reference);
      } else {
        output << "No such workspace: " << name << '\n';
      }
    }
    if (deletes.empty()) {
      output << "Nothing changed.\n";
      return;
    }
    repo.record({}, std::move(deletes), repo.head_state(),
                "gg workspace forget", true);
    return;
  }
  std::vector<std::pair<std::string, git_oid>> workspaces;
  for (const auto& [name, oid] : repo.data_refs()) {
    if (starts_with(name, kWorkspacePrefix)) {
      workspaces.emplace_back(name.substr(kWorkspacePrefix.size()), oid);
    }
  }
  if (workspaces.empty()) {
    if (!options.template_value.empty()) {
      static const std::map<std::string, std::string> values{
          {"name", ""},      {"target", ""},
          {"root", ""},      {"commit_id", ""},
          {"change_id", ""}, {"description", ""},
          {"subject", ""},   {"author.name", ""},
          {"author.email", ""},
          {"committer.name", ""},
          {"committer.email", ""},
          {"bookmarks", ""},
          {"working_copy", ""}};  // GG_COV_EXCL_BRANCH
      (void)render_template(options.template_value, values);
    }
    output << "No workspaces.\n";
    return;
  }
  for (const auto& [name, oid] : workspaces) {
    const auto found = roots.find(name);
    const std::string workspace_root =
        found == roots.end()
            ? "(stale)"
            : std::filesystem::weakly_canonical(found->second).string();
    if (!options.template_value.empty()) {
      auto values = revision_template_values(repo, oid);
      values["name"] = name;
      values["target"] = oid_string(oid);
      values["root"] = workspace_root;
      values["working_copy"] = name == workspace_name ? "true" : "false";
      output << render_template(options.template_value, values);
    } else {
      const auto id = repo.change_id(oid);
      output << name << ": "
             << (id.has_value() ? repo.short_change_id(*id) : "--------")
             << ' ' << oid_string(oid, 8) << ' ' << workspace_root << '\n';
    }
  }
}

void command_sparse(Repository& repo,
                    const SparseCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  if (!repo.workspace().has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  if (options.action == SparseAction::list) output << ".\n";
}

}  // namespace gg::detail
