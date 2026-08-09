// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "gg/gg.h"

#include "commands.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct gg_repository {
  explicit gg_repository(git_repository* raw) : implementation(raw) {}
  gg::detail::Repository implementation;
};

namespace {

using gg::detail::AbandonCommand;
using gg::detail::BookmarkAction;
using gg::detail::BookmarkCommand;
using gg::detail::CommitCommand;
using gg::detail::DescribeCommand;
using gg::detail::EditCommand;
using gg::detail::FileAction;
using gg::detail::FileCommand;
using gg::detail::MetaeditCommand;
using gg::detail::MovementCommand;
using gg::detail::MovementDirection;
using gg::detail::NewCommand;
using gg::detail::OperationRestoreCommand;
using gg::detail::RebaseCommand;
using gg::detail::Repository;
using gg::detail::RestoreCommand;
using gg::detail::SimplifyParentsCommand;
using gg::detail::SparseAction;
using gg::detail::SparseCommand;
using gg::detail::SplitCommand;
using gg::detail::SquashCommand;
using gg::detail::TagAction;
using gg::detail::TagCommand;
using gg::detail::WorkspaceAction;
using gg::detail::WorkspaceCommand;

char* duplicate(const std::string& value) {
  char* result = static_cast<char*>(std::malloc(value.size() + 1));
  if (result == nullptr) throw std::bad_alloc();
  std::memcpy(result, value.c_str(), value.size() + 1);
  return result;
}

std::string string(const char* value) {
  return value == nullptr ? std::string{} : std::string(value);
}

std::vector<std::string> strings(gg_string_array values) {
  if (values.count != 0 && values.strings == nullptr) {
    throw gg::detail::UserError("string array must not be null");
  }
  std::vector<std::string> result;
  result.reserve(values.count);
  for (size_t index = 0; index < values.count; ++index) {
    if (values.strings[index] == nullptr) {
      throw gg::detail::UserError("string array item must not be null");
    }
    result.emplace_back(values.strings[index]);
  }
  return result;
}

void assign_strings(gg_owned_string_array* out,
                    const std::vector<std::string>& values) {
  if (values.empty()) return;
  out->strings =
      static_cast<char**>(std::calloc(values.size(), sizeof(char*)));
  if (out->strings == nullptr) throw std::bad_alloc();
  for (const std::string& value : values) {
    out->strings[out->count++] = duplicate(value);
  }
}

std::vector<std::string> matching_names(
    gg_string_array patterns,
    const std::vector<std::string>& available,
    std::string_view kind,
    bool all_when_empty) {
  const std::vector<std::string> selected_patterns = strings(patterns);
  if (selected_patterns.empty()) return all_when_empty ? available
                                                       : std::vector<std::string>{};
  std::set<std::string> result;
  for (const std::string& pattern : selected_patterns) {
    bool matched = false;
    for (const std::string& value : available) {
      if (!gg::detail::string_pattern_matches(pattern, value)) continue;
      matched = true;
      result.insert(value);
    }
    if (!matched) {
      throw gg::detail::UserError(std::string(kind) + " not found: " + pattern,
                                  GIT_ENOTFOUND);
    }
  }
  return {result.begin(), result.end()};
}

git_oid commit_target(Repository& repository, std::string_view reference) {
  git_object* raw_object = nullptr;
  gg::detail::check(git_revparse_single(&raw_object, repository.raw(),
                                        std::string(reference).c_str()),
                    "resolve reference target");
  gg::detail::ObjectPtr object(raw_object);
  git_object* raw_commit = nullptr;
  gg::detail::check(git_object_peel(&raw_commit, object.get(),
                                    GIT_OBJECT_COMMIT),
                    "resolve commit target");
  gg::detail::ObjectPtr commit(raw_commit);
  return *git_object_id(commit.get());
}

void add_refspec(gg_transport_plan* plan,
                 const std::string& remote,
                 const std::string& source,
                 const std::string& destination,
                 const std::optional<git_oid>& target) {
  void* storage = std::realloc(
      plan->refspecs, (plan->refspec_count + 1) * sizeof(gg_refspec));
  if (storage == nullptr) throw std::bad_alloc();
  plan->refspecs = static_cast<gg_refspec*>(storage);
  gg_refspec& refspec = plan->refspecs[plan->refspec_count++];
  refspec = {};
  refspec.remote = duplicate(remote);
  refspec.source = duplicate(source);
  refspec.destination = duplicate(destination);
  if (target.has_value()) {
    refspec.target = *target;
    refspec.has_target = 1;
  }
}

Repository& query_repository(
    gg_repository* repository,
    const char* at_operation,
    std::unique_ptr<Repository>& historical) {
  if (repository == nullptr) {
    throw gg::detail::UserError("repository must not be null");
  }
  if (at_operation == nullptr || *at_operation == '\0') {
    return repository->implementation;
  }
  historical = std::make_unique<Repository>(repository->implementation.raw(),
                                             true, false);
  historical->view_at_operation(at_operation);
  return *historical;
}

int user_error_code(const gg::detail::UserError& error) {
  if (error.code() != GIT_EINVALID) return error.code();
  std::string message(error.what());
  std::ranges::transform(message, message.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (message.find("ambiguous") != std::string_view::npos) {
    return GIT_EAMBIGUOUS;
  }
  if (message.find("not found") != std::string_view::npos ||
      message.find("no such") != std::string_view::npos ||
      message.find("no ") == 0) {
    return GIT_ENOTFOUND;
  }
  if (message.find("already exists") != std::string_view::npos) {
    return GIT_EEXISTS;
  }
  if (message.find("conflict") != std::string_view::npos) {
    return GIT_ECONFLICT;
  }
  if (message.find("lock") != std::string_view::npos) return GIT_ELOCKED;
  if (message.find("unsupported") != std::string_view::npos) {
    return GIT_ENOTSUPPORTED;
  }
  if (message.find("revision") != std::string_view::npos ||
      message.find("pattern") != std::string_view::npos ||
      message.find("name") != std::string_view::npos) {
    return GIT_EINVALIDSPEC;
  }
  return GIT_EINVALID;
}

template <typename Function>
int boundary(Function&& function) noexcept {
  try {
    return function();
  } catch (const gg::detail::UserError& error) {
    git_error_set_str(GIT_ERROR_INVALID, error.what());
    return user_error_code(error);
  } catch (const gg::detail::GitError& error) {
    git_error_set_str(GIT_ERROR_REPOSITORY, error.what());
    return error.code();
  } catch (const std::bad_alloc&) {
    git_error_set_oom();
    return GIT_ERROR;
  } catch (const std::exception& error) {
    git_error_set_str(GIT_ERROR_INTERNAL, error.what());
    return GIT_ERROR;
  } catch (...) {  // GG_COV_EXCL_BRANCH
    git_error_set_str(GIT_ERROR_INTERNAL, "unknown gg error");
    return GIT_ERROR;
  }
}

template <typename Options>
const Options& required(const Options* options) {
  if (options == nullptr || options->version != GG_OPTIONS_VERSION) {
    throw gg::detail::UserError("invalid options version");
  }
  return *options;
}

void begin_operation(const gg_operation_options* options,
                     const char* phase) {
  if (options == nullptr) return;
  required(options);
  if (options->cancel_cb != nullptr && options->cancel_cb(options->payload)) {
    throw gg::detail::UserError("operation cancelled", GIT_EUSER);
  }
  if (options->progress_cb != nullptr) {
    options->progress_cb(phase, 0, 1, options->payload);
  }
}

void finish_operation(const gg_operation_options* options,
                      const char* phase) {
  if (options != nullptr && options->progress_cb != nullptr) {
    options->progress_cb(phase, 1, 1, options->payload);
  }
}

void clear(gg_mutation_result* result) {
  if (result == nullptr) {
    throw gg::detail::UserError("mutation result must not be null");
  }
  *result = {};
  result->version = GG_OPTIONS_VERSION;
}

void fill_mutation_result(
    gg_mutation_result* out,
    Repository& repository,
    const std::map<std::string, git_oid>& before,
    const std::optional<git_oid>& before_operation) {
  repository.invalidate_ref_cache();
  const auto after = repository.data_refs();
  const auto after_operation = repository.operation();
  std::set<std::string> names;
  for (const auto& [name, oid] : before) {
    (void)oid;
    names.insert(name);
  }
  for (const auto& [name, oid] : after) {
    (void)oid;
    names.insert(name);
  }

  std::vector<gg_reference_change> changes;
  std::vector<gg_rewrite> rewrites;
  for (const std::string& name : names) {
    const auto old_value = before.find(name);
    const auto new_value = after.find(name);
    const bool same = old_value != before.end() && new_value != after.end() &&
                      git_oid_equal(&old_value->second,
                                    &new_value->second) != 0;
    if (same) continue;
    gg_reference_change change{};
    change.name = duplicate(name);
    if (old_value != before.end()) {
      change.before = old_value->second;
      change.had_before = 1;
    }
    if (new_value != after.end()) {
      change.after = new_value->second;
      change.has_after = 1;
    }
    changes.push_back(change);
    if (name.starts_with(gg::detail::kChangePrefix) &&
        change.had_before && change.has_after) {
      rewrites.push_back({change.before, change.after});
    }
  }

  out->reference_count = changes.size();
  if (!changes.empty()) {
    out->references = static_cast<gg_reference_change*>(
        std::malloc(changes.size() * sizeof(gg_reference_change)));
    if (out->references == nullptr) throw std::bad_alloc();
    std::ranges::copy(changes, out->references);
  }
  out->rewrite_count = rewrites.size();
  if (!rewrites.empty()) {
    out->rewrites = static_cast<gg_rewrite*>(
        std::malloc(rewrites.size() * sizeof(gg_rewrite)));
    if (out->rewrites == nullptr) throw std::bad_alloc();
    std::ranges::copy(rewrites, out->rewrites);
  }
  const auto workspace = repository.workspace();
  if (workspace.has_value()) {
    out->working_copy = *workspace;
    out->has_working_copy = 1;
  }
  if (after_operation.has_value()) {
    out->operation = *after_operation;
    out->has_operation = 1;
  }
  const bool same_operation =
      before_operation.has_value() == after_operation.has_value() &&
      (!before_operation.has_value() ||
       git_oid_equal(&*before_operation, &*after_operation) != 0);
  out->changed = !changes.empty() || !same_operation;
}

template <typename Function>
int mutate(gg_mutation_result* out,
           gg_repository* repository,
           const gg_operation_options* options,
           const char* phase,
           Function&& function) {
  return boundary([&] {
    clear(out);
    if (repository == nullptr) {
      throw gg::detail::UserError("repository must not be null");
    }
    begin_operation(options, phase);
    Repository& implementation = repository->implementation;
    implementation.invalidate_ref_cache();
    const auto before = implementation.data_refs();
    const auto before_operation = implementation.operation();
    std::ostringstream discarded;
    function(implementation, discarded);
    fill_mutation_result(out, implementation, before, before_operation);
    finish_operation(options, phase);
    return GIT_OK;
  });
}

template <typename Options>
int initialize(Options* options, unsigned int version, Options defaults) {
  if (options == nullptr || version != GG_OPTIONS_VERSION) return GIT_EINVALID;
  *options = defaults;
  return GIT_OK;
}

}  // namespace

extern "C" {

int gg_operation_options_init(gg_operation_options* options,
                              unsigned int version) {
  return initialize(options, version, gg_operation_options GG_OPERATION_OPTIONS_INIT);
}

int gg_new_options_init(gg_new_options* options, unsigned int version) {
  return initialize(options, version, gg_new_options GG_NEW_OPTIONS_INIT);
}

int gg_commit_options_init(gg_commit_options* options, unsigned int version) {
  return initialize(options, version, gg_commit_options GG_COMMIT_OPTIONS_INIT);
}

int gg_describe_options_init(gg_describe_options* options,
                             unsigned int version) {
  return initialize(options, version,
                    gg_describe_options GG_DESCRIBE_OPTIONS_INIT);
}

int gg_metaedit_options_init(gg_metaedit_options* options,
                             unsigned int version) {
  return initialize(options, version,
                    gg_metaedit_options GG_METAEDIT_OPTIONS_INIT);
}

int gg_rebase_options_init(gg_rebase_options* options, unsigned int version) {
  return initialize(options, version, gg_rebase_options GG_REBASE_OPTIONS_INIT);
}

int gg_split_options_init(gg_split_options* options, unsigned int version) {
  return initialize(options, version, gg_split_options GG_SPLIT_OPTIONS_INIT);
}

int gg_squash_options_init(gg_squash_options* options, unsigned int version) {
  return initialize(options, version, gg_squash_options GG_SQUASH_OPTIONS_INIT);
}

int gg_abandon_options_init(gg_abandon_options* options,
                            unsigned int version) {
  return initialize(options, version,
                    gg_abandon_options GG_ABANDON_OPTIONS_INIT);
}

int gg_restore_options_init(gg_restore_options* options,
                            unsigned int version) {
  return initialize(options, version,
                    gg_restore_options GG_RESTORE_OPTIONS_INIT);
}

int gg_simplify_parents_options_init(gg_simplify_parents_options* options,
                                     unsigned int version) {
  return initialize(options, version,
                    gg_simplify_parents_options
                        GG_SIMPLIFY_PARENTS_OPTIONS_INIT);
}

int gg_bookmark_options_init(gg_bookmark_options* options,
                             unsigned int version) {
  return initialize(options, version,
                    gg_bookmark_options GG_BOOKMARK_OPTIONS_INIT);
}

int gg_tag_options_init(gg_tag_options* options, unsigned int version) {
  return initialize(options, version, gg_tag_options GG_TAG_OPTIONS_INIT);
}

int gg_move_options_init(gg_move_options* options, unsigned int version) {
  return initialize(options, version, gg_move_options GG_MOVE_OPTIONS_INIT);
}

int gg_workspace_add_options_init(gg_workspace_add_options* options,
                                  unsigned int version) {
  return initialize(options, version,
                    gg_workspace_add_options GG_WORKSPACE_ADD_OPTIONS_INIT);
}

int gg_fetch_options_init(gg_fetch_options* options, unsigned int version) {
  return initialize(options, version, gg_fetch_options GG_FETCH_OPTIONS_INIT);
}

int gg_push_options_init(gg_push_options* options, unsigned int version) {
  return initialize(options, version, gg_push_options GG_PUSH_OPTIONS_INIT);
}

int gg_revision_query_options_init(gg_revision_query_options* options,
                                   unsigned int version) {
  return initialize(options, version,
                    gg_revision_query_options GG_REVISION_QUERY_OPTIONS_INIT);
}

int gg_status_options_init(gg_status_options* options, unsigned int version) {
  return initialize(options, version, gg_status_options GG_STATUS_OPTIONS_INIT);
}

int gg_repository_attach(gg_repository** out, git_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("repository output and handle must not be null");
    }
    *out = new gg_repository(repository);
    return GIT_OK;
  });
}

