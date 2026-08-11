// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

bool same_file(const FileValue& left, const FileValue& right) {
  return left.present == right.present &&  // GG_COV_EXCL_BRANCH
         (!left.present || (left.oid == right.oid &&  // GG_COV_EXCL_BRANCH
                             left.mode == right.mode));  // GG_COV_EXCL_BRANCH
}

FileValue file_value(git_tree* tree, const std::string& path) {
  git_tree_entry* raw = nullptr;
  const int result = git_tree_entry_bypath(&raw, tree, path.c_str());
  if (result == GIT_ENOTFOUND) { git_error_clear(); return {}; }  // GG_COV_EXCL_BRANCH
  check(result, "read merged file");
  TreeEntryPtr entry(raw);
  return {true, *git_tree_entry_id(entry.get()),
          git_tree_entry_filemode(entry.get())};
}

FileValue index_value(const git_index_entry* entry) {
  return entry == nullptr
             ? FileValue{}
             : FileValue{true, entry->id,
                         static_cast<git_filemode_t>(entry->mode)};
}

ConflictValue resolved(FileValue value) {
  return {{}, {value}};
}

void simplify(ConflictValue& conflict) {
  for (auto remove = conflict.removes.begin();
       remove != conflict.removes.end();) {
    const auto add = std::ranges::find_if(
        conflict.adds,
        [&](const FileValue& value) { return same_file(*remove, value); });
    if (add == conflict.adds.end()) {
      ++remove;
    } else {
      conflict.adds.erase(add);
      remove = conflict.removes.erase(remove);
    }
  }
}

std::optional<FileValue> automerge(const Repository& repo,
                                   const ConflictValue& conflict) {
  if (conflict.removes.size() != 1 || conflict.adds.size() != 2) return std::nullopt;  // GG_COV_EXCL_BRANCH
  const auto supported = [](const FileValue& value) {
    return !value.present || value.mode == GIT_FILEMODE_BLOB ||  // GG_COV_EXCL_BRANCH
           value.mode == GIT_FILEMODE_BLOB_EXECUTABLE;
  };
  if (!supported(conflict.removes.front()) || !supported(conflict.adds[0]) || !supported(conflict.adds[1])) return std::nullopt;  // GG_COV_EXCL_BRANCH
  const std::array<const FileValue*, 3> values{
      &conflict.removes.front(), &conflict.adds[0], &conflict.adds[1]};
  std::array<BlobPtr, 3> blobs;
  std::array<git_merge_file_input, 3> inputs{};
  std::array<const git_merge_file_input*, 3> pointers{};
  for (std::size_t index = 0; index < values.size(); ++index) {
    inputs[index].version = GIT_MERGE_FILE_INPUT_VERSION;
    if (!values[index]->present) continue;  // GG_COV_EXCL_BRANCH
    git_blob* raw = nullptr;
    check(git_blob_lookup(&raw, repo.raw(), &values[index]->oid),
          "read automatic conflict side");
    blobs[index].reset(raw);
    inputs[index].ptr =
        static_cast<const char*>(git_blob_rawcontent(blobs[index].get()));
    inputs[index].size = git_blob_rawsize(blobs[index].get());
    inputs[index].path = "conflict";
    inputs[index].mode = values[index]->mode;
    pointers[index] = &inputs[index];
  }
  git_merge_file_result result{};
  check(git_merge_file(&result, pointers[0], pointers[1], pointers[2], nullptr),
        "merge conflicted file");
  if (result.automergeable == 0) {  // GG_COV_EXCL_BRANCH
    git_merge_file_result_free(&result);
    return std::nullopt;
  }
  FileValue value;
  if (result.path != nullptr) {  // GG_COV_EXCL_BRANCH
    value.present = true;
    value.mode = static_cast<git_filemode_t>(result.mode);
    check(git_blob_create_from_buffer(&value.oid, repo.raw(), result.ptr,
                                      result.len),
          "write merged conflict resolution");
  }
  git_merge_file_result_free(&result);
  return value;
}

