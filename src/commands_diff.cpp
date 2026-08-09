// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <utility>

extern char** environ;

namespace gg::detail {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "gg-diff-XXXXXX").string();
    const char* created = mkdtemp(pattern.data());
    if (created == nullptr) throw UserError("cannot create diff directory");  // GG_COV_EXCL_BRANCH
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::string tool_program(Repository& repo,
                         std::string_view family,
                         std::string_view tool) {
  const std::string key = std::string(family) + "." + std::string(tool) +
                          ".path";
  return config_value(repo, key).value_or(std::string(tool));
}

std::vector<std::string> tool_command(Repository& repo,
                                      std::string_view family,
                                      std::string_view tool,
                                      const std::filesystem::path& left,
                                      const std::filesystem::path& right) {
  const std::string key = std::string(family) + "." + std::string(tool) +
                          ".cmd";
  if (const auto command = config_value(repo, key); command.has_value()) {
    return {"/bin/sh", "-c",
            "LOCAL=\"$1\"; REMOTE=\"$2\"; MERGED=\"$3\"; "
            "export LOCAL REMOTE MERGED; " +
                *command,
            "gg-tool", left.string(), right.string(), right.string()};
  }
  return {tool_program(repo, family, tool), left.string(), right.string()};
}

git_oid tree_id(Repository& repo, const git_oid& revision) {
  CommitPtr commit = repo.commit(revision);
  return *git_commit_tree_id(commit.get());
}

git_oid parent_tree_id(Repository& repo, const git_oid& revision) {
  const std::vector<git_oid> parents = repo.parents(revision);
  return combined_tree(repo, parents);
}

std::optional<std::pair<git_oid, git_oid>> revision_set_trees(
    Repository& repo, std::string_view expression) {
  const std::vector<git_oid> resolved = repo.resolve_set(expression);
  if (resolved.empty()) return std::nullopt;
  const std::set<git_oid, OidLess> selected(resolved.begin(), resolved.end());
  std::set<git_oid, OidLess> roots;
  std::set<git_oid, OidLess> heads = selected;
  for (const git_oid& oid : selected) {
    for (const git_oid& parent : repo.parents(oid)) {
      if (selected.contains(parent)) {
        heads.erase(parent);
      } else {
        roots.insert(parent);
      }
    }
  }
  for (const git_oid& root : roots) {
    for (const git_oid& oid : selected) {
      const int gap = git_graph_descendant_of(repo.raw(), &root, &oid);
      check(gap, "validate revision-set diff");
      if (gap != 0) {
        throw UserError("diff revision set contains a gap");
      }
    }
  }
  const std::vector<git_oid> root_values(roots.begin(), roots.end());
  const std::vector<git_oid> head_values(heads.begin(), heads.end());
  return std::pair(combined_tree(repo, root_values),
                   combined_tree(repo, head_values));
}

int collect_tree_path(const char* root,
                      const git_tree_entry* entry,
                      void* payload) {
  if (git_tree_entry_type(entry) != GIT_OBJECT_TREE) {
    static_cast<std::vector<std::string>*>(payload)->push_back(
        std::string(root) + git_tree_entry_name(entry));
  }
  return 0;
}

std::vector<std::string> matching_tree_paths(
    Repository& repo,
    const git_oid& tree_oid,
    const std::vector<std::string>& filesets) {
  if (filesets.empty()) return {};
  for (const std::string& fileset : filesets) {
    (void)fileset_matches(fileset, "");
  }
  TreePtr tree = repo.tree(tree_oid);
  std::vector<std::string> paths;
  check(git_tree_walk(tree.get(), GIT_TREEWALK_PRE, collect_tree_path, &paths),
        "walk diff tree");
  std::erase_if(paths, [&](const std::string& path) {
    return std::ranges::none_of(filesets, [&](const std::string& fileset) {
      return fileset_matches(fileset, path);
    });
  });
  return paths;
}