void gg_repository_free(gg_repository* repository) { delete repository; }

git_repository* gg_repository_raw(gg_repository* repository) {
  return repository == nullptr ? nullptr : repository->implementation.raw();
}

int gg_repository_adopt_git_history(gg_repository* repository,
                                    const gg_operation_options* options) {
  return boundary([&] {
    if (repository == nullptr) {
      throw gg::detail::UserError("repository must not be null");
    }
    begin_operation(options, "adopt_git_history");
    repository->implementation.import_git_history();
    finish_operation(options, "adopt_git_history");
    return GIT_OK;
  });
}

int gg_repository_snapshot_working_copy(int* changed,
                                        gg_repository* repository,
                                        const gg_operation_options* options) {
  return boundary([&] {
    if (changed == nullptr || repository == nullptr) {
      throw gg::detail::UserError("snapshot output and repository must not be null");
    }
    begin_operation(options, "snapshot_working_copy");
    *changed = repository->implementation.sync_workspace();
    finish_operation(options, "snapshot_working_copy");
    return GIT_OK;
  });
}

int gg_repository_resolve(git_oid* out,
                          gg_repository* repository,
                          const char* revision) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr || revision == nullptr) {
      throw gg::detail::UserError("resolve arguments must not be null");
    }
    *out = repository->implementation.resolve(revision);
    return GIT_OK;
  });
}

