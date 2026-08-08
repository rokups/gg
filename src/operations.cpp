// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <sstream>

namespace gg::detail {
OperationState Repository::state() const { return {head_state(), data_refs()}; }

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
  std::istringstream input(git_commit_message(operation.get()));
  std::string line;
  if (!std::getline(input, line) || line != "gg-operation-v2") {
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
    check(git_oid_fromstr(&target, oid_text.c_str()), "parse operation reference");
    state.refs.emplace(name, target);
  }
  return state;
}

std::string Repository::operation_description(const git_oid& oid) const {
  CommitPtr operation = commit(oid);
  std::istringstream input(git_commit_message(operation.get()));
  std::string line;
  if (!std::getline(input, line) || line != "gg-operation-v2") {
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
  std::istringstream input(git_commit_message(operation.get()));
  std::string header;
  std::string keyword;
  std::string previous;
  if (!(input >> header >> keyword >> previous) || header != "gg-operation-v2" ||  // GG_COV_EXCL_BRANCH
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
  if (previous.has_value()) {
    parents.push_back(*previous);
    seen.insert(*previous);
  }
  for (const auto& [name, target] : state.refs) {
    (void)name;
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
  return create_commit(empty_tree(), parents,
                       serialize(state, previous, description));
}

std::optional<git_oid> Repository::operation() const { return ref_target(kOperationRef); }

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
  if (!match.has_value()) {
    throw UserError("operation not found: " + std::string(expression));
  }
  return *match;
}

git_oid Repository::ensure_operation() const {
  const auto current = operation();
  if (current.has_value()) {
    return *current;
  }
  return create_operation(state(), std::nullopt, "initialize repository");
}

void Repository::record(std::map<std::string, git_oid> updates,
            std::set<std::string> deletes,
            const HeadState& head,
            std::string_view description) const {
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
  updates[std::string(kOperationRef)] = operation_oid;
  apply_refs(updates, deletes, description);
}

void Repository::restore_operation(const git_oid& operation_oid,
                                   std::string_view description,
                                   bool restore_repository,
                                   bool restore_remote_tracking) const {
  const OperationState source = parse_operation(operation_oid);
  OperationState target = state();
  if (restore_repository) {
    target.head = source.head;
  }
  for (auto iterator = target.refs.begin(); iterator != target.refs.end();) {
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
  updates[std::string(kOperationRef)] =
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
  apply_refs(updates, deletes,
             description.empty() ? "gg restore operation" : description);
  if (restore_repository) {
    set_head(target.head);
    const auto workspace = ref_target(kWorkspaceRef);
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