void export_tree(Repository& repo,
                 const git_oid& tree_oid,
  const std::vector<std::string>& path_values,
                 const std::filesystem::path& destination) {
  std::filesystem::create_directory(destination);
  std::vector<std::string> paths =
      matching_tree_paths(repo, tree_oid, path_values);
  if (!path_values.empty() && paths.empty()) {
    paths.emplace_back("/.gg-fileset-no-match");
  }
  std::vector<char*> path_pointers;
  path_pointers.reserve(paths.size());
  for (const std::string& path : paths) {
    path_pointers.push_back(const_cast<char*>(path.c_str()));
  }
  TreePtr tree = repo.tree(tree_oid);
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_FORCE |
                              GIT_CHECKOUT_DONT_UPDATE_INDEX |
                              GIT_CHECKOUT_DONT_WRITE_INDEX |
                              GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
  const std::string target = destination.string();
  options.target_directory = target.c_str();
  options.paths = {path_pointers.data(), path_pointers.size()};
  check(git_checkout_tree(repo.raw(),
                          reinterpret_cast<const git_object*>(tree.get()),
                          &options),
        "export diff tree");
}

void render_external_diff(Repository& repo,
                          const git_oid& from_tree,
                          const git_oid& to_tree,
                          const std::vector<std::string>& paths,
                          const DiffFormatOptions& format,
                          std::ostream& output) {
  TemporaryDirectory temporary;
  const std::filesystem::path left = temporary.path() / "left";
  const std::filesystem::path right = temporary.path() / "right";
  const std::filesystem::path captured = temporary.path() / "stdout";
  export_tree(repo, from_tree, paths, left);
  export_tree(repo, to_tree, paths, right);

  std::vector<std::string> storage =
      tool_command(repo, "difftool", format.tool, left, right);
  const std::string& program = storage.front();
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) throw UserError("cannot prepare external diff tool");  // GG_COV_EXCL_BRANCH
  const int opened = posix_spawn_file_actions_addopen(
      &actions, STDOUT_FILENO, captured.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
      0600);
  if (opened != 0) { posix_spawn_file_actions_destroy(&actions); throw UserError("cannot capture external diff output"); }  // GG_COV_EXCL_BRANCH
  pid_t process = 0;
  const int spawned = posix_spawnp(&process, program.c_str(), &actions,
                                   nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) {
    throw UserError("cannot execute external diff tool: " + program);
  }
  int status = 0;
  if (waitpid(process, &status, 0) < 0) throw UserError("cannot wait for external diff tool");  // GG_COV_EXCL_BRANCH
  if (!WIFEXITED(status)) throw UserError("external diff tool terminated by a signal");  // GG_COV_EXCL_BRANCH
  const int exit_code = WEXITSTATUS(status);
  const bool accepted = exit_code == 0 ||
                        (format.tool == "diff" && exit_code == 1);  // GG_COV_EXCL_BRANCH
  if (!accepted) {
    throw UserError("external diff tool exited with status " +
                    std::to_string(exit_code));
  }
  std::ifstream input(captured);
  output << input.rdbuf();
}

git_oid import_tree(Repository& repo,
                    const git_oid& base_tree,
                    const std::vector<std::string>& paths,
                    const std::filesystem::path& source) {
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo.raw()),
        "create edited tree index");
  IndexPtr index(raw_index);
  TreePtr base = repo.tree(base_tree);
  check(git_index_read_tree(index.get(), base.get()), "prepare edited tree");
  const std::vector<std::string> selectors =
      paths.empty() ? std::vector<std::string>{"."} : paths;
  std::vector<std::string> removed_paths;
  for (std::size_t item = 0; item < git_index_entrycount(index.get()); ++item) {
    const git_index_entry* entry = git_index_get_byindex(index.get(), item);
    if (std::ranges::any_of(selectors, [&](const std::string& selector) {
          return fileset_matches(selector, entry->path);
        })) {
      removed_paths.emplace_back(entry->path);
    }
  }
  for (const std::string& path : removed_paths) {
    check(git_index_remove_bypath(index.get(), path.c_str()),
          "remove edited path");
  }
  for (const auto& item : std::filesystem::recursive_directory_iterator(source)) {
    const std::filesystem::file_status status = item.symlink_status();
    if (!std::filesystem::is_regular_file(status) &&
        !std::filesystem::is_symlink(status)) {
      continue;
    }
    const std::string path =
        std::filesystem::relative(item.path(), source).generic_string();
    std::string contents;
    std::uint32_t mode = GIT_FILEMODE_BLOB;
    if (std::filesystem::is_symlink(status)) {
      contents = std::filesystem::read_symlink(item.path()).generic_string();
      mode = GIT_FILEMODE_LINK;
    } else {
      std::ifstream input(item.path(), std::ios::binary);
      contents.assign(std::istreambuf_iterator<char>(input),
                      std::istreambuf_iterator<char>());
      if ((status.permissions() & std::filesystem::perms::owner_exec) !=
          std::filesystem::perms::none) {
        mode = GIT_FILEMODE_BLOB_EXECUTABLE;
      }
    }
    git_index_entry entry{};
    entry.mode = mode;
    entry.file_size = contents.size();
    entry.path = path.c_str();
    check(git_index_add_frombuffer(index.get(), &entry, contents.data(),
                                   contents.size()),
          "import edited file");
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, index.get(), repo.raw()),
        "write edited tree");
  repo.preserve_conflicts(base_tree, result);
  return result;
}

