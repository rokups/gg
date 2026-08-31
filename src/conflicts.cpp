// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <iomanip>
#include <sstream>

namespace gg::detail {
namespace {

bool is_conflict_marker(std::string_view line) {
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  if (line.size() < 7) return false;
  const char marker = line.front();
  const std::size_t marker_end = line.find_first_not_of(marker);
  const std::size_t marker_length =
      marker_end == std::string_view::npos ? line.size() : marker_end;
  if (marker_length < 7) return false;
  const std::string_view label = line.substr(marker_length);
  switch (marker) {
    case '<':
    case '>':
    case '|':
      return label.empty() || label.starts_with(' ');
    case '=':
      return label.empty();
    case '+':
      return label.starts_with(" Side #");
    case '-':
      return label.starts_with(" Base #");
    default:
      return false;
  }
}

bool has_conflict_marker(git_repository* repository,
                         const git_tree_entry* entry) {
  if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) return false;
  git_blob* raw_blob = nullptr;
  check(git_blob_lookup(&raw_blob, repository, git_tree_entry_id(entry)),
        "read conflict marker");
  BlobPtr blob(raw_blob);
  if (git_blob_rawsize(blob.get()) == 0) return false;
  std::string_view contents(
      static_cast<const char*>(git_blob_rawcontent(blob.get())),
      static_cast<std::size_t>(git_blob_rawsize(blob.get())));
  while (!contents.empty()) {
    const std::size_t line_end = contents.find('\n');
    const std::string_view line = contents.substr(0, line_end);
    if (is_conflict_marker(line)) return true;
    if (line_end == std::string_view::npos) break;
    contents.remove_prefix(line_end + 1);
  }
  return false;
}

}  // namespace

bool Repository::has_legacy_rewrite() const {
  return ref_target(rewrite_ref_name()).has_value();
}

TreeConflicts Repository::tree_conflicts(const git_oid& tree_oid) const {
  if (const auto cached = conflict_cache_.find(tree_oid);
      cached != conflict_cache_.end()) {
    return cached->second;
  }
  const std::string reference =
      std::string(kConflictPrefix) + oid_string(tree_oid);
  git_reference* raw_reference = nullptr;
  const int lookup =
      git_reference_lookup(&raw_reference, repo_.get(), reference.c_str());
  if (lookup == GIT_ENOTFOUND) git_error_clear();  // GG_COV_EXCL_BRANCH
  if (lookup != 0 && lookup != GIT_ENOTFOUND) check(lookup, "read conflict metadata");  // GG_COV_EXCL_BRANCH
  ReferencePtr metadata_reference(raw_reference);
  TreeConflicts result;
  if (metadata_reference == nullptr) {  // GG_COV_EXCL_BRANCH
    conflict_cache_.emplace(tree_oid, result);
    return result;
  }
  if (git_reference_type(metadata_reference.get()) != GIT_REFERENCE_DIRECT) throw GitError("invalid conflict metadata reference");  // GG_COV_EXCL_BRANCH
  CommitPtr holder = commit(*git_reference_target(metadata_reference.get()));
  std::istringstream input(git_commit_message(holder.get()));
  std::string line;
  if (!std::getline(input, line) || line != "gg-conflicts-v1") {  // GG_COV_EXCL_BRANCH
    throw GitError("invalid conflict metadata");
  }
  std::string path;
  while (input >> std::quoted(path)) {
    ConflictValue conflict;
    char kind = '\0';
    std::string oid_text;
    unsigned int mode = 0;
    while (input >> kind && kind != 'E') {  // GG_COV_EXCL_BRANCH
      if (!(input >> oid_text >> mode) ||  // GG_COV_EXCL_BRANCH
          (kind != 'R' && kind != 'A')) {
        throw GitError("invalid conflict metadata entry");
      }
      FileValue value;
      value.present = oid_text != "-";
      value.mode = static_cast<git_filemode_t>(mode);
      if (value.present) {  // GG_COV_EXCL_BRANCH
        check(git_oid_fromstr(&value.oid, oid_text.c_str(),
                              git_repository_oid_type(repo_.get())),
              "parse conflict object");
      }
      (kind == 'R' ? conflict.removes : conflict.adds).push_back(value);
    }
    if (kind != 'E' || conflict.adds.empty()) {  // GG_COV_EXCL_BRANCH
      throw GitError("invalid conflict metadata entry");
    }
    result.emplace(path, std::move(conflict));
  }
  conflict_cache_[tree_oid] = result;
  return result;
}

