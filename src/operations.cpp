// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <exception>
#include <sstream>

namespace gg::detail {
namespace {

bool refs_equal(const std::map<std::string, git_oid>& left,
                const std::map<std::string, git_oid>& right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const auto& first, const auto& second) {
                      return first.first == second.first &&
                             first.second == second.second;
                    });
}

constexpr std::string_view kOperationV2 = "gg-operation-v2";
constexpr std::string_view kOperationV3 = "gg-operation-v3";
constexpr std::string_view kOperationV4 = "gg-operation-v4";
constexpr std::size_t kMaxOperationParents = 128;

git_oid create_keepalive(const Repository& repo,
                         std::vector<git_oid> targets) {
  const git_oid tree = repo.empty_tree();
  while (targets.size() > kMaxOperationParents) {
    std::vector<git_oid> next;
    for (std::size_t begin = 0; begin < targets.size();
         begin += kMaxOperationParents) {
      const std::size_t end =
          std::min(targets.size(), begin + kMaxOperationParents);
      next.push_back(repo.create_commit(
          tree, {targets.begin() + static_cast<std::ptrdiff_t>(begin),
                 targets.begin() + static_cast<std::ptrdiff_t>(end)},
          "gg operation keepalive"));
    }
    targets = std::move(next);
  }
  return repo.create_commit(tree, targets, "gg operation keepalive");
}

std::string migrate_operation_description(
    std::string description,
    const std::map<git_oid, git_oid, OidLess>& rewritten) {
  for (const std::string_view prefix : {
           "undo: restore to operation ",
           "redo: restore to operation ",
       }) {
    if (!starts_with(description, prefix)) continue;
    const std::string_view target(description.data() + prefix.size(),
                                  description.size() - prefix.size());
    for (const auto& [old_oid, new_oid] : rewritten) {
      if (target == oid_string(old_oid)) {
        return std::string(prefix) + oid_string(new_oid);
      }
    }
  }
  return description;
}

OperationState parse_operation_state(std::string_view text,
                                     git_oid_t oid_type) {
  std::istringstream input(std::string{text});
  std::string line;
  if (!std::getline(input, line) ||
      (line != kOperationV2 && line != kOperationV4)) {  // GG_COV_EXCL_BRANCH
    throw GitError("invalid gg operation snapshot");
  }
  OperationState state;
  std::string previous;
  std::string keyword;
  if (!(input >> keyword >> previous) || keyword != "previous") {
    throw GitError("invalid gg operation predecessor");
  }
  constexpr std::string_view description_prefix = "description ";
  if (!std::getline(input >> std::ws, line) ||
      !starts_with(line, description_prefix) ||
      line.size() == description_prefix.size()) {
    throw GitError("invalid gg operation description");
  }
  char kind = '\0';
  if (!(input >> keyword >> kind >> state.head.value) || keyword != "head" ||  // GG_COV_EXCL_BRANCH
      (kind != 'S' && kind != 'D')) {
    throw GitError("invalid gg operation HEAD");
  }
  state.head.symbolic = kind == 'S';
  while (input >> keyword) {
    if (keyword != "ref") {
      throw GitError("invalid gg operation reference");
    }
    std::string oid_text;
    std::string name;
    if (!(input >> oid_text >> name)) {
      throw GitError("invalid gg operation reference");
    }
    git_oid target{};
    check(git_oid_fromstr(&target, oid_text.c_str(), oid_type),
          "parse operation reference");
    if (starts_with(name, kLegacyBranchTrackingPrefix)) {
      name = std::string(kBranchTrackingPrefix) +
             name.substr(kLegacyBranchTrackingPrefix.size());
    }
    state.refs.emplace(name, target);
  }
  std::erase_if(state.refs, [](const auto& item) {
    return starts_with(item.first, kLegacyChangePrefix);
  });
  return state;
}

std::string operation_metadata(std::optional<git_oid> previous,
                               std::string_view description,
                               std::string_view workspace_name) {
  std::ostringstream output;
  output << kOperationV4 << "\nprevious "
         << (previous.has_value() ? oid_string(*previous) : "-")
         << "\ndescription " << description << "\nworkspace "
         << workspace_name << '\n';
  return output.str();
}

}  // namespace

OperationState Repository::state() const {
  auto refs = data_refs();
  std::erase_if(refs, [](const auto& item) {
    return starts_with(item.first, kAliasPrefix) || item.first == kAliasMapRef;
  });
  return {head_state(), std::move(refs), workspace_name()};
}

