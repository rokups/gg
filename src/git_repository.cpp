// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <array>
#include <cstring>

namespace gg::detail {

void check(int result, std::string_view action) {
if (result >= 0) return;
const git_error* error = git_error_last();
std::string message(action);
if (error != nullptr && error->message != nullptr) {  // GG_COV_EXCL_BRANCH
  message += ": ";
  message += error->message;
}
throw GitError(message);
}

Libgit2::Libgit2() { check(git_libgit2_init(), "initialize libgit2"); }
Libgit2::~Libgit2() { git_libgit2_shutdown(); }

bool OidLess::operator()(const git_oid& left, const git_oid& right) const {
return git_oid_cmp(&left, &right) < 0;
}

bool operator==(const git_oid& left, const git_oid& right) {
return git_oid_equal(&left, &right) != 0;
}

std::string oid_string(const git_oid& oid, std::size_t length) {
std::array<char, GIT_OID_SHA1_HEXSIZE + 1> buffer{};
git_oid_tostr(buffer.data(), buffer.size(), &oid);
return std::string(buffer.data(), std::min(length, std::strlen(buffer.data())));
}

std::string first_line(const char* message) {
const std::string text = message == nullptr ? "" : message;
return text.substr(0, text.find('\n'));
}

bool starts_with(std::string_view value, std::string_view prefix) {
return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}
Repository::Repository(const std::filesystem::path& path,
                       std::vector<std::string> config_values,
                       std::vector<std::filesystem::path> config_files,
                       bool ignore_working_copy)
    : config_values_(std::move(config_values)),
      config_files_(std::move(config_files)),
      ignore_working_copy_(ignore_working_copy) {
  git_repository* repository = nullptr;
  check(git_repository_open_ext(&repository, path.string().c_str(),
                                GIT_REPOSITORY_OPEN_CROSS_FS, nullptr),
        "open Git repository");
  repo_.reset(repository);
  if (git_repository_is_bare(repo_.get()) != 0) {
    throw UserError("this command requires a Git working tree");
  }
}

git_repository* Repository::raw() const { return repo_.get(); }

const std::vector<std::string>& Repository::config_values() const {
  return config_values_;
}

const std::vector<std::filesystem::path>& Repository::config_files() const {
  return config_files_;
}

CommitPtr Repository::commit(const git_oid& oid) const {
  git_commit* value = nullptr;
  check(git_commit_lookup(&value, repo_.get(), &oid), "read commit");
  return CommitPtr(value);
}

TreePtr Repository::tree(const git_oid& oid) const {
  git_tree* value = nullptr;
  check(git_tree_lookup(&value, repo_.get(), &oid), "read tree");
  return TreePtr(value);
}

std::optional<git_oid> Repository::ref_target(std::string_view name) const {
  git_reference* direct = nullptr;
  const int lookup =
      git_reference_lookup(&direct, repo_.get(), std::string(name).c_str());
  if (lookup == GIT_ENOTFOUND) {
    return std::nullopt;
  }
  check(lookup, "read reference");
  ReferencePtr reference(direct);
  if (git_reference_type(reference.get()) == GIT_REFERENCE_SYMBOLIC) {
    git_reference* resolved = nullptr;
    const int resolution = git_reference_resolve(&resolved, reference.get());
    if (resolution == GIT_ENOTFOUND) {
      return std::nullopt;
    }
    check(resolution, "resolve reference");
    reference.reset(resolved);
  }
  return *git_reference_target(reference.get());
}

HeadState Repository::head_state() const {
  git_reference* head = nullptr;
  check(git_reference_lookup(&head, repo_.get(), "HEAD"), "read HEAD");
  ReferencePtr reference(head);
  if (git_reference_type(reference.get()) == GIT_REFERENCE_SYMBOLIC) {
    return {true, git_reference_symbolic_target(reference.get())};
  }
  return {false, oid_string(*git_reference_target(reference.get()))};
}

std::optional<git_oid> Repository::head_oid() const { return ref_target("HEAD"); }

std::map<std::string, git_oid> Repository::data_refs() const {
  std::map<std::string, git_oid> refs;
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_new(&raw_iterator, repo_.get()),
        "list references");
  ReferenceIteratorPtr iterator(raw_iterator);
  while (true) {
    git_reference* raw_reference = nullptr;
    const int result = git_reference_next(&raw_reference, iterator.get());
    if (result == GIT_ITEROVER) {
      break;
    }
    check(result, "list references");
    ReferencePtr reference(raw_reference);
    const std::string name = git_reference_name(reference.get());
    if (starts_with(name, "refs/gg/operations/")) {
      continue;
    }
    git_reference* raw_resolved = nullptr;
    const int resolution = git_reference_resolve(&raw_resolved, reference.get());
    if (resolution == GIT_ENOTFOUND) {
      continue;
    }
    check(resolution, "resolve reference");
    ReferencePtr resolved(raw_resolved);
    if (starts_with(name, "refs/heads/") ||
        starts_with(name, "refs/tags/") ||
        starts_with(name, "refs/remotes/") ||
        starts_with(name, "refs/gg/changes/") ||
        starts_with(name, kRemoteTagPrefix) ||
        starts_with(name, "refs/gg/workspaces/")) {
      refs.emplace(name, *git_reference_target(resolved.get()));
    }
  }
  return refs;
}

std::map<std::string, git_oid> Repository::rewrite_refs() const {
  auto refs = data_refs();
  for (auto iterator = refs.begin(); iterator != refs.end();) {
    if (starts_with(iterator->first, "refs/remotes/") ||
        starts_with(iterator->first, kRemoteTagPrefix)) {
      iterator = refs.erase(iterator);
    } else {
      ++iterator;
    }
  }
  return refs;
}

SignaturePtr Repository::signature() const {
  git_signature* raw_signature = nullptr;
  if (git_signature_default(&raw_signature, repo_.get()) < 0) {
    git_error_clear();
    check(git_signature_now(&raw_signature, "gg", "gg@localhost"),
          "create signature");
  }
  return SignaturePtr(raw_signature);
}

git_oid Repository::create_commit(const git_oid& tree_oid,
                      const std::vector<git_oid>& parent_oids,
                      std::string_view message,
                      const git_signature* author,
                      const git_signature* committer_override) const {
  TreePtr commit_tree = tree(tree_oid);
  std::vector<CommitPtr> parents;
  std::vector<const git_commit*> parent_pointers;
  parents.reserve(parent_oids.size());
  parent_pointers.reserve(parent_oids.size());
  for (const git_oid& parent_oid : parent_oids) {
    parents.push_back(commit(parent_oid));
    parent_pointers.push_back(parents.back().get());
  }
  SignaturePtr committer = signature();
  const git_signature* actual_author = author == nullptr ? committer.get() : author;
  const git_signature* actual_committer =
      committer_override == nullptr ? committer.get() : committer_override;
  git_oid result{};
  const std::string owned_message(message);
  check(git_commit_create(&result, repo_.get(), nullptr, actual_author,
                          actual_committer, nullptr, owned_message.c_str(),
                          commit_tree.get(), parent_pointers.size(),
                          parent_pointers.data()),
        "write commit");
  return result;
}

git_oid Repository::empty_tree() const {
  git_treebuilder* raw_builder = nullptr;
  check(git_treebuilder_new(&raw_builder, repo_.get(), nullptr),
        "create empty tree");
  GitPtr<git_treebuilder, git_treebuilder_free> builder(raw_builder);
  git_oid result{};
  check(git_treebuilder_write(&result, builder.get()), "write empty tree");
  return result;
}
}  // namespace gg::detail
