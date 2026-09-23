// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"
#include "process.hpp"

#include <git2/sys/errors.h>

#include <fcntl.h>
#ifndef _WIN32
#include <sys/file.h>
#endif
#ifdef _MSC_VER
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <exception>
#ifndef _WIN32
#include <fnmatch.h>
#endif
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

#ifdef _MSC_VER
int create_temporary_file(char* pattern) {
  if (_mktemp_s(pattern, std::char_traits<char>::length(pattern) + 1) != 0) {
    return -1;
  }
  return _open(pattern, _O_CREAT | _O_EXCL | _O_RDWR,
               _S_IREAD | _S_IWRITE);
}
#else
int create_temporary_file(char* pattern) { return mkstemp(pattern); }
#endif

std::vector<std::string> split_command(std::string_view command) {
  std::vector<std::string> result;
  std::string argument;
  char quote = '\0';
  bool escaped = false;
  for (const char value : command) {
    if (escaped) {
      argument.push_back(value);
      escaped = false;
    } else if (value == '\\' && quote != '\'') {
      escaped = true;
    } else if (quote != '\0') {
      if (value == quote) quote = '\0';
      else argument.push_back(value);
    } else if (value == '\'' || value == '"') {
      quote = value;
    } else if (std::isspace(static_cast<unsigned char>(value)) != 0) {
      if (!argument.empty()) {
        result.push_back(std::move(argument));
        argument.clear();
      }
    } else {
      argument.push_back(value);
    }
  }
  if (escaped || quote != '\0') {
    throw UserError("invalid editor command");
  }
  if (!argument.empty()) result.push_back(std::move(argument));
  return result;
}

class WorkspaceLock {
 public:
  explicit WorkspaceLock(git_repository* repo) {
    const std::filesystem::path directory =
        std::filesystem::path(git_repository_commondir(repo)) / "gg";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / "workspace.lock";
#ifdef _WIN32
    descriptor_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (descriptor_ == INVALID_HANDLE_VALUE) {
      throw UserError("cannot lock workspace state");
    }
#else
    descriptor_ = open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (descriptor_ < 0 || flock(descriptor_, LOCK_EX) != 0) { if (descriptor_ >= 0) close(descriptor_); throw UserError("cannot lock workspace state"); }  // GG_COV_EXCL_BRANCH
#endif
  }

  ~WorkspaceLock() {
#ifdef _WIN32
    CloseHandle(descriptor_);
#else
    (void)flock(descriptor_, LOCK_UN);
    close(descriptor_);
#endif
  }

  WorkspaceLock(const WorkspaceLock&) = delete;
  WorkspaceLock& operator=(const WorkspaceLock&) = delete;

 private:
#ifdef _WIN32
  HANDLE descriptor_{INVALID_HANDLE_VALUE};
#else
  int descriptor_{-1};
#endif
};

void prevalidate_workspace_deletions(Repository& repo,
                                    const std::set<std::string>& references) {
  // Catch existing ref locks and unwritable ref paths before Git removes any
  // files or administration. Native Git clients do not take gg's lifecycle
  // lock, so the final reference transaction still performs its own checks.
  git_transaction* raw_transaction = nullptr;
  check(git_transaction_new(&raw_transaction, repo.raw()),
        "prepare workspace reference cleanup");
  TransactionPtr transaction(raw_transaction);
  std::set<std::string> names = references;
  names.insert(repo.operation_ref_name());
  for (const auto& name : names) {
    check(git_transaction_lock_ref(transaction.get(), name.c_str()),
          "prepare workspace reference cleanup");
  }
}

bool wildcard_matches(std::string_view pattern, std::string_view value) {
#ifdef _WIN32
  const std::string owned_pattern(pattern);
  const std::string owned_value(value);
  char* pattern_pointer = const_cast<char*>(owned_pattern.c_str());
  git_strarray patterns{&pattern_pointer, 1};
  git_pathspec* raw_pathspec = nullptr;
  if (git_pathspec_new(&raw_pathspec, &patterns) != 0) {
    git_error_clear();
    return false;
  }
  GitPtr<git_pathspec, git_pathspec_free> pathspec(raw_pathspec);
  return git_pathspec_matches_path(pathspec.get(), GIT_PATHSPEC_USE_CASE,
                                   owned_value.c_str()) != 0;
#else
  return fnmatch(std::string(pattern).c_str(), std::string(value).c_str(), 0) ==
         0;
#endif
}

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