std::string Repository::serialize(const OperationState& state,
                      std::optional<git_oid> previous,
                      std::string_view description) const {
  std::ostringstream output;
  output << kOperationV2 << "\nprevious "
         << (previous.has_value() ? oid_string(*previous) : "-")
         << "\ndescription " << description << "\nhead "
         << (state.head.symbolic ? 'S' : 'D')
         << ' ' << state.head.value << '\n';
  for (const auto& [name, oid] : state.refs) {
    output << "ref " << oid_string(oid) << ' ' << name << '\n';
  }
  return output.str();
}

OperationState Repository::parse_operation(const git_oid& oid) const {
  CommitPtr operation = commit(oid);
  return parse_operation(operation.get());
}

OperationState Repository::parse_operation(const git_commit* operation) const {
  const std::string_view message = git_commit_message(operation);
  if (message.starts_with(kOperationV2)) {
    return parse_operation_state(message, git_repository_oid_type(repo_.get()));
  }
  if (!message.starts_with(kOperationV3) && !message.starts_with(kOperationV4)) {
    throw GitError("invalid gg operation snapshot");
  }
  git_tree* raw_tree = nullptr;
  check(git_commit_tree(&raw_tree, operation), "read operation state tree");
  TreePtr tree(raw_tree);
  git_tree_entry* raw_entry = nullptr;
  check(git_tree_entry_bypath(&raw_entry, tree.get(), "state"),
        "read operation state");
  TreeEntryPtr entry(raw_entry);
  if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB) {  // GG_COV_EXCL_BRANCH
    throw GitError("invalid gg operation state");
  }
  git_blob* raw_blob = nullptr;
  check(git_blob_lookup(&raw_blob, repo_.get(), git_tree_entry_id(entry.get())),
        "read operation state");
  BlobPtr blob(raw_blob);
  OperationState state = parse_operation_state(
      std::string_view(static_cast<const char*>(git_blob_rawcontent(blob.get())),
                       git_blob_rawsize(blob.get())),
      git_repository_oid_type(repo_.get()));
  std::istringstream metadata{std::string(message)};
  std::string line;
  for (int index = 0; index < 3; ++index) {
    (void)std::getline(metadata, line);
  }
  constexpr std::string_view workspace_prefix = "workspace ";
  if (std::getline(metadata, line) &&  // GG_COV_EXCL_BRANCH
      starts_with(line, workspace_prefix)) {  // GG_COV_EXCL_BRANCH
    state.workspace_name = line.substr(workspace_prefix.size());
  }
  return state;
}

std::string Repository::operation_description(const git_oid& oid) const {
  CommitPtr operation = commit(oid);
  return operation_description(operation.get());
}

std::string Repository::operation_description(
    const git_commit* operation) const {
  std::istringstream input(git_commit_message(operation));
  std::string line;
  if (!std::getline(input, line) ||
      (line != kOperationV2 && line != kOperationV3 && line != kOperationV4)) {
    throw GitError("invalid gg operation snapshot");
  }
  if (!std::getline(input, line) || !starts_with(line, "previous ")) {
    throw GitError("invalid gg operation description");
  }
  if (!std::getline(input, line) || !starts_with(line, "description ") ||
      line.size() == std::string_view("description ").size()) {
    throw GitError("invalid gg operation description");
  }
  return line.substr(std::string_view("description ").size());
}

std::optional<git_oid> Repository::operation_target(
    const git_oid& oid, std::string_view prefix) const {
  const std::string description = operation_description(oid);
  if (!starts_with(description, prefix)) {
    return std::nullopt;
  }
  git_oid target{};
  const std::string text(description.substr(prefix.size()));
  check(git_oid_fromstr(&target, text.c_str(),
                        git_repository_oid_type(repo_.get())),
        "parse restored operation");
  return target;
}

std::optional<git_oid> Repository::operation_previous(const git_oid& oid) const {
  CommitPtr operation = commit(oid);
  return operation_previous(operation.get());
}

