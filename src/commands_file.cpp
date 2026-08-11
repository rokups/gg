// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <algorithm>
#include <filesystem>
#include <regex>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

struct FileEntry {
  std::string path;
  git_oid oid;
  git_filemode_t mode;
  std::size_t conflict_sides{1};
  bool conflict{false};
};

int collect_entry(const char* root,
                  const git_tree_entry* entry,
                  void* payload) {
  if (git_tree_entry_type(entry) != GIT_OBJECT_TREE) {
    auto& entries = *static_cast<std::vector<FileEntry>*>(payload);
    entries.push_back(
        {std::string(root) + git_tree_entry_name(entry),
         *git_tree_entry_id(entry), git_tree_entry_filemode(entry)});
  }
  return 0;
}

std::vector<FileEntry> tree_entries(Repository& repo, const git_oid& revision) {
  CommitPtr commit = repo.commit(revision);
  TreePtr tree = repo.tree(*git_commit_tree_id(commit.get()));
  std::vector<FileEntry> entries;
  check(git_tree_walk(tree.get(), GIT_TREEWALK_PRE, collect_entry, &entries),
        "walk revision tree");
  const TreeConflicts conflicts =
      repo.tree_conflicts(*git_commit_tree_id(commit.get()));
  for (FileEntry& entry : entries) {
    if (const auto conflict = conflicts.find(entry.path);
        conflict != conflicts.end()) {
      entry.conflict_sides = conflict->second.adds.size();
      entry.conflict = true;
    }
  }
  return entries;
}

std::string normalize_selector(const std::string& value) {
  if (value.empty()) {
    throw UserError("file paths must not be empty");
  }
  const std::filesystem::path path(value);
  if (path.is_absolute()) {
    throw UserError("file paths must be repository-relative");
  }
  std::filesystem::path normalized;
  for (const auto& component : path) {
    if (component == ".") {
      continue;
    }
    if (component == "..") {
      throw UserError("file paths must not contain '..'");
    }
    normalized /= component;
  }
  return normalized.generic_string();
}

std::vector<const FileEntry*> select_entries(
    const std::vector<FileEntry>& entries,
    const std::vector<std::string>& values,
    bool require_matches) {
  std::vector<std::string> selectors;
  selectors.reserve(values.size());
  for (const std::string& value : values) {
    (void)fileset_matches(value, "");
    selectors.push_back(value);
  }
  if (require_matches) {
    for (std::size_t index = 0; index < selectors.size(); ++index) {
      const bool matched = std::ranges::any_of(entries, [&](const auto& entry) {
        return fileset_matches(selectors[index], entry.path);
      });
      if (!matched) {
        throw UserError("path not found: " + values[index]);
      }
    }
  }

  std::vector<const FileEntry*> selected;
  for (const FileEntry& entry : entries) {
    if (selectors.empty() ||
        std::ranges::any_of(selectors, [&](const auto& selector) {
          return fileset_matches(selector, entry.path);
        })) {
      selected.push_back(&entry);
    }
  }
  return selected;
}

bool is_regular_file(git_filemode_t mode) {
  return mode == GIT_FILEMODE_BLOB || mode == GIT_FILEMODE_BLOB_EXECUTABLE;
}

std::string blob_contents(Repository& repo, const FileEntry& entry) {
  git_blob* raw_blob = nullptr;
  check(git_blob_lookup(&raw_blob, repo.raw(), &entry.oid), "read file");
  BlobPtr blob(raw_blob);
  const auto size = static_cast<std::size_t>(git_blob_rawsize(blob.get()));
  if (size == 0) {
    return {};
  }
  return {static_cast<const char*>(git_blob_rawcontent(blob.get())), size};
}

std::string escape_regex(std::string_view pattern, bool glob) {
  std::string result;
  for (const char character : pattern) {
    if (glob && character == '*') {
      result += ".*";
    } else if (glob && character == '?') {
      result += '.';
    } else {
      if (std::string_view(R"(\.^$|()[]{}+*)").find(character) !=
          std::string_view::npos) {
        result += '\\';
      }
      result += character;
    }
  }
  return result;
}

std::regex search_pattern(const std::string& value) {
  std::string kind = "regex";
  std::string pattern = value;
  if (const std::size_t colon = value.find(':'); colon != std::string::npos) {
    kind = value.substr(0, colon);
    pattern = value.substr(colon + 1);
  }
  std::string expression;
  if (kind == "regex") {
    expression = pattern;
  } else if (kind == "exact") {
    expression = '^' + escape_regex(pattern, false) + '$';
  } else if (kind == "substring") {
    expression = escape_regex(pattern, false);
  } else if (kind == "glob") {
    expression = '^' + escape_regex(pattern, true) + '$';
  } else {
    throw UserError("unsupported search pattern kind: " + kind);
  }
  try {
    return std::regex(expression);
  } catch (const std::regex_error&) {  // GG_COV_EXCL_BRANCH
    throw UserError("invalid regular expression: " + pattern);
  }
}

void list_files(const std::vector<const FileEntry*>& entries,
                std::ostream& output) {
  for (const FileEntry* entry : entries) {
    output << entry->path << '\n';
  }
}

