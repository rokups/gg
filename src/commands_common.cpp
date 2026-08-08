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
  updates[repo.workspace_ref().value_or(std::string(kWorkspaceRef))] = workspace;
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

void command_workspace(Repository& repo,
                       const WorkspaceCommand& options,
                       std::ostream& output) {
  const std::filesystem::path root =
      std::filesystem::weakly_canonical(git_repository_workdir(repo.raw()));
  const auto workspace_reference = repo.workspace_ref();
  const auto workspace = repo.workspace();
  const std::string workspace_name =
      workspace_reference.has_value()
          ? workspace_reference->substr(kWorkspacePrefix.size())
          : "";
  if (options.action == WorkspaceAction::root) {
    if (!options.name.empty() && options.name != workspace_name) {
      throw UserError("workspace not found: " + options.name);
    }
    output << root.string() << '\n';
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
    repo.record({{renamed, *workspace}}, {*workspace_reference},
                repo.head_state(),
                "gg workspace rename " + workspace_name + " " + options.name);
    return;
  }
  if (options.action == WorkspaceAction::forget) {
    const std::vector<std::string> names =
        options.names.empty()
            ? std::vector<std::string>{workspace_reference.has_value()
                                           ? workspace_name
                                           : "default"}
            : options.names;
    bool found = false;
    for (const std::string& name : names) {
      if (workspace_reference.has_value() && name == workspace_name) {
        found = true;
      } else {
        output << "No such workspace: " << name << '\n';
      }
    }
    if (!found) {
      output << "Nothing changed.\n";
      return;
    }
    repo.record({}, {*workspace_reference}, repo.head_state(),
                "gg workspace forget " + workspace_name);
    return;
  }
  if (!workspace.has_value()) {
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
  if (!options.template_value.empty()) {
    auto values = revision_template_values(repo, *workspace);
    values["name"] = workspace_name;
    values["target"] = oid_string(*workspace);
    values["root"] = root.string();
    output << render_template(options.template_value, values);
  } else {
    const auto id = repo.change_id(*workspace);
    output << workspace_name << ": "
           << (id.has_value() ? repo.short_change_id(*id) : "--------") << ' '
           << oid_string(*workspace, 8) << ' ' << root.string() << '\n';
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