void Repository::record_conflicts(const git_oid& tree_oid,
                                  TreeConflicts conflicts) const {
  if (conflicts.empty()) return;
  std::ostringstream output;
  output << "gg-conflicts-v1\n";
  for (const auto& [path, conflict] : conflicts) {
    output << std::quoted(path) << '\n';
    const auto write = [&](char kind, const FileValue& value) {
      output << kind << ' '
             << (value.present ? oid_string(value.oid) : "-") << ' '
             << static_cast<unsigned int>(value.mode) << '\n';
    };
    for (const FileValue& value : conflict.removes) write('R', value);
    for (const FileValue& value : conflict.adds) write('A', value);
    output << "E\n";
  }
  const git_oid holder = create_commit(tree_oid, {}, output.str());
  const std::string reference =
      std::string(kConflictPrefix) + oid_string(tree_oid);
  conflict_cache_[tree_oid] = std::move(conflicts);
  pending_conflict_refs_[reference] = holder;
}

bool Repository::tree_has_conflicts(const git_oid& tree_oid) const {
  return !tree_conflicts(tree_oid).empty();
}

bool Repository::commit_has_conflicts(const git_oid& commit_oid) const {
  CommitPtr value = commit(commit_oid);
  return tree_has_conflicts(*git_commit_tree_id(value.get()));
}

bool Repository::history_has_conflicts(const git_oid& commit_oid) const {
  std::set<git_oid, OidLess> seen;
  std::vector<git_oid> pending{commit_oid};
  while (!pending.empty()) {
    const git_oid oid = pending.back();
    pending.pop_back();
    if (!seen.insert(oid).second) continue;  // GG_COV_EXCL_BRANCH
    if (commit_has_conflicts(oid)) return true;
    const std::vector<git_oid> commit_parents = parents(oid);
    pending.insert(pending.end(), commit_parents.begin(), commit_parents.end());
  }
  return false;
}

std::vector<std::string> Repository::conflict_paths(
    const git_oid& commit_oid) const {
  CommitPtr value = commit(commit_oid);
  const TreeConflicts conflicts =
      tree_conflicts(*git_commit_tree_id(value.get()));
  std::vector<std::string> result;
  result.reserve(conflicts.size());
  for (const auto& [path, conflict] : conflicts) {
    (void)conflict;
    result.push_back(path);
  }
  return result;
}

void Repository::preserve_conflicts(const git_oid& old_tree,
                                    const git_oid& new_tree) const {
  const TreeConflicts old = tree_conflicts(old_tree);
  if (old.empty()) return;
  TreePtr before = tree(old_tree);
  TreePtr after = tree(new_tree);
  TreeConflicts retained;
  for (const auto& [path, conflict] : old) {
    git_tree_entry* raw_before = nullptr;
    git_tree_entry* raw_after = nullptr;
    const int before_result =
        git_tree_entry_bypath(&raw_before, before.get(), path.c_str());
    const int after_result =
        git_tree_entry_bypath(&raw_after, after.get(), path.c_str());
    TreeEntryPtr before_entry(raw_before);
    TreeEntryPtr after_entry(raw_after);
    const bool same = before_result == after_result &&  // GG_COV_EXCL_BRANCH
                      (before_result == GIT_ENOTFOUND ||  // GG_COV_EXCL_BRANCH
                       (before_result == 0 &&
                        git_tree_entry_filemode(before_entry.get()) ==
                            git_tree_entry_filemode(after_entry.get()) &&  // GG_COV_EXCL_BRANCH
                        *git_tree_entry_id(before_entry.get()) ==
                            *git_tree_entry_id(after_entry.get())));
    if (before_result != 0 && before_result != GIT_ENOTFOUND) check(before_result, "read conflict materialization");  // GG_COV_EXCL_BRANCH
    if (after_result != 0 && after_result != GIT_ENOTFOUND) check(after_result, "read conflict snapshot");  // GG_COV_EXCL_BRANCH
    const bool marked = !same && after_result == 0 &&
                        has_conflict_marker(repo_.get(), after_entry.get());
    if (same || marked) retained.emplace(path, conflict);
  }
  record_conflicts(new_tree, std::move(retained));
}


}  // namespace gg::detail