std::optional<git_oid> Repository::operation_previous(
    const git_commit* operation) const {
  std::istringstream input(git_commit_message(operation));
  std::string header;
  std::string keyword;
  std::string previous;
  if (!(input >> header >> keyword >> previous) ||
      (header != kOperationV2 && header != kOperationV3 &&
       header != kOperationV4) ||  // GG_COV_EXCL_BRANCH
      keyword != "previous") {
    throw GitError("invalid gg operation predecessor");
  }
  if (previous == "-") {
    return std::nullopt;
  }
  git_oid result{};
  check(git_oid_fromstr(&result, previous.c_str(),
                        git_repository_oid_type(repo_.get())),
        "parse operation predecessor");
  return result;
}

git_oid Repository::create_operation(const OperationState& state,
                         std::optional<git_oid> previous,
                         std::string_view description) const {
  if (description.empty()) {
    throw GitError("operation description is empty");
  }
  std::vector<git_oid> parents;
  std::set<git_oid, OidLess> seen;
  std::optional<OperationState> previous_state;
  if (previous.has_value()) {
    parents.push_back(*previous);
    seen.insert(*previous);
    CommitPtr previous_commit = commit(*previous);
    if (std::string_view(git_commit_message(previous_commit.get()))
            .starts_with(kOperationV4)) {
      previous_state = parse_operation(previous_commit.get());
    }
  }
  std::vector<git_oid> displaced_targets;
  if (previous_state.has_value()) {
    for (const auto& [name, target] : previous_state->refs) {
      const auto replacement = state.refs.find(name);
      if (replacement == state.refs.end() || !(replacement->second == target)) {
        displaced_targets.push_back(target);
      }
    }
  }
  for (const auto& [name, target] : state.refs) {
    const auto physical = ref_target(name);
    if (!physical.has_value() || !(*physical == target)) {
      displaced_targets.push_back(target);
    }
  }
  // A detached HEAD can be the only reference to a revision. Retain its old
  // and replacement targets just like displaced refs so operation restoration
  // remains possible after reflog expiration and garbage collection.
  const auto retain_detached_head = [&](const HeadState& head) {
    if (head.symbolic || head.value.empty()) return;
    git_oid target{};
    check(git_oid_fromstr(&target, head.value.c_str(),
                          git_repository_oid_type(repo_.get())),
          "parse operation HEAD");
    displaced_targets.push_back(target);
  };
  if (!previous_state.has_value() ||
      previous_state->head.symbolic != state.head.symbolic ||
      previous_state->head.value != state.head.value) {
    retain_detached_head(state.head);
    if (previous_state.has_value()) retain_detached_head(previous_state->head);
  }
  for (const git_oid& target : displaced_targets) {
    git_commit* raw_commit = nullptr;
    if (!seen.contains(target) &&
        git_commit_lookup(&raw_commit, repo_.get(), &target) == 0) {
      CommitPtr target_commit(raw_commit);
      parents.push_back(target);
      seen.insert(target);
    } else {
      git_error_clear();
    }
  }
  if (parents.size() > kMaxOperationParents) {
    const auto first_target = parents.begin() + (previous.has_value() ? 1 : 0);
    const git_oid keepalive =
        create_keepalive(*this, {first_target, parents.end()});
    parents.erase(first_target, parents.end());
    parents.push_back(keepalive);
  }
  std::string serialized = serialize(state, previous, description);
  serialized.replace(0, kOperationV2.size(), kOperationV4);
  git_oid state_oid{};
  check(git_blob_create_from_buffer(&state_oid, repo_.get(), serialized.data(),
                                    serialized.size()),
        "write operation state");
  git_treebuilder* raw_builder = nullptr;
  check(git_treebuilder_new(&raw_builder, repo_.get(), nullptr),
        "create operation state tree");
  GitPtr<git_treebuilder, git_treebuilder_free> builder(raw_builder);
  check(git_treebuilder_insert(nullptr, builder.get(), "state", &state_oid,
                               GIT_FILEMODE_BLOB),
        "add operation state");
  git_oid tree_oid{};
  check(git_treebuilder_write(&tree_oid, builder.get()),
        "write operation state tree");
  return create_commit(tree_oid, parents,
                       operation_metadata(previous, description,
                                          state.workspace_name));
}