int gg_repository_resolve_set(gg_oid_array* out,
                              gg_repository* repository,
                              const char* revisions) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr || revisions == nullptr) {
      throw gg::detail::UserError("resolve-set arguments must not be null");
    }
    *out = {};
    const auto values = repository->implementation.resolve_set(revisions);
    if (!values.empty()) {
      out->ids = static_cast<git_oid*>(
          std::malloc(values.size() * sizeof(git_oid)));
      if (out->ids == nullptr) throw std::bad_alloc();
      std::ranges::copy(values, out->ids);
      out->count = values.size();
    }
    return GIT_OK;
  });
}

int gg_repository_working_copy(git_oid* out, gg_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("working-copy arguments must not be null");
    }
    const auto value = repository->implementation.workspace();
    if (!value.has_value()) {
      throw gg::detail::UserError("working-copy change not found", GIT_ENOTFOUND);
    }
    *out = *value;
    return GIT_OK;
  });
}

int gg_repository_change_id(char** out,
                            gg_repository* repository,
                            const git_oid* revision) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr || revision == nullptr) {
      throw gg::detail::UserError("change-ID arguments must not be null");
    }
    const auto value = repository->implementation.change_id(*revision);
    if (!value.has_value()) {
      throw gg::detail::UserError("change ID not found", GIT_ENOTFOUND);
    }
    *out = duplicate(*value);
    return GIT_OK;
  });
}

int gg_repository_references(gg_reference_array* out,
                             gg_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("reference arguments must not be null");
    }
    *out = {};
    const auto refs = repository->implementation.data_refs();
    if (!refs.empty()) {
      out->items = static_cast<gg_reference*>(
          std::calloc(refs.size(), sizeof(gg_reference)));
      if (out->items == nullptr) throw std::bad_alloc();
      for (const auto& [name, oid] : refs) {
        out->items[out->count].name = duplicate(name);
        out->items[out->count].target = oid;
        ++out->count;
      }
    }
    return GIT_OK;
  });
}

int gg_repository_named_refs(gg_named_ref_array* out,
                             gg_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("named-reference arguments must not be null");
    }
    *out = {};
    Repository& repo = repository->implementation;
    const auto refs = repo.data_refs();
    struct NamedRef {
      std::string name;
      std::string remote;
      git_oid target;
      gg_named_ref_kind kind;
      bool tracked;
    };
    std::vector<NamedRef> values;
    const auto has_tracking = [&](std::string_view prefix,
                                  std::string_view remote,
                                  std::string_view name) {
      return refs.contains(std::string(prefix) + std::string(remote) + "/" +
                           std::string(name));
    };
    const auto locally_tracked = [&](std::string_view prefix,
                                     std::string_view name) {
      const std::string suffix = "/" + std::string(name);
      return std::ranges::any_of(refs, [&](const auto& item) {
        return item.first.starts_with(prefix) && item.first.ends_with(suffix);
      });
    };
    for (const auto& [reference, oid] : refs) {
      (void)oid;
      std::string name;
      std::string remote;
      gg_named_ref_kind kind{};
      bool tracked = false;
      if (reference.starts_with("refs/heads/")) {
        name = reference.substr(std::string_view("refs/heads/").size());
        kind = GG_NAMED_REF_LOCAL_BOOKMARK;
        tracked = locally_tracked(gg::detail::kBookmarkTrackingPrefix, name);
      } else if (reference.starts_with("refs/remotes/")) {
        const std::string suffix =
            reference.substr(std::string_view("refs/remotes/").size());
        const size_t slash = suffix.find('/');
        if (slash == std::string::npos || suffix.substr(slash + 1) == "HEAD") {
          continue;
        }
        remote = suffix.substr(0, slash);
        name = suffix.substr(slash + 1);
        kind = GG_NAMED_REF_REMOTE_BOOKMARK;
        tracked = has_tracking(gg::detail::kBookmarkTrackingPrefix, remote,
                               name);
      } else if (reference.starts_with("refs/tags/")) {
        name = reference.substr(std::string_view("refs/tags/").size());
        kind = GG_NAMED_REF_LOCAL_TAG;
        tracked = locally_tracked(gg::detail::kTagTrackingPrefix, name);
      } else if (reference.starts_with(gg::detail::kRemoteTagPrefix)) {
        const std::string suffix =
            reference.substr(gg::detail::kRemoteTagPrefix.size());
        constexpr std::string_view marker = "/tags/";
        const size_t separator = suffix.find(marker);
        if (separator == std::string::npos) continue;
        remote = suffix.substr(0, separator);
        name = suffix.substr(separator + marker.size());
        kind = GG_NAMED_REF_REMOTE_TAG;
        tracked = has_tracking(gg::detail::kTagTrackingPrefix, remote, name);
      } else {
        continue;
      }
      values.push_back(
          {std::move(name), std::move(remote), commit_target(repo, reference),
           kind, tracked});
    }
    if (values.empty()) return GIT_OK;
    out->items = static_cast<gg_named_ref*>(
        std::calloc(values.size(), sizeof(gg_named_ref)));
    if (out->items == nullptr) throw std::bad_alloc();
    for (const NamedRef& value : values) {
      gg_named_ref& item = out->items[out->count];
      item.name = duplicate(value.name);
      item.remote = duplicate(value.remote);
      item.target = value.target;
      item.kind = value.kind;
      item.tracked = value.tracked;
      item.conflicted = repo.commit_has_conflicts(value.target);
      ++out->count;
    }
    return GIT_OK;
  });
}

int gg_repository_conflict_paths(gg_owned_string_array* out,
                                 gg_repository* repository,
                                 const git_oid* revision) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr || revision == nullptr) {
      throw gg::detail::UserError("conflict arguments must not be null");
    }
    *out = {};
    const auto paths = repository->implementation.conflict_paths(*revision);
    if (!paths.empty()) {
      out->strings = static_cast<char**>(std::calloc(paths.size(), sizeof(char*)));
      if (out->strings == nullptr) throw std::bad_alloc();
      for (const std::string& path : paths) {
        out->strings[out->count++] = duplicate(path);
      }
    }
    return GIT_OK;
  });
}