std::string value_text(const Repository& repo, const FileValue& value) {
  if (!value.present) return "(absent)\n";  // GG_COV_EXCL_BRANCH
  if (value.mode == GIT_FILEMODE_COMMIT) {
    return "(git-submodule " + oid_string(value.oid) + ")\n";
  }
  if (value.mode == GIT_FILEMODE_TREE) return "(tree " + oid_string(value.oid) + ")\n";  // GG_COV_EXCL_BRANCH
  git_blob* raw = nullptr;
  check(git_blob_lookup(&raw, repo.raw(), &value.oid), "read conflict side");
  BlobPtr blob(raw);
  const auto size = static_cast<std::size_t>(git_blob_rawsize(blob.get()));
  const char* data = static_cast<const char*>(git_blob_rawcontent(blob.get()));
  std::string result(data, size);
  if (result.empty() || result.back() != '\n') result += '\n';  // GG_COV_EXCL_BRANCH
  return result;
}

FileValue materialize(const Repository& repo, const ConflictValue& conflict) {
  std::vector<std::string> sides;
  sides.reserve(conflict.adds.size() + conflict.removes.size());
  for (const FileValue& value : conflict.adds) {
    sides.push_back(value_text(repo, value));
  }
  for (const FileValue& value : conflict.removes) {
    sides.push_back(value_text(repo, value));
  }
  std::size_t marker_length = 7;
  const auto collides = [&](std::size_t length) {
    return std::ranges::any_of(sides, [&](const std::string& side) {
      return side.find(std::string(length, '<')) != std::string::npos ||  // GG_COV_EXCL_BRANCH
             side.find(std::string(length, '>')) != std::string::npos ||  // GG_COV_EXCL_BRANCH
             side.find(std::string(length, '+')) != std::string::npos ||  // GG_COV_EXCL_BRANCH
             side.find(std::string(length, '-')) != std::string::npos;  // GG_COV_EXCL_BRANCH
    });
  };
  while (collides(marker_length)) marker_length += 8;  // GG_COV_EXCL_BRANCH
  const auto marker = [&](char character) {
    return std::string(marker_length, character);
  };
  std::string content = marker('<') + " Conflict\n";
  std::size_t side_index = 0;
  const auto identity = [](const FileValue& value) {
    return value.present
               ? oid_string(value.oid) + " mode " +
                     std::to_string(static_cast<unsigned int>(value.mode))
               : std::string("absent");
  };
  for (std::size_t index = 0; index < conflict.adds.size(); ++index) {
    content += marker('+') + " Side #" + std::to_string(index + 1) + " (" +
               identity(conflict.adds[index]) + ")\n";
    content += sides[side_index++];
  }
  for (std::size_t index = 0; index < conflict.removes.size(); ++index) {
    content += marker('-') + " Base #" + std::to_string(index + 1) + " (" +
               identity(conflict.removes[index]) + ")\n";
    content += sides[side_index++];
  }
  content += marker('>') + " Conflict ends\n";
  git_oid oid{};
  check(git_blob_create_from_buffer(&oid, repo.raw(), content.data(),
                                    content.size()),
        "write conflict markers");
  git_filemode_t mode = GIT_FILEMODE_BLOB;
  const auto executable = std::ranges::find_if(
      conflict.adds, [](const FileValue& value) {
        return value.present &&  // GG_COV_EXCL_BRANCH
               value.mode == GIT_FILEMODE_BLOB_EXECUTABLE;  // GG_COV_EXCL_BRANCH
      });
  if (executable != conflict.adds.end()) mode = GIT_FILEMODE_BLOB_EXECUTABLE;  // GG_COV_EXCL_BRANCH
  return {true, oid, mode};
}

void set_index_value(git_index* index, const std::string& path,
                     const FileValue& value) {
  (void)git_index_conflict_remove(index, path.c_str());
  (void)git_index_remove_bypath(index, path.c_str());
  if (!value.present) return;  // GG_COV_EXCL_BRANCH
  git_index_entry entry{};
  entry.mode = value.mode;
  entry.id = value.oid;
  entry.path = path.c_str();
  check(git_index_add(index, &entry), "materialize conflict");
}

}  // namespace