void Repository::migrate_operation_history() const {
  std::vector<git_oid> history;
  std::set<git_oid, OidLess> seen;
  bool migration_needed = false;
  auto current = operation();
  while (current.has_value()) {
    if (!seen.insert(*current).second) {
      throw GitError("operation history contains a cycle");
    }
    CommitPtr value = commit(*current);
    migration_needed |=
        git_commit_parentcount(value.get()) > kMaxOperationParents;
    history.push_back(*current);
    current = operation_previous(value.get());
  }
  if (!migration_needed) return;

  std::optional<git_oid> rewritten;
  std::map<git_oid, git_oid, OidLess> rewritten_oids;
  for (auto iterator = history.rbegin(); iterator != history.rend();
       ++iterator) {
    const std::string description = migrate_operation_description(
        operation_description(*iterator), rewritten_oids);
    rewritten = create_operation(parse_operation(*iterator), rewritten,
                                 description);
    rewritten_oids.emplace(*iterator, *rewritten);
  }
  apply_refs({{operation_ref_name(), *rewritten}}, {},
             "gg migrate operation history");
  const int reflog = git_reflog_delete(repo_.get(),
                                       operation_ref_name().c_str());
  if (reflog != 0 && reflog != GIT_ENOTFOUND) {
    check(reflog, "delete legacy operation log");
  }
}

std::optional<git_oid> Repository::operation() const {
  return ref_target(operation_ref_name());
}

git_oid Repository::resolve_operation(std::string_view expression) const {
  if (expression.empty()) {
    throw UserError("operation ID is empty");
  }
  auto current = operation();
  if (!current.has_value()) {
    throw UserError("no operations");
  }
  if (expression == "@" || starts_with(expression, "@-")) {
    for (std::size_t index = 1; index < expression.size(); ++index) {
      if (expression[index] != '-') {
        throw UserError("invalid operation: " + std::string(expression));
      }
      current = operation_previous(*current);
      if (!current.has_value()) {
        throw UserError("operation has no predecessor: " +
                        std::string(expression));
      }
    }
    return *current;
  }

  std::optional<git_oid> match;
  while (current.has_value()) {
    if (starts_with(oid_string(*current), expression)) {
      if (match.has_value()) {
        throw UserError("ambiguous operation ID: " + std::string(expression));
      }
      match = *current;
    }
    current = operation_previous(*current);
  }
  if (match.has_value()) return *match;
  git_object* raw_object = nullptr;
  const int resolved = git_revparse_single(
      &raw_object, repo_.get(), std::string(expression).c_str());
  if (resolved == 0) {
    ObjectPtr object(raw_object);
    const git_oid oid = *git_object_id(object.get());
    try {
      (void)parse_operation(oid);
      return oid;
    } catch (const GitError&) {  // GG_COV_EXCL_BRANCH
    }
  } else {
    git_error_clear();
  }
  throw UserError("operation not found: " + std::string(expression));
}

void Repository::view_at_operation(std::string_view expression) {
  const git_oid operation_oid = resolve_operation(expression);
  operation_view_ = parse_operation(operation_oid);
  if (!operation_view_->workspace_name.empty()) {
    workspace_name_ = operation_view_->workspace_name;
  }
  viewed_operation_ = operation_oid;
  ignore_working_copy_ = true;
}

git_oid Repository::ensure_operation() const {
  const auto current = operation();
  if (current.has_value()) {
    CommitPtr current_commit = commit(*current);
    if (std::string_view(git_commit_message(current_commit.get()))
            .starts_with(kOperationV4)) {
      const OperationState recorded = parse_operation(current_commit.get());
      const OperationState actual = state();
      if (recorded.head.symbolic == actual.head.symbolic &&
          recorded.head.value == actual.head.value &&
          refs_equal(recorded.refs, actual.refs)) {
        return *current;
      }
    }
  }
  std::optional<git_oid> predecessor;
  if (current.has_value()) {
    CommitPtr current_commit = commit(*current);
    // A legacy repository transitions directly to V4 while retaining its
    // existing operation as the restoration predecessor. A mismatch against
    // an existing V4 operation, however, represents state changed outside
    // this worktree (for example another linked workspace). Establish a fresh
    // baseline so undo in this worktree cannot roll back that external state.
    if (!std::string_view(git_commit_message(current_commit.get()))
             .starts_with(kOperationV4)) {
      predecessor = current;
    }
  }
  const git_oid synchronized = create_operation(
      state(), predecessor,
      current.has_value() ? "synchronize workspace" : "initialize repository");
  apply_refs({{operation_ref_name(), synchronized}}, {},
             "gg synchronize workspace");
  return synchronized;
}

