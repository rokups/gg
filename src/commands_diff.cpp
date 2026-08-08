// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace gg::detail {
namespace {

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

std::vector<std::string> diff_paths(const std::vector<std::string>& values) {
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const std::string& value : values) {
    if (value.empty()) {
      throw UserError("diff paths must not be empty");
    }
    std::string pattern = value;
    if (starts_with(pattern, "file:") || starts_with(pattern, "glob:")) {
      pattern = pattern.substr(pattern.find(':') + 1);
    }
    const std::filesystem::path path(pattern);
    if (path.is_absolute()) {
      throw UserError("diff paths must be repository-relative");
    }
    for (const auto& component : path) {
      if (component == "..") {
        throw UserError("diff paths must not contain '..'");
      }
    }
    result.push_back(path == "." ? "*" : path.generic_string());
  }
  return result;
}

DiffPtr create_diff(Repository& repo,
                    const git_oid& from_tree_id,
                    const git_oid& to_tree_id,
                    const std::vector<std::string>& path_values,
                    const DiffFormatOptions& format) {
  TreePtr from_tree = repo.tree(from_tree_id);
  TreePtr to_tree = repo.tree(to_tree_id);
  const std::vector<std::string> paths = diff_paths(path_values);
  std::vector<char*> path_pointers;
  path_pointers.reserve(paths.size());
  for (const std::string& path : paths) {
    path_pointers.push_back(const_cast<char*>(path.c_str()));
  }
  git_diff_options options = GIT_DIFF_OPTIONS_INIT;
  options.context_lines = format.context;
  options.pathspec = {path_pointers.data(), path_pointers.size()};
  options.flags = GIT_DIFF_INCLUDE_TYPECHANGE;
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
  check(git_diff_find_similar(diff.get(), &find_options), "find renamed files");
  return diff;
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

void render_diff(git_diff* diff,
                 const DiffFormatOptions& format,
                 std::ostream& output) {
  if (!format.tool.empty()) {
    throw UserError("external diff tools are not supported yet");
  }
  if (format.summary) render_summary(diff, output);
  if (format.stat) render_stat(diff, output);
  if (format.types) render_types(diff, output);
  if (format.name_only) render_names(diff, output);
  const bool has_short_format =
      format.summary || format.stat || format.types || format.name_only;
  if (format.git || format.color_words || !has_short_format) {
    render_patch(diff, format.color_words, output);
  }
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
  DiffPtr diff = create_diff(repo, parent_tree_id(repo, revision),
                             tree_id(repo, revision), paths, format);
  render_diff(diff.get(), format, output);
}

void command_diff(Repository& repo,
                  const DiffCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  if (!options.template_value.empty()) {
    throw UserError("diff templates are not supported yet");
  }
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
  render_diff(diff.get(), options.format, output);
}

void command_show(Repository& repo,
                  const ShowCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
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
    if (options.template_value.empty()) {
      if (index != 0) output << '\n';
      render_revision_header(repo, revision, output);
    } else {
      output << render_template(options.template_value,
                                revision_template_values(repo, revision));
    }
    if (!options.no_patch) {
      render_revision_diff(repo, revision, {}, options.format, output);
    }
  }
}

}  // namespace gg::detail