git_oid run_external_editor(Repository& repo,
                            const git_oid& left_tree,
                            const git_oid& right_tree,
                            const std::vector<std::string>& paths,
                            std::string_view tool) {
  TemporaryDirectory temporary;
  const std::filesystem::path left = temporary.path() / "left";
  const std::filesystem::path right = temporary.path() / "right";
  export_tree(repo, left_tree, paths, left);
  export_tree(repo, right_tree, paths, right);
  std::vector<std::string> storage =
      tool_command(repo, "mergetool", tool, left, right);
  const std::string& program = storage.front();
  std::vector<char*> argv;
  argv.reserve(storage.size() + 1);
  for (std::string& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  pid_t process = 0;
  const int spawned = posix_spawnp(&process, program.c_str(), nullptr, nullptr,
                                   argv.data(), environ);
  if (spawned != 0) {
    throw UserError("cannot execute external diff editor: " + program);
  }
  int status = 0;
  if (waitpid(process, &status, 0) < 0) throw UserError("cannot wait for external diff editor");  // GG_COV_EXCL_BRANCH
  if (!WIFEXITED(status)) throw UserError("external diff editor terminated by a signal");  // GG_COV_EXCL_BRANCH
  if (WEXITSTATUS(status) != 0) {
    throw UserError("external diff editor exited with status " +
                    std::to_string(WEXITSTATUS(status)));
  }
  return import_tree(repo, right_tree, paths, right);
}

DiffPtr create_diff(Repository& repo,
                    const git_oid& from_tree_id,
                    const git_oid& to_tree_id,
                    const std::vector<std::string>& path_values,
                    const DiffFormatOptions& format) {
  TreePtr from_tree = repo.tree(from_tree_id);
  TreePtr to_tree = repo.tree(to_tree_id);
  const auto build = [&](const std::vector<std::string>& paths,
                         bool exact_paths) {
    std::vector<char*> path_pointers;
    path_pointers.reserve(paths.size());
    for (const std::string& path : paths) {
      path_pointers.push_back(const_cast<char*>(path.c_str()));
    }
    git_diff_options options = GIT_DIFF_OPTIONS_INIT;
    options.context_lines = format.context;
    options.pathspec = {path_pointers.data(), path_pointers.size()};
    options.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
    if (exact_paths) options.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
    if (format.ignore_all_space) {
      options.flags |= GIT_DIFF_IGNORE_WHITESPACE;
    }
    if (format.ignore_space_change) {
      options.flags |= GIT_DIFF_IGNORE_WHITESPACE_CHANGE;
    }
    git_diff* raw_diff = nullptr;
    check(git_diff_tree_to_tree(&raw_diff, repo.raw(), from_tree.get(),
                                to_tree.get(), &options),
          "compare revisions");
    DiffPtr diff(raw_diff);
    git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
    find_options.flags = GIT_DIFF_FIND_RENAMES;
    check(git_diff_find_similar(diff.get(), &find_options),
          "find renamed files");
    return diff;
  };

  DiffPtr diff = build({}, false);
  if (path_values.empty()) return diff;
  for (const std::string& fileset : path_values) {
    (void)fileset_matches(fileset, "");
  }
  std::set<std::string> selected;
  for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
    const auto matches = [&](const char* path) {
      return std::ranges::any_of(path_values, [&](const auto& fileset) {
        return fileset_matches(fileset, path);
      });
    };
    if (matches(delta->old_file.path) || matches(delta->new_file.path)) {  // GG_COV_EXCL_BRANCH
      selected.insert(delta->old_file.path);
      selected.insert(delta->new_file.path);
    }
  }
  std::vector<std::string> paths(selected.begin(), selected.end());
  if (paths.empty()) paths.emplace_back("/.gg-fileset-no-match");
  return build(paths, true);
}

std::string display_path(const git_diff_delta& delta) {
  const std::string old_path = delta.old_file.path;
  const std::string new_path = delta.new_file.path;
  return old_path == new_path ? new_path
                              : "{" + old_path + " => " + new_path + "}";
}