void Repository::record(std::map<std::string, git_oid> updates,
            std::set<std::string> deletes,
            const HeadState& head,
            std::string_view description,
            bool manage_workspaces) const {
  if (!manage_workspaces) {
    const std::string current_workspace = workspace_ref_name();
    for (const auto& [name, target] : updates) {
      if (!starts_with(name, kWorkspacePrefix) || name == current_workspace) {
        continue;
      }
      const auto existing = ref_target(name);
      if (!existing.has_value() || !(*existing == target)) {
        throw UserError("operation would rewrite active workspace: " +
                        name.substr(kWorkspacePrefix.size()));
      }
    }
    for (const std::string& name : deletes) {
      if (starts_with(name, kWorkspacePrefix) && name != current_workspace &&
          ref_target(name).has_value()) {
        throw UserError("operation would remove active workspace: " +
                        name.substr(kWorkspacePrefix.size()));
      }
    }
  }
  OperationState next = state();
  next.head = head;
  for (const std::string& name : deletes) {
    if (starts_with(name, kAliasPrefix) || name == kAliasMapRef) continue;
    next.refs.erase(name);
  }
  for (const auto& [name, oid] : updates) {
    if (starts_with(name, kAliasPrefix) || name == kAliasMapRef ||
        starts_with(name, "refs/gg/operations/")) {
      continue;
    }
    next.refs[name] = oid;
  }
  const git_oid operation_oid =
      create_operation(next, ensure_operation(), description);
  updates[operation_ref_name()] = operation_oid;
  updates.insert(pending_conflict_refs_.begin(), pending_conflict_refs_.end());
  apply_refs(updates, deletes, description);
  pending_conflict_refs_.clear();
}