void show_files(Repository& repo,
                const std::vector<const FileEntry*>& entries,
                std::ostream& output) {
  for (const FileEntry* entry : entries) {
    if (!is_regular_file(entry->mode)) {
      throw UserError("path is not a regular file: " + entry->path);
    }
    const std::string content = blob_contents(repo, *entry);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
  }
}

void search_files(Repository& repo,
                  const FileCommand& options,
                  const std::vector<const FileEntry*>& entries,
                  std::ostream& output) {
  const std::regex matcher = search_pattern(options.pattern);
  for (const FileEntry* entry : entries) {
    if (!is_regular_file(entry->mode)) {
      continue;
    }
    std::istringstream lines(blob_contents(repo, *entry));
    std::string line;
    std::size_t number = 0;
    while (std::getline(lines, line)) {
      ++number;
      if (!std::regex_search(line, matcher)) {
        continue;
      }
      output << entry->path;
      if (options.name_only) {
        output << '\n';
        break;
      }
      output << ':';
      if (options.line_number) {
        output << number << ':';
      }
      output << line << '\n';
    }
  }
}

void chmod_files(Repository& repo,
                 const FileCommand& options,
                 const git_oid& old,
                 const std::vector<const FileEntry*>& entries,
                 std::ostream& output) {
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  CommitPtr old_commit = repo.commit(old);
  TreePtr old_tree = repo.tree(*git_commit_tree_id(old_commit.get()));
  git_index* raw_index = nullptr;
  git_index_options index_options = GIT_INDEX_OPTIONS_INIT;
  index_options.oid_type = git_repository_oid_type(repo.raw());
  check(git_index_new(&raw_index, &index_options), "create file index");
  IndexPtr index(raw_index);
  check(git_index_read_tree(index.get(), old_tree.get()), "prepare file modes");
  const bool executable = options.mode == "x" || options.mode == "executable";
  const std::uint32_t mode = executable ? GIT_FILEMODE_BLOB_EXECUTABLE
                                        : GIT_FILEMODE_BLOB;
  bool changed = false;
  for (const FileEntry* entry : entries) {
    if (!is_regular_file(entry->mode)) {
      throw UserError("path is not a regular file: " + entry->path);
    }
    if (entry->mode == mode) {
      continue;
    }
    const git_index_entry* original =
        git_index_get_bypath(index.get(), entry->path.c_str(), 0);
    git_index_entry updated = *original;
    updated.mode = mode;
    check(git_index_add(index.get(), &updated), "update executable bit");
    changed = true;
  }
  if (!changed) {
    output << "Nothing changed.\n";
    return;
  }
  git_oid tree_oid{};
  check(git_index_write_tree_to(&tree_oid, index.get(), repo.raw()),
        "write file modes");
  repo.preserve_conflicts(*git_commit_tree_id(old_commit.get()), tree_oid);
  const git_oid rewritten =
      repo.rewrite_commit(old, repo.parents(old), tree_oid);
  RewritePlan plan = repo.descendants({{old, rewritten}});
  const git_oid new_workspace = plan.commits.contains(*workspace)
                                    ? plan.commits.at(*workspace)
                                    : *workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {},
                   "gg file chmod");
  output << "Updated file modes in " << repo.short_commit_id(rewritten).value
         << ".\n";
}

}  // namespace

void command_file(Repository& repo,
                  const FileCommand& options,
                  std::ostream& output) {
  repo.sync_for_command();
  std::vector<std::string> normalized_paths;
  normalized_paths.reserve(options.paths.size());
  for (const std::string& path : options.paths) {
    normalized_paths.push_back(normalize_selector(path));
  }
  if (options.action == FileAction::track) {
    if (!repo.workspace().has_value()) {
      throw UserError("this command requires a working-copy change");
    }
    repo.track_paths(normalized_paths, options.include_ignored);
    repo.sync_for_command();
    output << "Started tracking " << normalized_paths.size() << " path(s).\n";
    return;
  }
  const git_oid revision = repo.resolve(options.revision);
  const std::vector<FileEntry> entries = tree_entries(repo, revision);
  const bool require_matches =
      options.action == FileAction::show || options.action == FileAction::chmod ||
      options.action == FileAction::untrack;
  const std::vector<const FileEntry*> selected =
      select_entries(entries, options.paths, require_matches);
  if (options.action == FileAction::untrack) {
    repo.untrack_paths(normalized_paths);
    repo.sync_for_command();
    output << "Stopped tracking " << normalized_paths.size() << " path(s).\n";
    return;
  }
  switch (options.action) {
    case FileAction::list:
      list_files(selected, output);
      return;
    case FileAction::show:
      show_files(repo, selected, output);
      return;
    case FileAction::search:
      search_files(repo, options, selected, output);
      return;
    case FileAction::chmod:
    case FileAction::track:    // Handled above.
    case FileAction::untrack:  // Handled above.
    default:  // GG_COV_EXCL_BRANCH
      chmod_files(repo, options, revision, selected, output);
      return;
  }
}

}  // namespace gg::detail