constexpr std::array<StyleSpec, 17> kStyles{{
    {"\x1b[1;38;5;2m", "working_copy"},
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

void set_output_color_mode(std::ostream& output, OutputColorMode mode) {
  output.iword(color_mode_index()) = static_cast<long>(mode);
}

OutputColorMode output_color_mode(std::ostream& output) {
  return static_cast<OutputColorMode>(output.iword(color_mode_index()));
}

bool fileset_matches(std::string_view expression, std::string_view path) {
  const auto trim = [](std::string_view value) {
    const std::size_t begin = value.find_first_not_of(" \t\n\r");
    if (begin == std::string_view::npos) return std::string_view{};
    return value.substr(begin,
                        value.find_last_not_of(" \t\n\r") - begin + 1);
  };
  const auto top_level = [](std::string_view value, char operation) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t index = value.size(); index-- > 0;) {
      const char character = value[index];
      if (quote != '\0') {
        if (character == quote) quote = '\0';
      } else if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == ')') {
        ++depth;
      } else if (character == '(') {
        --depth;
      } else if (depth == 0 && character == operation) {
        return index;
      }
    }
    return std::string_view::npos;
  };
  const auto wrapped = [](std::string_view value) {
    if (value.size() < 2 || value.front() != '(' || value.back() != ')') {  // GG_COV_EXCL_BRANCH
      return false;
    }
    int depth = 0;
    for (std::size_t index = 0; index + 1 < value.size(); ++index) {
      if (value[index] == '(') ++depth;
      if (value[index] == ')' && --depth == 0) return false;
    }
    return true;
  };

  std::function<bool(std::string_view)> evaluate;
  evaluate = [&](std::string_view value) {
    value = trim(value);
    if (value.empty()) throw UserError("fileset must not be empty");
    int depth = 0;
    char quote = '\0';
    for (const char character : value) {
      if (quote != '\0') {
        if (character == quote) quote = '\0';
      } else if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == '(') {
        ++depth;
      } else if (character == ')' && --depth < 0) {
        throw UserError("unbalanced fileset expression");
      }
    }
    if (quote != '\0' || depth != 0) {  // GG_COV_EXCL_BRANCH
      throw UserError("unbalanced fileset expression");
    }
    while (wrapped(value)) value = trim(value.substr(1, value.size() - 2));
    if (const std::size_t separator = top_level(value, '|');
        separator != std::string_view::npos) {
      return evaluate(value.substr(0, separator)) ||
             evaluate(value.substr(separator + 1));
    }
    if (const std::size_t separator = top_level(value, '&');
        separator != std::string_view::npos) {
      return evaluate(value.substr(0, separator)) &&  // GG_COV_EXCL_BRANCH
             evaluate(value.substr(separator + 1));  // GG_COV_EXCL_BRANCH
    }
    if (const std::size_t separator = top_level(value, '~');
        separator != std::string_view::npos) {
      if (separator == 0) return !evaluate(value.substr(1));
      return evaluate(value.substr(0, separator)) &&
             !evaluate(value.substr(separator + 1));
    }
    if (value == "all()" || value == ".") return true;
    if (value == "none()") return false;
    const auto function_argument = [&](std::string_view name)
        -> std::optional<std::string_view> {
      if (!value.starts_with(name) || value.size() <= name.size() + 1 ||  // GG_COV_EXCL_BRANCH
          value[name.size()] != '(' || value.back() != ')') {  // GG_COV_EXCL_BRANCH
        return std::nullopt;
      }
      std::string_view argument =
          trim(value.substr(name.size() + 1, value.size() - name.size() - 2));
      if (argument.size() >= 2 &&  // GG_COV_EXCL_BRANCH
          ((argument.front() == '\'' && argument.back() == '\'') ||  // GG_COV_EXCL_BRANCH
           (argument.front() == '"' && argument.back() == '"'))) {  // GG_COV_EXCL_BRANCH
        argument = argument.substr(1, argument.size() - 2);
      }
      if (argument.empty()) throw UserError("fileset path must not be empty");
      return argument;
    };
    if (const auto argument = function_argument("glob"); argument.has_value()) {
      value = *argument;
      if (value.front() == '/' || value.find("../") != std::string_view::npos) {  // GG_COV_EXCL_BRANCH
        throw UserError("filesets must be repository-relative");
      }
      return wildcard_matches(value, path);
    }
    for (const std::string_view name : {"file", "root", "cwd", "exact"}) {
      if (const auto argument = function_argument(name); argument.has_value()) {
        value = *argument;
        break;
      }
    }
    if (value.starts_with("glob:")) {
      if (value.size() == 5 || value[5] == '/' ||  // GG_COV_EXCL_BRANCH
          value.substr(5).find("../") != std::string_view::npos) {
        throw UserError("filesets must be repository-relative");
      }
      return wildcard_matches(value.substr(5), path);
    }
    for (const std::string_view prefix : {"file:", "root:", "cwd:"}) {
      if (value.starts_with(prefix)) value.remove_prefix(prefix.size());
    }
    if (value.size() >= 2 &&  // GG_COV_EXCL_BRANCH
        ((value.front() == '\'' && value.back() == '\'') ||  // GG_COV_EXCL_BRANCH
         (value.front() == '"' && value.back() == '"'))) {  // GG_COV_EXCL_BRANCH
      value = value.substr(1, value.size() - 2);
    }
    if (value.empty() || value.front() == '/') {  // GG_COV_EXCL_BRANCH
      throw UserError("filesets must be repository-relative");
    }
    const std::filesystem::path parsed(value);
    for (const auto& component : parsed) {
      if (component == "..") {
        throw UserError("filesets must not contain '..'");
      }
    }
    const std::string normalized = parsed.lexically_normal().generic_string();
    return path == normalized ||
           (path.size() > normalized.size() && starts_with(path, normalized) &&
            path[normalized.size()] == '/');
  };
  return evaluate(expression);
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
    return wildcard_matches(body, value);
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
    result = repo.merge_trees(ancestor_tree, result, their_tree,
                              /*preserve_ours_conflicts=*/true,
                              /*preserve_theirs_conflicts=*/true);
  }
  return result;
}