int gg_repository_conflicts(gg_conflict_array* out,
                            gg_repository* repository,
                            const git_oid* revision) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr || revision == nullptr) {
      throw gg::detail::UserError("conflict arguments must not be null");
    }
    *out = {};
    Repository& repo = repository->implementation;
    const auto commit = repo.commit(*revision);
    const auto conflicts =
        repo.tree_conflicts(*git_commit_tree_id(commit.get()));
    if (conflicts.empty()) return GIT_OK;
    out->items = static_cast<gg_conflict*>(
        std::calloc(conflicts.size(), sizeof(gg_conflict)));
    if (out->items == nullptr) throw std::bad_alloc();
    const auto copy_terms = [](gg_conflict_term** output,
                               size_t* count,
                               const std::vector<gg::detail::FileValue>& terms) {
      if (terms.empty()) return;
      *output = static_cast<gg_conflict_term*>(
          std::calloc(terms.size(), sizeof(gg_conflict_term)));
      if (*output == nullptr) throw std::bad_alloc();
      *count = terms.size();
      for (size_t index = 0; index < terms.size(); ++index) {
        (*output)[index].oid = terms[index].oid;
        (*output)[index].mode = terms[index].mode;
        (*output)[index].present = terms[index].present;
      }
    };
    for (const auto& [path, conflict] : conflicts) {
      gg_conflict& item = out->items[out->count];
      item.path = duplicate(path);
      copy_terms(&item.removes, &item.remove_count, conflict.removes);
      copy_terms(&item.adds, &item.add_count, conflict.adds);
      ++out->count;
    }
    return GIT_OK;
  });
}

int gg_repository_revisions(gg_revision_array* out,
                            gg_repository* repository,
                            const gg_revision_query_options* options) {
  return boundary([&] {
    if (out == nullptr) {
      throw gg::detail::UserError("revision output must not be null");
    }
    *out = {};
    const auto& value = required(options);
    std::unique_ptr<Repository> historical;
    Repository& repo = query_repository(repository, value.at_operation,
                                        historical);
    std::vector<git_oid> revisions =
        repo.resolve_set(value.revisions == nullptr ? "all()" : value.revisions);
    if (value.reversed) std::reverse(revisions.begin(), revisions.end());
    if (revisions.size() > value.limit) revisions.resize(value.limit);
    if (revisions.empty()) return GIT_OK;
    out->items = static_cast<gg_revision*>(
        std::calloc(revisions.size(), sizeof(gg_revision)));
    if (out->items == nullptr) throw std::bad_alloc();
    for (const git_oid& oid : revisions) {
      gg_revision& item = out->items[out->count];
      item.oid = oid;
      const auto parents = repo.parents(oid);
      if (!parents.empty()) {
        item.parents.ids = static_cast<git_oid*>(
            std::malloc(parents.size() * sizeof(git_oid)));
        if (item.parents.ids == nullptr) throw std::bad_alloc();
        std::ranges::copy(parents, item.parents.ids);
        item.parents.count = parents.size();
      }
      const auto id = repo.change_id(oid);
      if (id.has_value()) item.change_id = duplicate(*id);
      auto commit = repo.commit(oid);
      item.description = duplicate(git_commit_message(commit.get()) == nullptr
                                       ? ""
                                       : git_commit_message(commit.get()));
      git_signature* author = nullptr;
      git_signature* committer = nullptr;
      gg::detail::check(git_signature_dup(&author,
                                          git_commit_author(commit.get())),
                        "copy author");
      item.author = author;
      gg::detail::check(git_signature_dup(&committer,
                                          git_commit_committer(commit.get())),
                        "copy committer");
      item.committer = committer;
      item.has_conflicts = repo.commit_has_conflicts(oid);
      ++out->count;
    }
    return GIT_OK;
  });
}

int gg_repository_status(gg_status* out,
                         gg_repository* repository,
                         const gg_status_options* options) {
  return boundary([&] {
    if (out == nullptr) {
      throw gg::detail::UserError("status output must not be null");
    }
    *out = {};
    const auto& value = required(options);
    const std::vector<std::string> filesets = strings(value.filesets);
    for (const std::string& fileset : filesets) {
      (void)gg::detail::fileset_matches(fileset, "");
    }
    std::unique_ptr<Repository> historical;
    Repository& repo = query_repository(repository, value.at_operation,
                                        historical);
    const auto workspace = repo.workspace();
    if (!workspace.has_value()) return GIT_OK;
    out->has_working_copy = 1;
    out->working_copy = *workspace;
    const auto parents = repo.parents(*workspace);
    if (!parents.empty()) {
      out->parents.ids = static_cast<git_oid*>(
          std::malloc(parents.size() * sizeof(git_oid)));
      if (out->parents.ids == nullptr) throw std::bad_alloc();
      std::ranges::copy(parents, out->parents.ids);
      out->parents.count = parents.size();
    }
    auto change = repo.commit(*workspace);
    const git_oid base_oid = parents.empty()
                                 ? repo.empty_tree()
                                 : *git_commit_tree_id(repo.commit(parents.front()).get());
    auto base = repo.tree(base_oid);
    auto tree = repo.tree(*git_commit_tree_id(change.get()));
    git_diff* raw_diff = nullptr;
    gg::detail::check(git_diff_tree_to_tree(&raw_diff, repo.raw(), base.get(),
                                            tree.get(), nullptr),
                      "compare working change");
    gg::detail::DiffPtr diff(raw_diff);
    git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT;
    gg::detail::check(git_diff_find_similar(diff.get(), &find),
                      "find renamed files");

    std::vector<gg_status_entry> entries;
    const auto selected = [&](const char* raw_path) {
      const std::string_view path = raw_path == nullptr ? "" : raw_path;
      return filesets.empty() || std::ranges::any_of(filesets, [&](const auto& fileset) {
        return gg::detail::fileset_matches(fileset, path);
      });
    };
    for (size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
      const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
      if (!selected(delta->old_file.path) && !selected(delta->new_file.path)) {
        continue;
      }
      gg_status_entry entry{};
      entry.status = delta->status;
      entry.old_path = duplicate(delta->old_file.path == nullptr
                                     ? ""
                                     : delta->old_file.path);
      entry.new_path = duplicate(delta->new_file.path == nullptr
                                     ? ""
                                     : delta->new_file.path);
      entry.old_mode = static_cast<git_filemode_t>(delta->old_file.mode);
      entry.new_mode = static_cast<git_filemode_t>(delta->new_file.mode);
      entries.push_back(entry);
    }
    for (const std::string& path : repo.conflict_paths(*workspace)) {
      if (!selected(path.c_str())) continue;
      auto found = std::ranges::find_if(entries, [&](const gg_status_entry& entry) {
        return path == entry.old_path || path == entry.new_path;
      });
      if (found != entries.end()) {
        found->conflicted = 1;
      } else {
        gg_status_entry entry{};
        entry.status = GIT_DELTA_CONFLICTED;
        entry.old_path = duplicate(path);
        entry.new_path = duplicate(path);
        entry.conflicted = 1;
        entries.push_back(entry);
      }
    }
    if (!entries.empty()) {
      out->entries = static_cast<gg_status_entry*>(
          std::malloc(entries.size() * sizeof(gg_status_entry)));
      if (out->entries == nullptr) throw std::bad_alloc();
      std::ranges::copy(entries, out->entries);
      out->entry_count = entries.size();
    }
    return GIT_OK;
  });
}

int gg_repository_operations(gg_operation_array* out,
                             gg_repository* repository,
                             size_t limit) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("operation arguments must not be null");
    }
    *out = {};
    Repository& repo = repository->implementation;
    std::vector<gg_operation> operations;
    auto current = repo.operation();
    while (current.has_value() && operations.size() < limit) {
      auto commit = repo.commit(*current);
      gg_operation item{};
      item.oid = *current;
      const auto previous = repo.operation_previous(commit.get());
      if (previous.has_value()) {
        item.previous = *previous;
        item.has_previous = 1;
      }
      item.description = duplicate(repo.operation_description(commit.get()));
      const git_signature* author = git_commit_author(commit.get());
      item.time = author->when.time;
      item.offset = author->when.offset;
      operations.push_back(item);
      current = previous;
    }
    if (!operations.empty()) {
      out->items = static_cast<gg_operation*>(
          std::malloc(operations.size() * sizeof(gg_operation)));
      if (out->items == nullptr) throw std::bad_alloc();
      std::ranges::copy(operations, out->items);
      out->count = operations.size();
    }
    return GIT_OK;
  });
}