char mode_type(std::uint16_t mode) {
  return mode == GIT_FILEMODE_UNREADABLE
             ? '-'
             : mode == GIT_FILEMODE_LINK
                   ? 'L'
                   : mode == GIT_FILEMODE_COMMIT ? 'G' : 'F';  // GG_COV_EXCL_BRANCH
}

OutputStyle delta_style(git_delta_t status) {
  return status == GIT_DELTA_ADDED     ? OutputStyle::added
         : status == GIT_DELTA_DELETED ? OutputStyle::removed
                                        : OutputStyle::modified;
}

void render_summary(git_diff* diff, std::ostream& output) {
  for (std::size_t index = 0; index < git_diff_num_deltas(diff); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff, index);
    const std::string line =
        std::string(1, git_diff_status_char(delta->status)) + " " +
        display_path(*delta);
    output << styled(output, line, delta_style(delta->status)) << '\n';
  }
}

void render_types(git_diff* diff, std::ostream& output) {
  for (std::size_t index = 0; index < git_diff_num_deltas(diff); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff, index);
    output << mode_type(delta->old_file.mode) << mode_type(delta->new_file.mode)
           << ' ' << display_path(*delta) << '\n';
  }
}

void render_names(git_diff* diff, std::ostream& output) {
  for (std::size_t index = 0; index < git_diff_num_deltas(diff); ++index) {
    output << display_path(*git_diff_get_delta(diff, index)) << '\n';
  }
}

void render_stat(git_diff* diff, std::ostream& output) {
  git_diff_stats* raw_stats = nullptr;
  check(git_diff_get_stats(&raw_stats, diff), "calculate diff statistics");
  DiffStatsPtr stats(raw_stats);
  git_buf buffer = GIT_BUF_INIT;
  const auto format = static_cast<git_diff_stats_format_t>(
      GIT_DIFF_STATS_FULL | GIT_DIFF_STATS_NUMBER);
  check(git_diff_stats_to_buf(&buffer, stats.get(), format, 80),
        "format diff statistics");
  if (buffer.size != 0) {  // GG_COV_EXCL_BRANCH
    output.write(buffer.ptr, static_cast<std::streamsize>(buffer.size));
  }
  git_buf_dispose(&buffer);
}

void render_word_line(std::string_view line,
                      std::string_view other,
                      OutputStyle style,
                      std::ostream& output) {
  const std::string_view value = line.substr(1);
  const std::string_view compared = other.substr(1);
  const std::size_t limit = std::min(value.size(), compared.size());
  std::size_t prefix = 0;
  while (prefix < limit && value[prefix] == compared[prefix]) ++prefix;
  std::size_t suffix = 0;
  while (suffix < limit - prefix &&
         value[value.size() - suffix - 1] ==
             compared[compared.size() - suffix - 1]) {
    ++suffix;
  }
  output << line.front() << value.substr(0, prefix);
  const std::string_view changed =
      value.substr(prefix, value.size() - prefix - suffix);
  if (!changed.empty()) output << styled(output, changed, style);
  if (suffix != 0) output << value.substr(value.size() - suffix);
}