void finish_workspace(Repository& repo,
                      const git_oid& workspace,
                      std::map<std::string, git_oid> updates,
                      std::set<std::string> deletes,
                      std::string_view operation,
                      bool captured_worktree) {
  bool tree_unchanged = false;
  if (const auto current = repo.workspace(); current.has_value()) {
    tree_unchanged = git_oid_equal(
        git_commit_tree_id(repo.commit(*current).get()),
        git_commit_tree_id(repo.commit(workspace).get())) != 0;
  }
  if (!tree_unchanged && !captured_worktree &&
      !repo.synchronizes_commands()) {
    const auto baseline = repo.workspace().has_value()
                              ? repo.workspace()
                              : repo.head_oid();
    const git_oid baseline_tree = baseline.has_value()
                                      ? *git_commit_tree_id(repo.commit(*baseline).get())
                                      : repo.empty_tree();
    if (repo.worktree_tracked_dirty(baseline_tree)) {
      throw UserError("working tree has uncommitted changes; commit or amend before switching changes");
    }
  }
  updates[repo.workspace_ref_name()] = workspace;
  const HeadState head = captured_worktree
                             ? HeadState{false, oid_string(workspace)}
                             : repo.head_for_workspace(workspace);
  const git_oid previous_operation = repo.ensure_operation();
  const auto previous_aliases = repo.data_refs();
  repo.record(std::move(updates), std::move(deletes), head, operation);
  try {
    repo.set_head(head);
    if (!tree_unchanged) repo.checkout(workspace);
  } catch (const std::exception& error) {
    const std::string original = error.what();
    try {
      repo.restore_operation(previous_operation, "", true, true, false,
                             &previous_aliases);
    } catch (const std::exception& recovery) {
      repo.clear_checkout_recovery();
      throw UserError(original + "; could not restore the previous operation: " +
                      recovery.what());
    }
    throw;
  }
}

void finish_workspace_preserving_worktree(
    Repository& repo, const git_oid& workspace,
    std::map<std::string, git_oid> updates,
    std::string_view operation) {
  const HeadState previous_head = repo.head_state();
  const git_oid previous_operation = repo.ensure_operation();
  const auto previous_refs = repo.data_refs();
  updates[repo.workspace_ref_name()] = workspace;
  const HeadState head{false, oid_string(workspace)};
  repo.record(std::move(updates), {}, head, operation);
  try {
    repo.set_head(head);
  } catch (const std::exception& error) {
    const std::string original = error.what();
    try {
      auto restore = previous_refs;
      restore[repo.operation_ref_name()] = previous_operation;
      std::set<std::string> deletes;
      for (const auto& [name, target] : repo.data_refs()) {
        (void)target;
        if (!previous_refs.contains(name)) deletes.insert(name);
      }
      repo.apply_refs(restore, deletes, "gg restore failed tree amend");
      repo.set_head(previous_head);
    } catch (const std::exception& recovery) {
      throw UserError(original + "; could not restore the previous operation: " +
                      recovery.what());
    }
    throw;
  }
}

void finish_without_workspace(Repository& repo, RewritePlan plan,
                              std::set<std::string> deletes,
                              std::string_view operation,
                              std::optional<git_oid> head_override) {
  const auto previous_head = repo.head_oid();
  HeadState head = repo.head_state();
  auto checkout = head_override.has_value() ? head_override : previous_head;
  if (!head_override.has_value() && previous_head.has_value()) {
    if (const auto found = plan.commits.find(*previous_head); found != plan.commits.end()) {
      checkout = found->second;
    }
  }
  if (head.symbolic && !deletes.contains(head.value)) {
    if (const auto found = plan.updates.find(head.value); found != plan.updates.end()) {
      checkout = found->second;
    }
  } else if (checkout.has_value()) {
    head = {false, oid_string(*checkout)};
  }
  if (!repo.synchronizes_commands() &&
      (checkout.has_value() != previous_head.has_value() ||
       (checkout.has_value() && !(*checkout == *previous_head)))) {
    const git_oid baseline_tree = previous_head.has_value()
                                      ? *git_commit_tree_id(repo.commit(*previous_head).get())
                                      : repo.empty_tree();
    if (repo.worktree_tracked_dirty(baseline_tree)) {
      throw UserError("working tree has uncommitted changes; commit or amend before switching changes");
    }
  }
  const git_oid previous_operation = repo.ensure_operation();
  const auto previous_aliases = repo.data_refs();
  repo.record(std::move(plan.updates), std::move(deletes), head, operation);
  try {
    repo.set_head(head);
    if (checkout.has_value() != previous_head.has_value() ||
        (checkout.has_value() && !(*checkout == *previous_head))) {
      repo.checkout(checkout);
    }
  } catch (const std::exception& error) {
    const std::string original = error.what();
    try {
      repo.restore_operation(previous_operation, "", true, true, false,
                             &previous_aliases);
    } catch (const std::exception& recovery) {
      repo.clear_checkout_recovery();
      throw UserError(original + "; could not restore the previous operation: " + recovery.what());
    }
    throw;
  }
}