int gg_repository_workspaces(gg_workspace_array* out,
                             gg_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("workspace arguments must not be null");
    }
    *out = {};
    Repository& repo = repository->implementation;
    const auto roots = repo.workspace_roots();
    std::vector<std::pair<std::string, git_oid>> workspaces;
    for (const auto& [reference, oid] : repo.data_refs()) {
      if (reference.starts_with(gg::detail::kWorkspacePrefix)) {
        workspaces.emplace_back(
            reference.substr(gg::detail::kWorkspacePrefix.size()), oid);
      }
    }
    if (workspaces.empty()) return GIT_OK;
    out->items = static_cast<gg_workspace*>(
        std::calloc(workspaces.size(), sizeof(gg_workspace)));
    if (out->items == nullptr) throw std::bad_alloc();
    for (const auto& [name, oid] : workspaces) {
      gg_workspace& item = out->items[out->count];
      item.name = duplicate(name);
      item.working_copy = oid;
      const auto root = roots.find(name);
      item.stale = root == roots.end();
      item.root = duplicate(item.stale ? "" : root->second.string());
      ++out->count;
    }
    return GIT_OK;
  });
}

int gg_repository_sparse_patterns(gg_owned_string_array* out,
                                  gg_repository* repository) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("sparse-pattern arguments must not be null");
    }
    *out = {};
    const std::filesystem::path path =
        std::filesystem::path(git_repository_path(
            repository->implementation.raw())) / "info" / "sparse-checkout";
    std::ifstream input(path);
    std::vector<std::string> patterns;
    std::string line;
    while (std::getline(input, line)) patterns.push_back(line);
    if (patterns.empty()) patterns.emplace_back(".");
    assign_strings(out, patterns);
    return GIT_OK;
  });
}

int gg_repository_new_change(gg_mutation_result* out,
                             gg_repository* repository,
                             const gg_new_options* options,
                             const gg_operation_options* operation) {
  return mutate(out, repository, operation, "new_change", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    command_new(repo, NewCommand{string(value.message), strings(value.parents),
                                 strings(value.insert_after),
                                 strings(value.insert_before),
                                 value.no_edit != 0}, output);
  });
}

int gg_repository_commit(gg_mutation_result* out,
                         gg_repository* repository,
                         const gg_commit_options* options,
                         const gg_operation_options* operation) {
  return mutate(out, repository, operation, "commit", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    CommitCommand command;
    command.paths = strings(value.filesets);
    command.message = string(value.message);
    command.message_provided = value.message_provided != 0;
    command_commit(repo, command, output);
  });
}

int gg_repository_describe(gg_mutation_result* out,
                           gg_repository* repository,
                           const gg_describe_options* options,
                           const gg_operation_options* operation) {
  return mutate(out, repository, operation, "describe", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    if (!value.message_provided) {
      throw gg::detail::UserError(
          "describe requires a final message in the library API");
    }
    DescribeCommand command;
    command.revisions = strings(value.revisions);
    command.message = string(value.message);
    command.message_provided = value.message_provided != 0;
    command_describe(repo, command, output);
  });
}

int gg_repository_metaedit(gg_mutation_result* out,
                           gg_repository* repository,
                           const gg_metaedit_options* options,
                           const gg_operation_options* operation) {
  return mutate(out, repository, operation, "metaedit", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    MetaeditCommand command;
    command.revisions = strings(value.revisions);
    command.message = string(value.message);
    command.author = string(value.author);
    command.author_timestamp = string(value.author_timestamp);
    command.message_provided = value.message_provided != 0;
    command.author_provided = value.author_provided != 0;
    command.author_timestamp_provided = value.author_timestamp_provided != 0;
    command.update_change_id = value.update_change_id != 0;
    command.update_author = value.update_author != 0;
    command.update_author_timestamp = value.update_author_timestamp != 0;
    command.force_rewrite = value.force_rewrite != 0;
    command_metaedit(repo, command, output);
  });
}

int gg_repository_edit(gg_mutation_result* out,
                       gg_repository* repository,
                       const char* revision,
                       const gg_operation_options* operation) {
  return mutate(out, repository, operation, "edit", [&](Repository& repo, std::ostream& output) {
    if (revision == nullptr) throw gg::detail::UserError("revision must not be null");
    command_edit(repo, EditCommand{revision}, output);
  });
}

int gg_repository_move(gg_mutation_result* out,
                       gg_repository* repository,
                       const gg_move_options* options,
                       const gg_operation_options* operation) {
  return mutate(out, repository, operation, "move", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    if ((value.direction != GG_MOVE_NEXT &&
         value.direction != GG_MOVE_PREVIOUS) ||
        value.offset == 0) {
      throw gg::detail::UserError("invalid movement options");
    }
    MovementCommand command;
    command.direction = value.direction == GG_MOVE_PREVIOUS
                            ? MovementDirection::previous
                            : MovementDirection::next;
    command.offset = value.offset;
    command.edit = value.edit != 0;
    command.conflict = value.conflict != 0;
    command_move(repo, command, output);
  });
}

int gg_repository_rebase(gg_mutation_result* out,
                         gg_repository* repository,
                         const gg_rebase_options* options,
                         const gg_operation_options* operation) {
  return mutate(out, repository, operation, "rebase", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    command_rebase(repo, RebaseCommand{string(value.source), string(value.destination)}, output);
  });
}

int gg_repository_split(gg_mutation_result* out,
                        gg_repository* repository,
                        const gg_split_options* options,
                        const gg_operation_options* operation) {
  return mutate(out, repository, operation, "split", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    command_split(repo, SplitCommand{string(value.revision), string(value.message), strings(value.filesets)}, output);
  });
}

int gg_repository_squash(gg_mutation_result* out,
                         gg_repository* repository,
                         const gg_squash_options* options,
                         const gg_operation_options* operation) {
  return mutate(out, repository, operation, "squash", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    command_squash(repo, SquashCommand{string(value.revision), string(value.source), string(value.destination), string(value.message)}, output);
  });
}

int gg_repository_abandon(gg_mutation_result* out,
                          gg_repository* repository,
                          const gg_abandon_options* options,
                          const gg_operation_options* operation) {
  return mutate(out, repository, operation, "abandon", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    AbandonCommand command;
    command.revisions = strings(value.revisions);
    command.retain_bookmarks = value.retain_bookmarks != 0;
    command.restore_descendants = value.restore_descendants != 0;
    command_abandon(repo, command, output);
  });
}

int gg_repository_restore(gg_mutation_result* out,
                          gg_repository* repository,
                          const gg_restore_options* options,
                          const gg_operation_options* operation) {
  return mutate(out, repository, operation, "restore", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    RestoreCommand command;
    command.paths = strings(value.filesets);
    command.from = string(value.from);
    command.into = string(value.into);
    command.changes_in = string(value.changes_in);
    command.restore_descendants = value.restore_descendants != 0;
    command_restore(repo, command, output);
  });
}

int gg_repository_simplify_parents(
    gg_mutation_result* out,
    gg_repository* repository,
    const gg_simplify_parents_options* options,
    const gg_operation_options* operation) {
  return mutate(out, repository, operation, "simplify_parents", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    command_simplify_parents(repo, SimplifyParentsCommand{strings(value.sources), strings(value.revisions)}, output);
  });
}