void render_patch(git_diff* diff,
                  bool color_words,
                  std::ostream& output) {
  git_buf buffer = GIT_BUF_INIT;
  check(git_diff_to_buf(&buffer, diff, GIT_DIFF_FORMAT_PATCH),
        "format Git patch");
  if (buffer.size != 0) {
    const std::string_view patch(buffer.ptr, buffer.size);
    std::vector<std::string_view> lines;
    std::size_t begin = 0;
    while (begin < patch.size()) {
      const std::size_t end = patch.find('\n', begin);
      lines.push_back(patch.substr(begin, end - begin));
      if (end == std::string_view::npos) break;  // GG_COV_EXCL_BRANCH
      begin = end + 1;
    }
    for (std::size_t index = 0; index < lines.size();) {
      const std::string_view line = lines[index];
      if (color_words && starts_with(line, "-") &&
          !starts_with(line, "--- ")) {
        std::size_t added = index;
        while (added < lines.size() && starts_with(lines[added], "-")) {
          ++added;
        }
        std::size_t end = added;
        while (end < lines.size() && starts_with(lines[end], "+")) {
          ++end;
        }
        if (end != added) {
          const std::size_t removed_count = added - index;
          const std::size_t added_count = end - added;
          for (std::size_t offset = 0; offset < removed_count; ++offset) {
            if (offset < added_count) {
              render_word_line(lines[index + offset], lines[added + offset],
                               OutputStyle::removed, output);
            } else {
              output << styled(output, lines[index + offset],
                               OutputStyle::removed);
            }
            output << '\n';
          }
          for (std::size_t offset = 0; offset < added_count; ++offset) {
            if (offset < removed_count) {
              render_word_line(lines[added + offset], lines[index + offset],
                               OutputStyle::added, output);
            } else {
              output << styled(output, lines[added + offset],
                               OutputStyle::added);
            }
            output << '\n';
          }
          index = end;
          continue;
        }
      }
      if (starts_with(line, "diff --git") || starts_with(line, "index ") ||
          starts_with(line, "--- ") || starts_with(line, "+++ ") ||
          starts_with(line, "new file mode ") ||
          starts_with(line, "deleted file mode ") ||
          starts_with(line, "similarity index ") ||
          starts_with(line, "rename from ") || starts_with(line, "rename to ")) {
        output << styled(output, line, OutputStyle::heading);
      } else if (starts_with(line, "@@")) {
        output << styled(output, line, OutputStyle::hunk);
      } else if (starts_with(line, "+")) {
        output << styled(output, line, OutputStyle::added);
      } else if (starts_with(line, "-")) {
        output << styled(output, line, OutputStyle::removed);
      } else {
        output << line;
      }
      output << '\n';
      ++index;
    }
  }
  git_buf_dispose(&buffer);
}

void render_diff(Repository& repo,
                 const git_oid& from_tree,
                 const git_oid& to_tree,
                 const std::vector<std::string>& paths,
                 git_diff* diff,
                 const DiffFormatOptions& requested,
                 std::ostream& output) {
  DiffFormatOptions format = requested;
  if (starts_with(format.tool, ":")) {
    const std::string builtin = format.tool.substr(1);
    format.tool.clear();
    if (builtin == "summary") {
      format.summary = true;
    } else if (builtin == "stat") {
      format.stat = true;
    } else if (builtin == "types") {
      format.types = true;
    } else if (builtin == "name-only") {
      format.name_only = true;
    } else if (builtin == "git") {
      format.git = true;
    } else if (builtin == "color-words") {
      format.color_words = true;
    } else {
      throw UserError("invalid builtin diff format: " + builtin);
    }
  }
  if (format.summary) render_summary(diff, output);
  if (format.stat) render_stat(diff, output);
  if (format.types) render_types(diff, output);
  if (format.name_only) render_names(diff, output);
  if (!format.tool.empty()) {
    render_external_diff(repo, from_tree, to_tree, paths, format, output);
    return;
  }
  const bool has_short_format =
      format.summary || format.stat || format.types || format.name_only;
  if (format.git || format.color_words || !has_short_format) {
    render_patch(diff, format.color_words, output);
  }
}

std::map<std::string, std::vector<std::string>> selectable_paths(
    Repository& repo,
    const git_oid& left_tree,
    const git_oid& right_tree,
    const std::vector<std::string>& paths) {
  DiffFormatOptions format;
  DiffPtr diff = create_diff(repo, left_tree, right_tree, paths, format);
  std::map<std::string, std::vector<std::string>> result;
  for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
    const std::string old_path = delta->old_file.path;
    const std::string new_path = delta->new_file.path;
    if (old_path == new_path) {
      result.emplace(new_path, std::vector<std::string>{new_path});
    } else {
      result.emplace(old_path + " -> " + new_path,
                     std::vector<std::string>{old_path, new_path});
    }
  }
  return result;
}

void render_revision_header(Repository& repo,
                            const git_oid& revision,
                            std::ostream& output) {
  CommitPtr commit = repo.commit(revision);
  output << "Commit ID: "
         << styled(output, oid_string(revision), OutputStyle::commit_id) << '\n';
  if (const auto id = repo.change_id(revision); id.has_value()) {
    output << "Change ID: " << styled(output, *id, OutputStyle::change_id)
           << '\n';
  }
  const std::vector<std::string> bookmarks = repo.bookmarks(revision);
  if (!bookmarks.empty()) {
    output << "Bookmarks:";
    for (const std::string& bookmark : bookmarks) {
      output << ' ' << styled(output, bookmark, OutputStyle::bookmark);
    }
    output << '\n';
  }
  const std::string description = first_line(git_commit_message(commit.get()));
  output << "Description: "
         << (description.empty() ? "(no description set)" : description)
         << '\n';
}

}  // namespace