void Repository::restore_operation(const git_oid& operation_oid,
                                   std::string_view description,
                                   bool restore_repository,
                                   bool restore_remote_tracking,
                                   bool rollback_on_failure,
                                   const std::map<std::string, git_oid>* rollback_aliases) const {
  const OperationState source = parse_operation(operation_oid);
  OperationState target = state();
  const HeadState previous_head = target.head;
  std::optional<git_oid> previous_checkout = workspace();
  if (!previous_checkout.has_value()) previous_checkout = head_oid();
  if (restore_repository && rollback_on_failure) {
    const git_oid baseline_tree = previous_checkout.has_value()
                                      ? *git_commit_tree_id(commit(*previous_checkout).get())
                                      : empty_tree();
    if (worktree_tracked_dirty(baseline_tree)) {
      std::optional<git_oid> restored_checkout;
      if (const auto found = source.refs.find(workspace_ref_name());
          found != source.refs.end()) {
        restored_checkout = found->second;
      } else if (source.head.symbolic) {
        if (const auto found = source.refs.find(source.head.value);
            found != source.refs.end()) {
          restored_checkout = found->second;
        }
      } else {
        git_oid parsed{};
        check(git_oid_fromstr(&parsed, source.head.value.c_str(),
                              git_repository_oid_type(repo_.get())),
              "parse restored HEAD");
        restored_checkout = parsed;
      }
      if (!restored_checkout.has_value() ||
          worktree_tracked_dirty(
              *git_commit_tree_id(commit(*restored_checkout).get()))) {
        throw UserError("working tree has uncommitted changes; commit or amend before restoring an operation");
      }
    }
  }
  const auto current_operation = operation();
  const bool manage_workspaces =
      starts_with(operation_description(operation_oid), "gg workspace ") ||
      (current_operation.has_value() &&
       starts_with(operation_description(*current_operation),  // GG_COV_EXCL_BRANCH
                   "gg workspace "));
  if (restore_repository) {
    target.head = source.head;
  }
  const std::string current_workspace = workspace_ref_name();
  std::set<std::string> other_workspaces;
  for (const auto& [name, root] : workspace_roots()) {
    (void)root;
    if (name != workspace_name()) {
      other_workspaces.insert(std::string(kWorkspacePrefix) + name);
    }
  }
  std::optional<std::string> restored_workspace_name;
  const auto current_target = target.refs.find(current_workspace);
  if (restore_repository && !source.workspace_name.empty() &&
      source.workspace_name != workspace_name()) {
    restored_workspace_name = source.workspace_name;
  } else if (restore_repository && source.workspace_name.empty() &&
             manage_workspaces &&  // GG_COV_EXCL_BRANCH
             current_target != target.refs.end() &&  // GG_COV_EXCL_BRANCH
             !source.refs.contains(current_workspace)) {  // GG_COV_EXCL_BRANCH
    for (const auto& [name, oid] : source.refs) {  // GG_COV_EXCL_BRANCH
      if (starts_with(name, kWorkspacePrefix) &&
          !target.refs.contains(name) &&  // GG_COV_EXCL_BRANCH
          !other_workspaces.contains(name) &&  // GG_COV_EXCL_BRANCH
          oid == current_target->second) {  // GG_COV_EXCL_BRANCH
        restored_workspace_name = name.substr(kWorkspacePrefix.size());
        break;
      }
    }
  }
  if (restored_workspace_name.has_value()) {
    target.workspace_name = *restored_workspace_name;
  }
  for (auto iterator = target.refs.begin(); iterator != target.refs.end();) {
    if (!manage_workspaces && other_workspaces.contains(iterator->first)) {
      ++iterator;
      continue;
    }
    const bool remote = starts_with(iterator->first, "refs/remotes/") ||
                        starts_with(iterator->first, kRemoteTagPrefix) ||
                        starts_with(iterator->first, kBranchTrackingPrefix) ||
                        starts_with(iterator->first, kTagTrackingPrefix);
    if ((remote && restore_remote_tracking) ||
        (!remote && restore_repository)) {
      iterator = target.refs.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (const auto& [name, oid] : source.refs) {
    if (starts_with(name, kAliasPrefix) || name == kAliasMapRef) continue;
    if (!manage_workspaces && other_workspaces.contains(name)) {
      continue;
    }
    const bool remote = starts_with(name, "refs/remotes/") ||
                        starts_with(name, kRemoteTagPrefix) ||
                        starts_with(name, kBranchTrackingPrefix) ||
                        starts_with(name, kTagTrackingPrefix);
    if ((remote && restore_remote_tracking) ||
        (!remote && restore_repository)) {
      target.refs[name] = oid;
    }
  }
  const auto current = data_refs();
  std::map<std::string, git_oid> updates = target.refs;
  updates[operation_ref_name()] =
      description.empty()
          ? operation_oid
          : create_operation(target, ensure_operation(), description);
  // V4 snapshots omit aliases, so normal restoration clears the overlay:
  // aliases for undone rewrites must not redirect revisions in restored history.
  // Failure recovery instead restores the exact overlay captured by its caller.
  if (rollback_aliases != nullptr) {
    for (const auto& [name, oid] : *rollback_aliases) {
      if (starts_with(name, kAliasPrefix) || name == kAliasMapRef) {
        updates[name] = oid;
      }
    }
  }
  std::set<std::string> deletes;
  for (const auto& [name, oid] : current) {
    (void)oid;
    if (!updates.contains(name)) {
      deletes.insert(name);
    }
  }
  const std::string previous_workspace_name = workspace_name();
  if (restored_workspace_name.has_value()) {
    set_workspace_name(*restored_workspace_name);
  }
  bool refs_applied = false;
  try {
    apply_refs(updates, deletes,
               description.empty() ? "gg restore operation" : description);
    refs_applied = true;
    if (restore_repository) {
      set_head(target.head);
      std::optional<git_oid> target_checkout = workspace();
      if (!target_checkout.has_value()) target_checkout = head_oid();
      checkout(target_checkout);
    }
  } catch (const std::exception& error) {
    const std::string original = error.what();
    if (restored_workspace_name.has_value() && (!refs_applied || rollback_on_failure)) {
      set_workspace_name(previous_workspace_name);
    }
    if (refs_applied && rollback_on_failure) {
      try {
        std::map<std::string, git_oid> rollback_updates = current;
        std::set<std::string> rollback_deletes;
        for (const auto& [name, oid] : data_refs()) {
          (void)oid;
          if (!current.contains(name)) rollback_deletes.insert(name);
        }
        if (current_operation.has_value()) {
          rollback_updates[operation_ref_name()] = *current_operation;
        } else {
          rollback_deletes.insert(operation_ref_name());
        }
        apply_refs(rollback_updates, rollback_deletes, "gg restore failed operation");
        if (restore_repository) {
          set_head(previous_head);
          checkout(previous_checkout);
        }
      } catch (const std::exception& recovery) {
        clear_checkout_recovery();
        throw UserError(original + "; could not restore the previous repository state: " +
                        recovery.what());
      }
    }
    clear_checkout_recovery();
    throw;
  }
}

}  // namespace gg::detail
