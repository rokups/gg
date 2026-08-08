// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <sstream>

namespace gg::detail {
OperationState Repository::state() const { return {head_state(), data_refs()}; }

std::string Repository::serialize(const OperationState& state,
                      std::optional<git_oid> previous) const {
  std::ostringstream output;
  output << "gg-operation-v1\nprevious "
         << (previous.has_value() ? oid_string(*previous) : "-") << "\nhead "
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
  if (!std::getline(input, line) || line != "gg-operation-v1") {
    throw GitError("invalid gg operation snapshot");
  }
  OperationState state;
  std::string previous;
  std::string keyword;
  if (!(input >> keyword >> previous) || keyword != "previous") {
    throw GitError("invalid gg operation predecessor");
  }
  char kind = '\0';
  if (!(input >> keyword >> kind >> state.head.value) || keyword != "head" ||  // GG_COV_EXCL_BRANCH
      (kind != 'S' && kind != 'D')) {
    throw GitError("invalid gg operation HEAD");
  }
  state.head.symbolic = kind == 'S';
  std::string oid_text;
  std::string name;
  while (input >> keyword >> oid_text >> name) {
    if (keyword != "ref") {
      throw GitError("invalid gg operation reference");
    }
    git_oid target{};
    check(git_oid_fromstr(&target, oid_text.c_str()), "parse operation reference");
    state.refs.emplace(name, target);
  }
  return state;
}

std::optional<git_oid> Repository::operation_previous(const git_oid& oid) const {
  CommitPtr operation = commit(oid);
  std::istringstream input(git_commit_message(operation.get()));
  std::string header;
  std::string keyword;
  std::string previous;
  if (!(input >> header >> keyword >> previous) || header != "gg-operation-v1" ||  // GG_COV_EXCL_BRANCH
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
                         std::optional<git_oid> previous) const {
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
  return create_commit(empty_tree(), parents, serialize(state, previous));
}

std::optional<git_oid> Repository::operation() const { return ref_target(kOperationRef); }

git_oid Repository::ensure_operation() const {
  const auto current = operation();
  if (current.has_value()) {
    return *current;
  }
  return create_operation(state(), std::nullopt);
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
  const git_oid operation_oid = create_operation(next, ensure_operation());
  updates[std::string(kOperationRef)] = operation_oid;
  apply_refs(updates, deletes, description);
}

void Repository::restore_operation(const git_oid& operation_oid) const {
  const OperationState target = parse_operation(operation_oid);
  const auto current = data_refs();
  std::map<std::string, git_oid> updates = target.refs;
  updates[std::string(kOperationRef)] = operation_oid;
  std::set<std::string> deletes;
  for (const auto& [name, oid] : current) {
    (void)oid;
    if (!target.refs.contains(name)) {
      deletes.insert(name);
    }
  }
  apply_refs(updates, deletes, "gg undo");
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

}  // namespace gg::detail