bool revision_matches_paths(Repository& repo,
                            const git_oid& revision,
                            const std::vector<std::string>& paths,
                            const DiffFormatOptions& format) {
  DiffPtr diff = create_diff(repo, parent_tree_id(repo, revision),
                             tree_id(repo, revision), paths, format);
  return git_diff_num_deltas(diff.get()) != 0;
}

void render_revision_diff(Repository& repo,
                          const git_oid& revision,
                          const std::vector<std::string>& paths,
                          const DiffFormatOptions& format,
                          std::ostream& output) {
  const git_oid from_tree = parent_tree_id(repo, revision);
  const git_oid to_tree = tree_id(repo, revision);
  DiffPtr diff = create_diff(repo, from_tree, to_tree, paths, format);
  render_diff(repo, from_tree, to_tree, paths, diff.get(), format, output);
}

void render_tree_diff(Repository& repo,
                      const git_oid& from_tree,
                      const git_oid& to_tree,
                      const std::vector<std::string>& paths,
                      const DiffFormatOptions& format,
                      std::ostream& output) {
  DiffPtr diff = create_diff(repo, from_tree, to_tree, paths, format);
  render_diff(repo, from_tree, to_tree, paths, diff.get(), format, output);
}

git_oid select_diff_tree(Repository& repo,
                         const git_oid& left_tree,
                         const git_oid& right_tree,
                         const std::vector<std::string>& paths,
                         std::string_view requested_tool) {
  std::string tool(requested_tool);
  if (tool.empty()) tool = ":builtin";
  if (tool != ":builtin") {
    if (starts_with(tool, ":")) {
      throw UserError("invalid builtin diff editor: " + tool.substr(1));
    }
    return run_external_editor(repo, left_tree, right_tree, paths, tool);
  }

  const auto candidates = selectable_paths(repo, left_tree, right_tree, paths);
  if (candidates.empty()) return right_tree;
  std::ostringstream manifest;
  manifest << "# Keep one changed path per line. Delete a line to exclude it.\n";
  for (const auto& [label, selected_paths] : candidates) {
    (void)selected_paths;
    manifest << label << '\n';
  }
  const std::string edited = edit_text(repo, manifest.str());
  std::istringstream lines(edited);
  std::vector<std::string> selected_paths;
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line.front() == '#') continue;
    const auto candidate = candidates.find(line);
    if (candidate == candidates.end()) {
      throw UserError("unknown path in interactive selection: " + line);
    }
    selected_paths.insert(selected_paths.end(), candidate->second.begin(),
                          candidate->second.end());
  }
  return repo.selected_tree(left_tree, right_tree, selected_paths);
}

void command_diff(Repository& repo,
                  const DiffCommand& options,
                  std::ostream& output) {
  repo.sync_for_command();
  git_oid from_tree{};
  git_oid to_tree{};
  if (!options.from.empty() || !options.to.empty()) {
    from_tree =
        tree_id(repo, repo.resolve(options.from.empty() ? "@" : options.from));
    to_tree =
        tree_id(repo, repo.resolve(options.to.empty() ? "@" : options.to));
  } else {
    std::string_view revisions = "@";
    if (!options.revisions.empty()) revisions = options.revisions;
    const auto trees = revision_set_trees(repo, revisions);
    if (!trees.has_value()) return;
    from_tree = trees->first;
    to_tree = trees->second;
  }
  DiffPtr diff =
      create_diff(repo, from_tree, to_tree, options.paths, options.format);
  render_diff(repo, from_tree, to_tree, options.paths, diff.get(),
              options.format, output);
}

void command_show(Repository& repo,
                  const ShowCommand& options,
                  std::ostream& output) {
  repo.sync_for_command();
  std::vector<std::string> revisions = options.revisions;
  revisions.insert(revisions.end(), options.revision_options.begin(),
                   options.revision_options.end());
  if (revisions.empty()) {
    revisions.emplace_back("@");
  }
  std::vector<git_oid> resolved = resolve_revision_arguments(repo, revisions);
  if (options.reversed) {
    std::reverse(resolved.begin(), resolved.end());
  }
  for (std::size_t index = 0; index < resolved.size(); ++index) {
    const git_oid revision = resolved[index];
    if (index != 0) output << '\n';
    render_revision_header(repo, revision, output);
    if (!options.no_patch) {
      render_revision_diff(repo, revision, {}, options.format, output);
    }
  }
}

}  // namespace gg::detail
