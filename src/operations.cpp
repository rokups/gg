// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

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

OperationState parse_operation_state(std::string_view text) {
  std::istringstream input(std::string{text});
  std::string line;
  if (!std::getline(input, line) || line != kOperationV2) {  // GG_COV_EXCL_BRANCH
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
    check(git_oid_fromstr(&target, oid_text.c_str()),
          "parse operation reference");
    state.refs.emplace(name, target);
  }
  return state;
}

std::string operation_metadata(std::optional<git_oid> previous,
                               std::string_view description,
                               std::string_view workspace_name) {
  std::ostringstream output;
  output << kOperationV3 << "\nprevious "
         << (previous.has_value() ? oid_string(*previous) : "-")
         << "\ndescription " << description << "\nworkspace "
         << workspace_name << '\n';
  return output.str();
}

}  // namespace

OperationState Repository::state() const {
  return {head_state(), data_refs(), workspace_name()};
}

std::string Repository::serialize(const OperationState& state,
                      std::optional<git_oid> previous,
                      std::string_view description) const {
  std::ostringstream output;
  output << "gg-operation-v2\nprevious "
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
    return parse_operation_state(message);
  }
  if (!message.starts_with(kOperationV3)) {
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
                       git_blob_rawsize(blob.get())));
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
      (line != kOperationV2 && line != kOperationV3)) {
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
  check(git_oid_fromstr(&target, text.c_str()), "parse restored operation");
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
      (header != kOperationV2 && header != kOperationV3) ||  // GG_COV_EXCL_BRANCH
      keyword != "previous") {
    throw GitError("invalid gg operation predecessor");
  }
  if (previous == "-") {
    return std::nullopt;
  }
  git_oid result{};
  check(git_oid_fromstr(&result, previous.c_str()), "parse operation predecessor");
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
    previous_state = parse_operation(*previous);
  }
  for (const auto& [name, target] : state.refs) {
    const auto old = previous_state.has_value()
                         ? previous_state->refs.find(name)
                         : state.refs.end();
    if (previous_state.has_value() && old != previous_state->refs.end() &&
        old->second == target) {
      continue;
    }
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
  const std::string serialized = serialize(state, previous, description);
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
    const OperationState recorded = parse_operation(*current);
    const OperationState actual = state();
    if (recorded.head.symbolic == actual.head.symbolic &&
        recorded.head.value == actual.head.value &&
        refs_equal(recorded.refs, actual.refs)) {
      return *current;
    }
  }
  const git_oid synchronized = create_operation(
      state(), std::nullopt,
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
    next.refs.erase(name);
  }
  for (const auto& [name, oid] : updates) {
    next.refs[name] = oid;
  }
  const git_oid operation_oid =
      create_operation(next, ensure_operation(), description);
  updates[operation_ref_name()] = operation_oid;
  apply_refs(updates, deletes, description);
}

void Repository::restore_operation(const git_oid& operation_oid,
                                   std::string_view description,
                                   bool restore_repository,
                                   bool restore_remote_tracking) const {
  const OperationState source = parse_operation(operation_oid);
  OperationState target = state();
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
                        starts_with(iterator->first, kBookmarkTrackingPrefix) ||
                        starts_with(iterator->first, kTagTrackingPrefix);
    if ((remote && restore_remote_tracking) ||
        (!remote && restore_repository)) {
      iterator = target.refs.erase(iterator);
    } else {
      ++iterator;
    }
  }
  for (const auto& [name, oid] : source.refs) {
    if (!manage_workspaces && other_workspaces.contains(name)) {
      continue;
    }
    const bool remote = starts_with(name, "refs/remotes/") ||
                        starts_with(name, kRemoteTagPrefix) ||
                        starts_with(name, kBookmarkTrackingPrefix) ||
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
  std::set<std::string> deletes;
  for (const auto& [name, oid] : current) {
    (void)oid;
    if (!target.refs.contains(name)) {
      deletes.insert(name);
    }
  }
  const std::string previous_workspace_name = workspace_name();
  if (restored_workspace_name.has_value()) {
    set_workspace_name(*restored_workspace_name);
  }
  try {
    apply_refs(updates, deletes,
               description.empty() ? "gg restore operation" : description);
  } catch (...) {
    if (restored_workspace_name.has_value()) {
      set_workspace_name(previous_workspace_name);
    }
    throw;
  }
  if (restore_repository) {
    set_head(target.head);
    const auto workspace = this->workspace();
    if (workspace.has_value()) {
      checkout(*workspace);
    } else {
      const auto head = head_oid();
      if (head.has_value()) {
        checkout(*head);
      }
    }
  }
}

}  // namespace gg::detail