git_oid Repository::merge_trees(const git_oid& ancestor_oid,
                    const git_oid& ours_oid,
                    const git_oid& theirs_oid) const {
  TreePtr ancestor = tree(ancestor_oid);
  TreePtr ours = tree(ours_oid);
  TreePtr theirs = tree(theirs_oid);
  git_merge_options options = GIT_MERGE_OPTIONS_INIT;
  options.flags = GIT_MERGE_FIND_RENAMES;
  git_index* raw_index = nullptr;
  check(git_merge_trees(&raw_index, repo_.get(), ancestor.get(), ours.get(),
                        theirs.get(), &options),
        "merge trees");
  IndexPtr index(raw_index);
  TreeConflicts result_conflicts;
  if (git_index_has_conflicts(index.get()) != 0) {
    git_index_conflict_iterator* raw_iterator = nullptr;
    check(git_index_conflict_iterator_new(&raw_iterator, index.get()),
          "inspect rewrite conflicts");
    ConflictIteratorPtr iterator(raw_iterator);
    const git_index_entry* base = nullptr;
    const git_index_entry* ours = nullptr;
    const git_index_entry* theirs = nullptr;
    while (git_index_conflict_next(&base, &ours, &theirs, iterator.get()) == 0) {
      const git_index_entry* entry = ours != nullptr ? ours :  // GG_COV_EXCL_BRANCH
                                     theirs != nullptr ? theirs : base;  // GG_COV_EXCL_BRANCH
      result_conflicts[entry->path] =
          {{index_value(base)}, {index_value(ours), index_value(theirs)}};
    }
  }

  const TreeConflicts ancestor_conflicts = tree_conflicts(ancestor_oid);
  const TreeConflicts ours_conflicts = tree_conflicts(ours_oid);
  const TreeConflicts theirs_conflicts = tree_conflicts(theirs_oid);
  std::set<std::string> logical_paths;
  for (const auto& [path, value] : ancestor_conflicts) {
    (void)value;
    logical_paths.insert(path);
  }
  for (const auto& [path, value] : ours_conflicts) {
    (void)value;
    logical_paths.insert(path);
  }
  for (const auto& [path, value] : theirs_conflicts) {
    (void)value;
    logical_paths.insert(path);
  }
  const auto value_at = [&](const TreeConflicts& conflicts, git_tree* source,
                            const std::string& path) {
    const auto found = conflicts.find(path);
    return found == conflicts.end() ? resolved(file_value(source, path))
                                    : found->second;
  };
  for (const std::string& path : logical_paths) {
    const ConflictValue base = value_at(ancestor_conflicts, ancestor.get(), path);
    const ConflictValue left = value_at(ours_conflicts, ours.get(), path);
    const ConflictValue right = value_at(theirs_conflicts, theirs.get(), path);
    ConflictValue combined;
    combined.removes = left.removes;
    combined.removes.insert(combined.removes.end(), right.removes.begin(),
                            right.removes.end());
    combined.removes.insert(combined.removes.end(), base.adds.begin(),
                            base.adds.end());
    combined.adds = left.adds;
    combined.adds.insert(combined.adds.end(), right.adds.begin(),
                         right.adds.end());
    combined.adds.insert(combined.adds.end(), base.removes.begin(),
                         base.removes.end());
    simplify(combined);
    const std::optional<FileValue> merged =
        combined.removes.empty() && combined.adds.size() == 1  // GG_COV_EXCL_BRANCH
            ? std::optional<FileValue>{combined.adds.front()}  // GG_COV_EXCL_BRANCH
            : automerge(*this, combined);  // GG_COV_EXCL_BRANCH
    if (merged.has_value()) {  // GG_COV_EXCL_BRANCH
      result_conflicts.erase(path);
      set_index_value(index.get(), path, *merged);
    } else {
      result_conflicts[path] = std::move(combined);
    }
  }
  for (auto& [path, conflict] : result_conflicts) {
    simplify(conflict);
    set_index_value(index.get(), path, materialize(*this, conflict));
  }
  git_oid result{};
  check(git_index_write_tree_to(&result, index.get(), repo_.get()),
        "write merged tree");
  record_conflicts(result, std::move(result_conflicts));
  return result;
}

git_oid Repository::replay(const git_oid& old_parent,
               const git_oid& new_parent,
               const git_oid& old_tree) const {
  CommitPtr old_parent_commit = commit(old_parent);
  CommitPtr new_parent_commit = commit(new_parent);
  return merge_trees(*git_commit_tree_id(old_parent_commit.get()),
                     *git_commit_tree_id(new_parent_commit.get()), old_tree);
}