int gg_repository_bookmark(gg_mutation_result* out,
                           gg_repository* repository,
                           const gg_bookmark_options* options,
                           const gg_operation_options* operation) {
  return mutate(out, repository, operation, "bookmark", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    BookmarkCommand command;
    switch (value.action) {
      case GG_BOOKMARK_ADVANCE: command.action = BookmarkAction::advance; break;
      case GG_BOOKMARK_CREATE: command.action = BookmarkAction::create; break;
      case GG_BOOKMARK_SET: command.action = BookmarkAction::set; break;
      case GG_BOOKMARK_MOVE: command.action = BookmarkAction::move; break;
      case GG_BOOKMARK_DELETE: command.action = BookmarkAction::erase; break;
      case GG_BOOKMARK_FORGET: command.action = BookmarkAction::forget; break;
      case GG_BOOKMARK_RENAME: command.action = BookmarkAction::rename; break;
      case GG_BOOKMARK_TRACK: command.action = BookmarkAction::track; break;
      case GG_BOOKMARK_UNTRACK: command.action = BookmarkAction::untrack; break;
      default: throw gg::detail::UserError("invalid bookmark action");
    }
    command.names = strings(value.names);
    command.from = strings(value.from);
    command.remotes = strings(value.remotes);
    command.revision = string(value.revision);
    command.allow_backwards = value.allow_backwards != 0;
    command.include_remotes = value.include_remotes != 0;
    command.overwrite_existing = value.overwrite_existing != 0;
    command_bookmark(repo, command, output);
  });
}

int gg_repository_tag(gg_mutation_result* out,
                      gg_repository* repository,
                      const gg_tag_options* options,
                      const gg_operation_options* operation) {
  return mutate(out, repository, operation, "tag", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    TagCommand command;
    switch (value.action) {
      case GG_TAG_SET: command.action = TagAction::set; break;
      case GG_TAG_DELETE: command.action = TagAction::erase; break;
      case GG_TAG_TRACK: command.action = TagAction::track; break;
      case GG_TAG_UNTRACK: command.action = TagAction::untrack; break;
      default: throw gg::detail::UserError("invalid tag action");
    }
    command.names = strings(value.names);
    command.remotes = strings(value.remotes);
    command.revision = string(value.revision);
    command.allow_move = value.allow_move != 0;
    command_tag(repo, command, output);
  });
}

int gg_repository_undo(gg_mutation_result* out,
                       gg_repository* repository,
                       const gg_operation_options* operation) {
  return mutate(out, repository, operation, "undo", gg::detail::command_undo);
}

int gg_repository_redo(gg_mutation_result* out,
                       gg_repository* repository,
                       const gg_operation_options* operation) {
  return mutate(out, repository, operation, "redo", gg::detail::command_redo);
}

int gg_repository_restore_operation(
    gg_mutation_result* out,
    gg_repository* repository,
    const char* operation,
    unsigned int flags,
    const gg_operation_options* operation_options) {
  return mutate(out, repository, operation_options, "restore_operation", [&](Repository& repo, std::ostream& output) {
    if (operation == nullptr || flags == 0 ||
        (flags & ~GG_RESTORE_ALL) != 0) {
      throw gg::detail::UserError("invalid operation restore request");
    }
    OperationRestoreCommand command;
    command.operation = operation;
    if (flags != GG_RESTORE_ALL) {
      if ((flags & GG_RESTORE_REPOSITORY) != 0) command.what.emplace_back("repo");
      if ((flags & GG_RESTORE_REMOTE_TRACKING) != 0) command.what.emplace_back("remote-tracking");
    }
    command_operation_restore(repo, command, output);
  });
}

int gg_repository_workspace_add(gg_mutation_result* out,
                                gg_repository* repository,
                                const gg_workspace_add_options* options,
                                const gg_operation_options* operation) {
  return mutate(out, repository, operation, "workspace_add", [&](Repository& repo, std::ostream& output) {
    const auto& value = required(options);
    const std::string sparse =
        value.sparse_patterns == nullptr ? "copy" : value.sparse_patterns;
    if (sparse != "copy" && sparse != "full" && sparse != "empty") {
      throw gg::detail::UserError("invalid sparse-pattern mode");
    }
    WorkspaceCommand command;
    command.action = WorkspaceAction::add;
    command.name = string(value.name);
    command.destination = string(value.destination);
    command.revision = string(value.revision);
    command.message = string(value.message);
    command.sparse_patterns = sparse;
    command_workspace(repo, command, output);
  });
}

int gg_repository_workspace_forget(gg_mutation_result* out,
                                   gg_repository* repository,
                                   gg_string_array names,
                                   const gg_operation_options* operation) {
  return mutate(out, repository, operation, "workspace_forget", [&](Repository& repo, std::ostream& output) {
    WorkspaceCommand command;
    command.action = WorkspaceAction::forget;
    command.names = strings(names);
    command_workspace(repo, command, output);
  });
}

int gg_repository_workspace_rename(gg_mutation_result* out,
                                   gg_repository* repository,
                                   const char* name,
                                   const gg_operation_options* operation) {
  return mutate(out, repository, operation, "workspace_rename", [&](Repository& repo, std::ostream& output) {
    if (name == nullptr) throw gg::detail::UserError("workspace name must not be null");
    WorkspaceCommand command;
    command.action = WorkspaceAction::rename;
    command.name = name;
    command_workspace(repo, command, output);
  });
}

int gg_repository_sparse_reset(gg_mutation_result* out,
                               gg_repository* repository,
                               const gg_operation_options* operation) {
  const int result = mutate(out, repository, operation, "sparse_reset", [&](Repository& repo, std::ostream& output) {
    command_sparse(repo, SparseCommand{SparseAction::reset}, output);
  });
  if (result == GIT_OK) out->changed = 1;
  return result;
}

int gg_repository_track_paths(gg_mutation_result* out,
                              gg_repository* repository,
                              gg_string_array filesets,
                              int include_ignored,
                              const gg_operation_options* operation) {
  const int result = mutate(out, repository, operation, "track_paths", [&](Repository& repo, std::ostream& output) {
    FileCommand command;
    command.action = FileAction::track;
    command.paths = strings(filesets);
    command.include_ignored = include_ignored != 0;
    command_file(repo, command, output);
  });
  if (result == GIT_OK) out->changed = 1;
  return result;
}

int gg_repository_untrack_paths(gg_mutation_result* out,
                                gg_repository* repository,
                                gg_string_array filesets,
                                const gg_operation_options* operation) {
  const int result = mutate(out, repository, operation, "untrack_paths", [&](Repository& repo, std::ostream& output) {
    FileCommand command;
    command.action = FileAction::untrack;
    command.paths = strings(filesets);
    command_file(repo, command, output);
  });
  if (result == GIT_OK) out->changed = 1;
  return result;
}

int gg_repository_chmod(gg_mutation_result* out,
                        gg_repository* repository,
                        gg_string_array filesets,
                        int executable,
                        const gg_operation_options* operation) {
  return mutate(out, repository, operation, "chmod", [&](Repository& repo, std::ostream& output) {
    FileCommand command;
    command.action = FileAction::chmod;
    command.paths = strings(filesets);
    command.mode = executable != 0 ? "executable" : "normal";
    command_file(repo, command, output);
  });
}