void edit_file_with_editor(Repository& repo,
                           const std::filesystem::path& path) {
  std::string editor;
  if (const char* value = std::getenv("GIT_EDITOR");  // GG_COV_EXCL_BRANCH
      value != nullptr && *value != '\0') {  // GG_COV_EXCL_BRANCH
    editor = value;
  } else if (const auto value = config_value(repo, "core.editor");  // GG_COV_EXCL_BRANCH
             value.has_value() && !value->empty()) {  // GG_COV_EXCL_BRANCH
    editor = *value;
  } else if (const char* value = std::getenv("VISUAL");  // GG_COV_EXCL_BRANCH
             value != nullptr && *value != '\0') {  // GG_COV_EXCL_BRANCH
    editor = value;
  } else if (const char* value = std::getenv("EDITOR");  // GG_COV_EXCL_BRANCH
             value != nullptr && *value != '\0') {  // GG_COV_EXCL_BRANCH
    editor = value;
  } else {
    throw UserError(
        "GIT_EDITOR, core.editor, VISUAL, or EDITOR must name an editor");
  }
  std::vector<std::string> argument_storage = split_command(editor);
  if (argument_storage.empty()) {
    throw UserError("editor command must name an executable");
  }
  argument_storage.push_back(path.string());
  const std::optional<int> exit_code = run_process(argument_storage);
  if (!exit_code.has_value()) throw UserError("cannot launch editor");
  if (*exit_code != 0) {
    throw UserError("editor exited unsuccessfully");
  }
}

std::string edit_text(Repository& repo, std::string_view initial) {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "gg-edit-XXXXXX").string();
  const int descriptor = create_temporary_file(pattern.data());
  if (descriptor < 0) throw UserError("cannot create editor file");  // GG_COV_EXCL_BRANCH
#ifdef _MSC_VER
  _close(descriptor);
#else
  close(descriptor);