git_oid Repository::rewrite_commit(const git_oid& old_oid,
                       const std::vector<git_oid>& new_parents,
                       std::optional<git_oid> tree_override,
                       std::optional<std::string_view> message_override,
                       const git_signature* author_override,
                       const git_signature* committer_override) const {
  CommitPtr old = commit(old_oid);
  git_oid new_tree = tree_override.value_or(*git_commit_tree_id(old.get()));
  if (!tree_override.has_value() && git_commit_parentcount(old.get()) > 0 &&
      !new_parents.empty()) {
    const git_oid& old_parent = *git_commit_parent_id(old.get(), 0);
    if (!(old_parent == new_parents.front())) {
      new_tree = replay(old_parent, new_parents.front(), new_tree);
    }
  }
  const char* old_message = git_commit_message(old.get());
  const std::string_view original =
      old_message == nullptr ? "" : old_message;  // GG_COV_EXCL_BRANCH
  const std::string_view message = message_override.value_or(original);
  return create_commit(
      new_tree, new_parents, message,
      author_override == nullptr ? git_commit_author(old.get()) : author_override,
      committer_override);
}

std::vector<git_oid> Repository::parents(const git_oid& oid) const {
  CommitPtr value = commit(oid);
  std::vector<git_oid> result;
  for (unsigned int index = 0; index < git_commit_parentcount(value.get()); ++index) {
    result.push_back(*git_commit_parent_id(value.get(), index));
  }
  return result;
}

RewritePlan Repository::descendants(
    std::map<git_oid, git_oid, OidLess> roots,
    const std::set<git_oid, OidLess>& skipped,
    bool preserve_content) const {
  RewritePlan plan;
  plan.commits = std::move(roots);
  const auto refs = rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {
      check(pushed, "walk revisions");
    }
  }

  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    if (plan.commits.contains(oid) || skipped.contains(oid)) {  // GG_COV_EXCL_BRANCH
      continue;
    }
    const auto old_parents = parents(oid);
    bool changed = false;
    std::vector<git_oid> new_parents;
    new_parents.reserve(old_parents.size());
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      changed = changed || replacement != plan.commits.end();  // GG_COV_EXCL_BRANCH
      new_parents.push_back(replacement == plan.commits.end()
                                ? parent
                                : replacement->second);
    }
    if (changed) {
      std::optional<git_oid> tree_override;
      if (preserve_content) {
        tree_override = *git_commit_tree_id(commit(oid).get());
      }
      plan.commits.emplace(
          oid, rewrite_commit(oid, new_parents, tree_override));
    }
  }

  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  add_alias_updates(plan);
  return plan;
}

RewritePlan Repository::move_files(const git_oid& source,
                                   const git_oid& destination,
                                   const std::vector<std::string>& paths) const {
  const std::vector<git_oid> source_parents = parents(source);
  const git_oid source_tree = *git_commit_tree_id(commit(source).get());
  const git_oid base_tree = *git_commit_tree_id(commit(source_parents.front()).get());
  const git_oid selected_change = selected_tree(base_tree, source_tree, paths);
  if (selected_change == base_tree) {
    throw UserError("selected paths are not changed in the source revision");
  }

  RewritePlan plan;
  const auto refs = rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) check(pushed, "walk revisions");
  }

  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    const std::vector<git_oid> old_parents = parents(oid);
    std::vector<git_oid> new_parents;
    new_parents.reserve(old_parents.size());
    bool parent_changed = false;
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      parent_changed = parent_changed || replacement != plan.commits.end();
      new_parents.push_back(replacement == plan.commits.end()
                                ? parent
                                : replacement->second);
    }
    if (!parent_changed && !(oid == source) && !(oid == destination)) continue;

    git_oid tree = *git_commit_tree_id(commit(oid).get());
    if (parent_changed && !old_parents.empty() && !new_parents.empty()) {
      tree = replay(old_parents.front(), new_parents.front(), tree);
    }
    if (oid == source) {
      const git_oid parent_tree =
          *git_commit_tree_id(commit(new_parents.front()).get());
      tree = selected_tree(tree, parent_tree, paths);
    }
    if (oid == destination) {
      tree = merge_trees(base_tree, tree, selected_change);
    }
    plan.commits.emplace(oid, rewrite_commit(oid, new_parents, tree));
  }

  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  add_alias_updates(plan);
  return plan;
}

}  // namespace gg::detail