int gg_repository_plan_fetch(gg_transport_plan* out,
                             gg_repository* repository,
                             const gg_fetch_options* options) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("fetch plan arguments must not be null");
    }
    *out = {};
    out->version = GG_OPTIONS_VERSION;
    const auto& value = required(options);
    if ((value.all_remotes && value.remotes.count != 0) ||
        (value.tracked &&
         (value.branches.count != 0 || value.tags.count != 0))) {
      throw gg::detail::UserError("incompatible fetch options");
    }
    if (value.advertised_ref_count != 0 && value.advertised_refs == nullptr) {
      throw gg::detail::UserError("advertised refs must not be null");
    }

    std::map<std::string, std::vector<const gg_advertised_ref*>> advertised;
    for (size_t index = 0; index < value.advertised_ref_count; ++index) {
      const gg_advertised_ref& ref = value.advertised_refs[index];
      if (ref.remote == nullptr || ref.name == nullptr ||
          (ref.kind != GG_REMOTE_BRANCH && ref.kind != GG_REMOTE_TAG)) {
        throw gg::detail::UserError("invalid advertised ref");
      }
      advertised[ref.remote].push_back(&ref);
    }
    std::vector<std::string> available_remotes;
    for (const auto& [remote, refs] : advertised) {
      (void)refs;
      available_remotes.push_back(remote);
    }
    std::vector<std::string> remotes;
    if (value.all_remotes) {
      remotes = available_remotes;
    } else if (value.remotes.count == 0) {
      remotes = {"origin"};
      if (!advertised.contains("origin")) {
        throw gg::detail::UserError("remote not found: origin", GIT_ENOTFOUND);
      }
    } else {
      remotes = matching_names(value.remotes, available_remotes, "remote", false);
    }
    if (remotes.empty()) {
      throw gg::detail::UserError("no remotes found", GIT_ENOTFOUND);
    }

    std::vector<std::string> deletes;
    const auto data_refs = repository->implementation.data_refs();
    for (const std::string& remote : remotes) {
      std::vector<std::string> branches;
      std::vector<std::string> tags;
      std::map<std::string, const gg_advertised_ref*> by_branch;
      std::map<std::string, const gg_advertised_ref*> by_tag;
      for (const gg_advertised_ref* ref : advertised[remote]) {
        (ref->kind == GG_REMOTE_BRANCH ? by_branch : by_tag).emplace(ref->name,
                                                                     ref);
      }
      for (const auto& [name, ref] : by_branch) {
        (void)ref;
        branches.push_back(name);
      }
      for (const auto& [name, ref] : by_tag) {
        (void)ref;
        tags.push_back(name);
      }

      if (value.tracked) {
        branches.clear();
        tags.clear();
        const std::string bookmark_prefix =
            std::string(gg::detail::kBookmarkTrackingPrefix) + remote + "/";
        const std::string tag_prefix =
            std::string(gg::detail::kTagTrackingPrefix) + remote + "/";
        for (const auto& [reference, oid] : data_refs) {
          (void)oid;
          if (reference.starts_with(bookmark_prefix)) {
            const std::string name = reference.substr(bookmark_prefix.size());
            if (by_branch.contains(name)) branches.push_back(name);
            else {
              deletes.push_back(reference);
              deletes.push_back("refs/remotes/" + remote + "/" + name);
            }
          } else if (reference.starts_with(tag_prefix)) {
            const std::string name = reference.substr(tag_prefix.size());
            if (by_tag.contains(name)) tags.push_back(name);
            else {
              deletes.push_back(reference);
              deletes.push_back(std::string(gg::detail::kRemoteTagPrefix) +
                                remote + "/tags/" + name);
            }
          }
        }
      } else {
        const bool default_selection = value.branches.count == 0 &&
                                       value.tags.count == 0;
        branches = matching_names(value.branches, branches, "branch",
                                  default_selection);
        tags = matching_names(value.tags, tags, "tag", default_selection);
      }

      for (const std::string& branch : branches) {
        const auto* ref = by_branch.at(branch);
        add_refspec(out, remote, "refs/heads/" + branch,
                    "refs/remotes/" + remote + "/" + branch, ref->target);
      }
      for (const std::string& tag : tags) {
        const auto* ref = by_tag.at(tag);
        add_refspec(out, remote, "refs/tags/" + tag,
                    "refs/tags/" + tag, ref->target);
      }
    }
    assign_strings(&out->reference_deletes, deletes);
    return GIT_OK;
  });
}

int gg_repository_complete_fetch(gg_mutation_result* out,
                                 gg_repository* repository,
                                 const gg_transport_plan* plan,
                                 const gg_operation_options* operation) {
  return mutate(out, repository, operation, "complete_fetch", [&](Repository& repo, std::ostream&) {
    if (plan == nullptr || plan->version != GG_OPTIONS_VERSION ||
        (plan->refspec_count != 0 && plan->refspecs == nullptr)) {
      throw gg::detail::UserError("invalid fetch plan");
    }
    std::map<std::string, git_oid> updates = repo.missing_change_ids();
    std::set<std::string> deletes;
    for (size_t index = 0; index < plan->reference_deletes.count; ++index) {
      deletes.insert(plan->reference_deletes.strings[index]);
    }
    for (size_t index = 0; index < plan->refspec_count; ++index) {
      const gg_refspec& refspec = plan->refspecs[index];
      if (refspec.remote == nullptr || refspec.destination == nullptr ||
          !refspec.has_target) {
        throw gg::detail::UserError("invalid fetch plan");
      }
      const auto fetched = repo.ref_target(refspec.destination);
      if (!fetched.has_value() ||
          git_oid_equal(&*fetched, &refspec.target) == 0) {
        throw gg::detail::UserError("fetched reference changed since planning",
                                    GIT_EMODIFIED);
      }
      const std::string destination = refspec.destination;
      if (destination.starts_with("refs/remotes/")) {
        const std::string prefix = "refs/remotes/" +
                                   std::string(refspec.remote) + "/";
        if (!destination.starts_with(prefix)) {
          throw gg::detail::UserError("invalid fetch destination");
        }
        const std::string name = destination.substr(prefix.size());
        updates[std::string(gg::detail::kBookmarkTrackingPrefix) +
                refspec.remote + "/" + name] = refspec.target;
      } else if (destination.starts_with("refs/tags/")) {
        const std::string name = destination.substr(std::string("refs/tags/").size());
        updates[std::string(gg::detail::kRemoteTagPrefix) + refspec.remote +
                "/tags/" + name] = refspec.target;
        updates[std::string(gg::detail::kTagTrackingPrefix) + refspec.remote +
                "/" + name] = refspec.target;
      } else {
        throw gg::detail::UserError("invalid fetch destination");
      }
    }
    repo.record(std::move(updates), std::move(deletes), repo.head_state(),
                "gg fetch");
  });
}