#endif
  std::ofstream(pattern) << initial;
  try {
    edit_file_with_editor(repo, pattern);
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

  std::vector<std::string> environment_storage;
  std::vector<char*> environment;
  char** child_environment = process_environment();
  git_repository* raw_repository = nullptr;
  const std::string repository_path = repository.string();
  if (git_repository_open_ext(&raw_repository, repository_path.c_str(), 0,
                              nullptr) == 0) {
    RepositoryPtr discovered(raw_repository);
    if (git_repository_is_bare(discovered.get()) == 0) {
      constexpr std::string_view prefix = "GG_WORKSPACE_ROOT=";
      for (char** entry = process_environment(); *entry != nullptr; ++entry) {
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

  const std::optional<int> exit_code =
      run_process(argument_storage, child_environment);
  if (!exit_code.has_value()) {
    throw UserError("cannot execute external command: " + options.command);
  }
  return *exit_code;
}

void command_util_gc(Repository& repo,
                     const UtilGcCommand& options,
                     std::ostream& output) {
  repo.sync_for_command();
  (void)repo.collect_expired_aliases("gg util gc");
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

void command_util_optimize(Repository& repo, std::ostream& output) {
  (void)repo.collect_expired_aliases("gg util optimize");
  UtilExecCommand command{
      "git", {"--git-dir=" + std::string(git_repository_path(repo.raw())),
              "commit-graph", "write", "--reachable"}};
  if (command_util_exec(command, git_repository_path(repo.raw())) != 0) {
    throw UserError("commit-graph optimization failed");
  }
  output << "Repository optimization completed.\n";
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
  const int descriptor = create_temporary_file(temporary_template.data());
  if (descriptor < 0) throw UserError("cannot create Git pre-push hook");  // GG_COV_EXCL_BRANCH
#ifdef _MSC_VER
  _close(descriptor);
#else
  close(descriptor);
#endif
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
    check(git_oid_fromstr(&oid, local_oid.c_str(),
                          git_repository_oid_type(repo.raw())),
          "parse pushed object");
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
  const auto native_worktree = [&](std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), "worktree");
    arguments.insert(arguments.begin(),
                     "--git-dir=" + std::string(git_repository_commondir(repo.raw())));
    arguments.insert(arguments.begin(), root.string());
    arguments.insert(arguments.begin(), "-C");
    return command_util_exec(UtilExecCommand{"git", std::move(arguments)}, root);
  };
  // Repair must run before enumeration: broken native links can prevent libgit2
  // from looking up the worktrees that Git is about to repair.
  if (options.action == WorkspaceAction::repair) {
    WorkspaceLock lock(repo.raw());
    std::vector<std::string> arguments{"repair"};
    for (const std::string& path : options.paths) {
      const auto destination = std::filesystem::absolute(path).lexically_normal();
      if (path.empty() || !std::filesystem::is_directory(destination) ||
          !std::filesystem::exists(destination / ".git")) {
        throw UserError("not a worktree path: " + path);
      }
      arguments.push_back(destination.string());
    }
    if (native_worktree(std::move(arguments)) != 0) {
      throw UserError("cannot repair workspaces; check the supplied worktree paths");
    }
    for (const WorkspaceRecord& record : repo.workspaces()) {
      // Remembered roots also belong to native worktrees moved by gg.
      // Refreshing this cache does not adopt them or create working refs.
      if (!record.stale) {
        repo.remember_workspace_root(record.name, record.root);
      }
    }
    output << "Repaired workspace links.\n";
    return;
  }
  // Serialize gg lifecycle commands and enumerate only after taking the lock.
  std::unique_ptr<WorkspaceLock> lifecycle_lock;
  if (options.action != WorkspaceAction::list &&
      options.action != WorkspaceAction::root) {
    lifecycle_lock = std::make_unique<WorkspaceLock>(repo.raw());
  }
  if (options.action == WorkspaceAction::add) repo.sync_for_command();
  const auto workspace_reference = repo.workspace_ref();
  const auto workspace = repo.workspace();
  const std::string workspace_name = repo.workspace_name();
  const auto workspace_records = repo.workspaces();
  std::map<std::string, std::filesystem::path> roots;
  for (const WorkspaceRecord& record : workspace_records) {
    if (!record.root.empty()) roots.emplace(record.name, record.root);
  }
  if (options.action == WorkspaceAction::root) {
    if (options.name.empty()) {
      output << root.string() << '\n';
      return;
    }
    const std::string reference =
        std::string(kWorkspacePrefix) + options.name;
    const auto found = roots.find(options.name);
    if (found == roots.end()) {
      throw UserError("workspace not found: " + options.name);
    }
    output << std::filesystem::weakly_canonical(found->second).string() << '\n';
    return;
  }
  if (options.action == WorkspaceAction::add) {
    if (options.destination.empty()) throw UserError("workspace destination must not be empty");
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
    if (!options.name.empty() &&
        std::ranges::any_of(workspace_records, [&](const WorkspaceRecord& item) {
          return item.name == options.name;
        })) {
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
    // Two identical empty changes created in one second must still have
    // independent identities; rewriting one must not target another workspace.
    SignaturePtr actor = repo.signature();
    git_oid working{};
    do {
      working = repo.create_commit(*git_commit_tree_id(parent_commit.get()),
                                   {parent}, options.message, actor.get(), actor.get());
      ++actor->when.time;
    } while (std::ranges::any_of(workspace_records, [&](const WorkspaceRecord& record) {
      return record.managed && record.working_copy.has_value() &&
             git_oid_equal(&*record.working_copy, &working) != 0;
    }));
    UtilExecCommand command{
        "git",
        {"--git-dir=" + std::string(git_repository_commondir(repo.raw())),
         "worktree", "add", "--quiet", "--detach", destination.string(),
         oid_string(parent)}};
    if (command_util_exec(command, git_repository_path(repo.raw())) != 0) {
      throw UserError("cannot create linked workspace");
    }
    std::string created_name;
    try {
      Repository linked(destination);
      created_name = linked.workspace_name();
      if (!options.name.empty()) linked.set_workspace_name(options.name);
      created_name = linked.workspace_name();
      if (options.sparse_patterns == "empty") {
        const UtilExecCommand sparse_command{
            "git", {"-C", destination.string(), "sparse-checkout", "set",
                    "--no-cone", "!/*"}};
        if (command_util_exec(sparse_command, destination) != 0) throw UserError("cannot initialize empty sparse workspace");  // GG_COV_EXCL_BRANCH
      } else if (options.sparse_patterns == "copy") {  // GG_COV_EXCL_BRANCH
        const std::filesystem::path current_patterns =
            std::filesystem::path(git_repository_path(repo.raw())) / "info" /
            "sparse-checkout";
        if (std::filesystem::exists(current_patterns)) {
          const UtilExecCommand initialize_sparse{
              "git", {"-C", destination.string(), "sparse-checkout", "init",
                      "--no-cone"}};
          if (command_util_exec(initialize_sparse, destination) != 0) throw UserError("cannot initialize copied sparse workspace");  // GG_COV_EXCL_BRANCH
          const std::filesystem::path linked_patterns =
              std::filesystem::path(git_repository_path(linked.raw())) / "info" /
              "sparse-checkout";
          std::filesystem::create_directories(linked_patterns.parent_path());
          std::filesystem::copy_file(
              current_patterns, linked_patterns,
              std::filesystem::copy_options::overwrite_existing);
          const UtilExecCommand apply_sparse{
              "git", {"-C", destination.string(), "sparse-checkout", "reapply"}};
          if (command_util_exec(apply_sparse, destination) != 0) throw UserError("cannot apply copied sparse patterns");  // GG_COV_EXCL_BRANCH
        }
      }
      const std::string name = linked.workspace_name();
      repo.record({{std::string(kWorkspacePrefix) + name, working}},
                  {}, repo.head_state(), "gg workspace add " + name, true);
    } catch (...) {
      // Only this newly created worktree is eligible for rollback. Native Git
      // owns its administration and refuses unexpected locks or submodules.
      if (native_worktree({"remove", "--force", destination.string()}) != 0) {
        throw UserError("workspace initialization failed; cannot roll back worktree at " +
                        destination.string() + "; inspect it before removing it");
      }
      if (!created_name.empty()) repo.forget_workspace_root(created_name);
      if (!options.name.empty()) repo.forget_workspace_root(options.name);
      throw;
    }
    output << "Created workspace " << created_name << " at " << destination.string()
           << ".\n";
    return;
  }
  if (options.action == WorkspaceAction::move ||
      options.action == WorkspaceAction::lock ||
      options.action == WorkspaceAction::unlock) {
    const auto selected = std::ranges::find(workspace_records, options.name,
                                            &WorkspaceRecord::name);
    if (selected == workspace_records.end()) {
      throw UserError("workspace not found: " + options.name);
    }
    if (selected->primary || selected->worktree_id.empty()) {
      throw UserError("workspace has no linked worktree: " + options.name);
    }
    if (options.action == WorkspaceAction::move) {
      if (selected->current) {
        throw UserError("cannot move the current workspace; run this command from another checkout");
      }
      if (selected->stale) throw UserError("workspace is stale; repair it before moving: " + options.name);
      if (selected->locked) throw UserError("workspace is locked: " + options.name);
      if (options.destination.empty()) throw UserError("workspace destination must not be empty");
      const auto destination = std::filesystem::absolute(options.destination).lexically_normal();
      if (std::filesystem::exists(destination) || std::filesystem::is_symlink(destination)) {
        throw UserError("workspace destination already exists: " + destination.string());
      }
      // Pre-write the only gg metadata that changes, then restore it if Git
      // refuses the move. Worktree IDs, names, refs and histories stay intact.
      repo.remember_workspace_root(selected->name, destination);
      try {
        if (native_worktree({"move", selected->root.string(), destination.string()}) != 0) {
          throw UserError("cannot move workspace: " + options.name);
        }
      } catch (...) {
        repo.remember_workspace_root(selected->name, selected->root);
        throw;
      }
      output << "Moved workspace " << options.name << " to " << destination.string() << ".\n";
      return;
    }
    git_worktree* raw_worktree = nullptr;
    check(git_worktree_lookup(&raw_worktree, repo.raw(), selected->worktree_id.c_str()),
          "read linked worktree");
    WorktreePtr worktree(raw_worktree);
    if (options.action == WorkspaceAction::lock) {
      if (selected->locked) throw UserError("workspace is already locked: " + options.name);
      check(git_worktree_lock(worktree.get(), options.reason.empty() ? nullptr : options.reason.c_str()),
            "lock workspace");
      output << "Locked workspace " << options.name << ".\n";
    } else {
      if (!selected->locked) throw UserError("workspace is not locked: " + options.name);
      check(git_worktree_unlock(worktree.get()), "unlock workspace");
      output << "Unlocked workspace " << options.name << ".\n";
    }
    return;
  }
  if (options.action == WorkspaceAction::prune) {
    if (!options.dry_run) {
      std::set<std::string> candidates;
      std::set<std::string> retained_ids;
      for (const auto& record : workspace_records) {
        if (record.stale && !record.locked) {
          if (record.managed) candidates.insert(std::string(kWorkspacePrefix) + record.name);
        } else if (!record.worktree_id.empty()) {
          retained_ids.insert(record.worktree_id);
        }
      }
      for (const std::string_view prefix : {"refs/gg/operations/worktrees/", "refs/gg/rewrites/"}) {
        for (const auto& [reference, oid] : repo.refs_with_prefix(prefix)) {
          (void)oid;
          if (!retained_ids.contains(reference.substr(prefix.size()))) candidates.insert(reference);
        }
      }
      prevalidate_workspace_deletions(repo, candidates);
    }
    std::vector<std::string> arguments{"prune", "--verbose", "--expire=" + options.expire};
    if (options.dry_run) arguments.emplace_back("--dry-run");
    if (native_worktree(std::move(arguments)) != 0) {
      throw UserError("cannot prune workspaces");
    }
    if (options.dry_run) {
      output << "Dry run; workspace state unchanged.\n";
      return;
    }
    const auto remaining = repo.workspaces();
    std::set<std::string> live_ids;
    for (const auto& record : remaining) {
      if (!record.worktree_id.empty()) live_ids.insert(record.worktree_id);
    }
    std::set<std::string> deletes;
    std::set<std::string> forgotten;
    for (const auto& record : workspace_records) {
      if (!record.worktree_id.empty() && !live_ids.contains(record.worktree_id)) {
        forgotten.insert(record.name);
      }
    }
    for (const auto& record : remaining) {
      if (record.stale && record.worktree_id.empty() && record.managed) {
        deletes.insert(std::string(kWorkspacePrefix) + record.name);
        forgotten.insert(record.name);
      }
    }
    for (const std::string_view prefix : {"refs/gg/operations/worktrees/", "refs/gg/rewrites/"}) {
      for (const auto& [reference, oid] : repo.refs_with_prefix(prefix)) {
        (void)oid;
        if (!live_ids.contains(reference.substr(prefix.size()))) {
          deletes.insert(reference);
        }
      }
    }
    if (!deletes.empty()) {
      repo.record({}, std::move(deletes), repo.head_state(), "gg workspace prune", true);
    }
    for (const auto& name : forgotten) repo.forget_workspace_root(name);
    output << "Pruned missing workspaces.\n";
    return;
  }
  if (options.action == WorkspaceAction::rename) {
    const std::string old_name =
        options.workspace.empty() ? workspace_name : options.workspace;
    const auto selected = std::ranges::find(workspace_records, old_name,
                                            &WorkspaceRecord::name);
    if (selected == workspace_records.end() || !selected->managed) {
      throw UserError("workspace not found: " + old_name);
    }
    if (options.name == old_name) {
      output << "Nothing changed.\n";
      return;
    }
    const std::string renamed = std::string(kWorkspacePrefix) + options.name;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, renamed.c_str()),
          "validate workspace name");
    if (valid == 0) throw UserError("invalid workspace name: " + options.name);
    if (std::ranges::any_of(workspace_records, [&](const WorkspaceRecord& item) {
          return item.name == options.name;
        }) ||
        repo.ref_target(renamed).has_value()) {
      throw UserError("workspace already exists: " + options.name);
    }
    Repository* controller = &repo;
    std::unique_ptr<Repository> target;
    if (!selected->current &&
        (!selected->stale || !selected->worktree_id.empty())) {
      const std::filesystem::path target_path = selected->stale
          ? std::filesystem::path(git_repository_commondir(repo.raw())) /
                "worktrees" / selected->worktree_id
          : selected->root;
      target = std::make_unique<Repository>(target_path, selected->stale);
      controller = target.get();
    }
    const std::string old_reference = std::string(kWorkspacePrefix) + old_name;
    const auto old_oid = repo.ref_target(old_reference);
    if (!old_oid.has_value()) throw UserError("workspace not found: " + old_name);
    if (target != nullptr || !selected->stale) {
      (void)controller->ensure_operation();
      controller->set_workspace_name(options.name);
    }
    try {
      controller->record({{renamed, *old_oid}}, {old_reference},
                         controller->head_state(),
                         "gg workspace rename " + old_name + " " +
                             options.name,
                         true);
    } catch (...) {
      if (target != nullptr || !selected->stale) {
        controller->set_workspace_name(old_name);
      }
      throw;
    }
    if (selected->stale) {
      repo.remember_workspace_root(options.name, selected->root);
    }
    repo.forget_workspace_root(old_name);
    return;
  }
  if (options.action == WorkspaceAction::remove) {
    const auto selected = std::ranges::find(workspace_records, options.name,
                                            &WorkspaceRecord::name);
    if (selected == workspace_records.end()) {
      throw UserError("workspace not found: " + options.name);
    }
    if (selected->primary) {
      throw UserError("cannot remove the primary workspace");
    }
    if (selected->current) {
      throw UserError("cannot remove the current workspace; run this command from another checkout");
    }

    std::set<std::string> deletes;
    const std::string workspace_ref = std::string(kWorkspacePrefix) + options.name;
    if (repo.ref_target(workspace_ref).has_value()) deletes.insert(workspace_ref);
    if (!selected->worktree_id.empty()) {
      for (const std::string& reference : {
               "refs/gg/operations/worktrees/" + selected->worktree_id,
               "refs/gg/rewrites/" + selected->worktree_id}) {
        deletes.insert(reference);
      }
    }
    prevalidate_workspace_deletions(repo, deletes);

    if (!selected->worktree_id.empty()) {
      git_worktree* raw_worktree = nullptr;
      check(git_worktree_lookup(&raw_worktree, repo.raw(),
                                selected->worktree_id.c_str()),
            "read linked worktree");
      WorktreePtr worktree(raw_worktree);
      git_buf reason = GIT_BUF_INIT;
      const int locked = git_worktree_is_locked(&reason, worktree.get());
      git_buf_dispose(&reason);
      check(locked, "inspect worktree lock");
      if (locked != 0) throw UserError("workspace is locked: " + options.name);

      if (selected->stale) {
        (void)repo.ensure_operation();
        check(git_worktree_prune(worktree.get(), nullptr),
              "prune stale workspace");
      } else {
        if (selected->managed) {
          Repository target(selected->root);
          // Explicit-commit callers never snapshot implicitly; uncommitted
          // edits must fail the loss check below instead of being absorbed
          // into a commit that removal would leave unreachable.
          if (repo.synchronizes_commands()) target.sync_workspace();
          repo.invalidate_ref_cache();
          const auto target_workspace = target.workspace();
          if (target_workspace.has_value() &&
              target.commit_has_conflicts(*target_workspace)) {
            throw UserError("workspace contains conflicts: " + options.name);
          }

          git_status_options status_options = GIT_STATUS_OPTIONS_INIT;
          status_options.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
          status_options.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
                                 GIT_STATUS_OPT_INCLUDE_IGNORED |
                                 GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS |
                                 GIT_STATUS_OPT_RECURSE_IGNORED_DIRS;
          git_status_list* raw_status = nullptr;
          check(git_status_list_new(&raw_status, target.raw(), &status_options),
                "verify workspace snapshot");
          GitPtr<git_status_list, git_status_list_free> statuses(raw_status);
          constexpr unsigned int unsafe =
              GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED |
              GIT_STATUS_WT_DELETED | GIT_STATUS_WT_TYPECHANGE |
              GIT_STATUS_WT_RENAMED | GIT_STATUS_IGNORED |
              GIT_STATUS_CONFLICTED;
          for (std::size_t index = 0;
               index < git_status_list_entrycount(statuses.get()); ++index) {
            const git_status_entry* entry =
                git_status_byindex(statuses.get(), index);
            if (entry != nullptr && (entry->status & unsafe) != 0) {
              throw UserError(
                  "workspace has files that would be lost: " + options.name);
            }
          }
        }

        prevalidate_workspace_deletions(repo, deletes);
        (void)repo.ensure_operation();
        UtilExecCommand remove{"git", {"--git-dir=" +
                                           std::string(git_repository_commondir(repo.raw())),
                                       "worktree", "remove"}};
        if (selected->managed) remove.arguments.push_back("--force");
        remove.arguments.push_back(selected->root.string());
        if (command_util_exec(remove, git_repository_workdir(repo.raw())) != 0) {
          throw UserError("cannot remove workspace: " + options.name);
        }
      }
    }

    repo.record({}, std::move(deletes), repo.head_state(),
                "gg workspace remove " + options.name, true);
    repo.forget_workspace_root(options.name);
    output << "Removed workspace " << options.name << ".\n";
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
  if (workspace_records.empty()) {
    output << "No workspaces.\n";
    return;
  }
  for (const WorkspaceRecord& record : workspace_records) {
    const std::string id = record.working_copy.has_value()
                               ? repo.short_commit_id(*record.working_copy).value
                               : "(unborn)";
    const std::string workspace_root = record.root.empty()
        ? "(stale)"
        : std::filesystem::weakly_canonical(record.root).string();
    output << record.name << ": " << id << ' ' << workspace_root;
    if (record.current) output << " (current)";
    if (record.primary) output << " (primary)";
    if (!record.managed) output << " (unmanaged)";
    if (record.stale) output << " (stale)";
    if (record.locked) {
      output << " (locked";
      if (!record.lock_reason.empty()) output << ": " << record.lock_reason;
      output << ')';
    }
    output << '\n';
  }
}

void command_sparse(Repository& repo,
                    const SparseCommand& options,
                    std::ostream& output) {
  if (!repo.workspace().has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  const std::filesystem::path patterns =
      std::filesystem::path(git_repository_path(repo.raw())) / "info" /
      "sparse-checkout";
  if (options.action == SparseAction::list) {
    if (!std::filesystem::exists(patterns)) {
      output << ".\n";
      return;
    }
    std::ifstream input(patterns);
    output << input.rdbuf();
    return;
  }
  WorkspaceLock lock(repo.raw());
  const std::filesystem::path root = git_repository_workdir(repo.raw());
  const UtilExecCommand command{
      "git", {"-C", root.string(), "sparse-checkout", "disable"}};
  if (command_util_exec(command, root) != 0) throw UserError("cannot reset sparse workspace");  // GG_COV_EXCL_BRANCH
}

}  // namespace gg::detail