int gg_repository_plan_push(gg_transport_plan* out,
                            gg_repository* repository,
                            const gg_push_options* options) {
  return boundary([&] {
    if (out == nullptr || repository == nullptr) {
      throw gg::detail::UserError("push plan arguments must not be null");
    }
    *out = {};
    out->version = GG_OPTIONS_VERSION;
    out->atomic = 1;
    const auto& value = required(options);
    Repository& repo = repository->implementation;
    const std::string remote = value.remote == nullptr ? "origin" : value.remote;
    git_remote* raw_remote = nullptr;
    const int lookup = git_remote_lookup(&raw_remote, repo.raw(), remote.c_str());
    if (lookup == GIT_ENOTFOUND) {
      throw gg::detail::UserError("remote not found: " + remote, GIT_ENOTFOUND);
    }
    gg::detail::check(lookup, "find remote");
    git_remote_free(raw_remote);

    const auto refs = repo.data_refs();
    std::map<std::string, std::string> selected;
    const auto add_kind = [&](std::string_view kind,
                              gg_string_array patterns) {
      const std::string prefix = "refs/" + std::string(kind) + "/";
      std::vector<std::string> available;
      for (const auto& [reference, oid] : refs) {
        (void)oid;
        if (reference.starts_with(prefix)) {
          available.push_back(reference.substr(prefix.size()));
        }
      }
      for (const std::string& name :
           matching_names(patterns, available,
                          kind == "heads" ? "bookmark" : "tag", false)) {
        selected[prefix + name] = prefix + name;
      }
    };
    add_kind("heads", value.bookmarks);
    add_kind("tags", value.tags);

    std::set<git_oid, gg::detail::OidLess> revisions;
    for (const std::string& expression : strings(value.revisions)) {
      const auto resolved = repo.resolve_set(expression);
      revisions.insert(resolved.begin(), resolved.end());
    }
    for (const auto& [reference, oid] : refs) {
      (void)oid;
      if ((!reference.starts_with("refs/heads/") &&
           !reference.starts_with("refs/tags/")) ||
          !revisions.contains(commit_target(repo, reference))) {
        continue;
      }
      selected[reference] = reference;
    }
    for (const git_oid& revision : revisions) {
      const bool named = std::ranges::any_of(selected, [&](const auto& item) {
        const git_oid target = commit_target(repo, item.second);
        return git_oid_equal(&target, &revision) != 0;
      });
      if (!named) {
        throw gg::detail::UserError(
            "push revision is not named by a local bookmark or tag",
            GIT_EINVALIDSPEC);
      }
    }

    if (value.all) {
      for (const auto& [reference, oid] : refs) {
        (void)oid;
        if (reference.starts_with("refs/heads/") ||
            reference.starts_with("refs/tags/")) {
          selected[reference] = reference;
        }
      }
    }
    const bool default_selection = !value.all && !value.tracked &&
                                   !value.deleted && value.bookmarks.count == 0 &&
                                   value.tags.count == 0 && value.revisions.count == 0;
    std::vector<std::string> deletes;
    const std::string bookmark_tracking =
        std::string(gg::detail::kBookmarkTrackingPrefix) + remote + "/";
    const std::string tag_tracking =
        std::string(gg::detail::kTagTrackingPrefix) + remote + "/";
    for (const auto& [reference, oid] : refs) {
      (void)oid;
      std::string local;
      if (reference.starts_with(bookmark_tracking)) {
        local = "refs/heads/" + reference.substr(bookmark_tracking.size());
      } else if (reference.starts_with(tag_tracking)) {
        local = "refs/tags/" + reference.substr(tag_tracking.size());
      } else {
        continue;
      }
      if ((value.tracked || default_selection) && repo.ref_target(local).has_value()) {
        selected[local] = local;
      }
      if (value.deleted && !repo.ref_target(local).has_value()) {
        selected[local] = "";
        deletes.push_back(reference);
      }
    }

    for (const auto& [destination, source] : selected) {
      if (source.empty()) {
        add_refspec(out, remote, "", destination, std::nullopt);
        continue;
      }
      const git_oid target = commit_target(repo, source);
      if (!value.allow_empty_description) {
        auto commit = repo.commit(target);
        if (gg::detail::first_line(git_commit_message(commit.get())).empty()) {
          throw gg::detail::UserError(
              "refusing to push an empty description: " + source);
        }
      }
      if (repo.history_has_conflicts(target)) {
        throw gg::detail::UserError("refusing to push conflicted history: " +
                                    destination,
                                    GIT_ECONFLICT);
      }
      add_refspec(out, remote, source, destination, target);
    }
    assign_strings(&out->reference_deletes, deletes);
    assign_strings(&out->push_options, strings(value.push_options));
    return GIT_OK;
  });
}

int gg_repository_complete_push(gg_mutation_result* out,
                                gg_repository* repository,
                                const gg_transport_plan* plan,
                                const gg_operation_options* operation) {
  return mutate(out, repository, operation, "complete_push", [&](Repository& repo, std::ostream&) {
    if (plan == nullptr || plan->version != GG_OPTIONS_VERSION || !plan->atomic ||
        (plan->refspec_count != 0 && plan->refspecs == nullptr)) {
      throw gg::detail::UserError("invalid push plan");
    }
    std::map<std::string, git_oid> updates;
    std::set<std::string> deletes;
    for (size_t index = 0; index < plan->reference_deletes.count; ++index) {
      deletes.insert(plan->reference_deletes.strings[index]);
    }
    for (size_t index = 0; index < plan->refspec_count; ++index) {
      const gg_refspec& refspec = plan->refspecs[index];
      if (refspec.remote == nullptr || refspec.source == nullptr ||
          refspec.destination == nullptr) {
        throw gg::detail::UserError("invalid push plan");
      }
      if (*refspec.source == '\0') continue;
      const git_oid current = commit_target(repo, refspec.source);
      if (!refspec.has_target ||
          git_oid_equal(&current, &refspec.target) == 0) {
        throw gg::detail::UserError("push source changed since planning",
                                    GIT_EMODIFIED);
      }
      const std::string destination = refspec.destination;
      if (destination.starts_with("refs/heads/")) {
        const std::string name = destination.substr(std::string("refs/heads/").size());
        updates["refs/remotes/" + std::string(refspec.remote) + "/" + name] = current;
        updates[std::string(gg::detail::kBookmarkTrackingPrefix) +
                refspec.remote + "/" + name] = current;
      } else if (destination.starts_with("refs/tags/")) {
        const std::string name = destination.substr(std::string("refs/tags/").size());
        const git_oid tag = *repo.ref_target(refspec.source);
        updates[std::string(gg::detail::kRemoteTagPrefix) + refspec.remote +
                "/tags/" + name] = tag;
        updates[std::string(gg::detail::kTagTrackingPrefix) + refspec.remote +
                "/" + name] = tag;
      } else {
        throw gg::detail::UserError("invalid push destination");
      }
    }
    repo.record(std::move(updates), std::move(deletes), repo.head_state(),
                "gg push");
  });
}

void gg_oid_array_dispose(gg_oid_array* array) {
  if (array == nullptr) return;
  std::free(array->ids);
  *array = {};
}

void gg_owned_string_array_dispose(gg_owned_string_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->strings[index]);
  }
  std::free(array->strings);
  *array = {};
}

void gg_reference_array_dispose(gg_reference_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->items[index].name);
  }
  std::free(array->items);
  *array = {};
}

void gg_named_ref_array_dispose(gg_named_ref_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->items[index].name);
    std::free(array->items[index].remote);
  }
  std::free(array->items);
  *array = {};
}

void gg_conflict_array_dispose(gg_conflict_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->items[index].path);
    std::free(array->items[index].removes);
    std::free(array->items[index].adds);
  }
  std::free(array->items);
  *array = {};
}

void gg_mutation_result_dispose(gg_mutation_result* result) {
  if (result == nullptr) return;
  for (size_t index = 0; index < result->reference_count; ++index) {
    std::free(result->references[index].name);
  }
  std::free(result->references);
  std::free(result->rewrites);
  *result = {};
}

void gg_transport_plan_dispose(gg_transport_plan* plan) {
  if (plan == nullptr) return;
  for (size_t index = 0; index < plan->refspec_count; ++index) {
    std::free(plan->refspecs[index].remote);
    std::free(plan->refspecs[index].source);
    std::free(plan->refspecs[index].destination);
  }
  std::free(plan->refspecs);
  gg_owned_string_array_dispose(&plan->reference_deletes);
  gg_owned_string_array_dispose(&plan->push_options);
  *plan = {};
}

void gg_revision_array_dispose(gg_revision_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    gg_revision& item = array->items[index];
    gg_oid_array_dispose(&item.parents);
    std::free(item.change_id);
    std::free(item.description);
    git_signature_free(item.author);
    git_signature_free(item.committer);
  }
  std::free(array->items);
  *array = {};
}

void gg_status_dispose(gg_status* status) {
  if (status == nullptr) return;
  gg_oid_array_dispose(&status->parents);
  for (size_t index = 0; index < status->entry_count; ++index) {
    std::free(status->entries[index].old_path);
    std::free(status->entries[index].new_path);
  }
  std::free(status->entries);
  *status = {};
}

void gg_operation_array_dispose(gg_operation_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->items[index].description);
  }
  std::free(array->items);
  *array = {};
}

void gg_workspace_array_dispose(gg_workspace_array* array) {
  if (array == nullptr) return;
  for (size_t index = 0; index < array->count; ++index) {
    std::free(array->items[index].name);
    std::free(array->items[index].root);
  }
  std::free(array->items);
  *array = {};
}

void gg_string_dispose(char* value) { std::free(value); }

}  // extern "C"
