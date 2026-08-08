// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <functional>
#include <random>

namespace gg::detail {
namespace {

std::string random_change_id() {
  constexpr std::string_view alphabet = "zyxwvutsrqponmlk";
  std::random_device random;
  std::string result;
  result.reserve(32);
  for (int index = 0; index < 16; ++index) {
    const unsigned int byte = random() & 0xffU;
    result.push_back(alphabet[byte >> 4U]);
    result.push_back(alphabet[byte & 0xfU]);
  }
  return result;
}

bool valid_change_id(std::string_view id) {
  return id.size() == 32 &&
         id.find_first_not_of("zyxwvutsrqponmlk") == std::string_view::npos;
}

std::string_view trim(std::string_view value) {
  const std::size_t begin = value.find_first_not_of(" \t\n\r");
  if (begin == std::string_view::npos) return {};
  return value.substr(begin, value.find_last_not_of(" \t\n\r") - begin + 1);
}

std::string_view unquote(std::string_view value) {
  value = trim(value);
  if (value.size() >= 2 &&
      ((value.front() == '\'' && value.back() == '\'') ||  // GG_COV_EXCL_BRANCH
       (value.front() == '"' && value.back() == '"'))) {  // GG_COV_EXCL_BRANCH
    return value.substr(1, value.size() - 2);
  }
  return value;
}

std::size_t top_level_operator(std::string_view expression,
                               std::string_view operation) {
  int depth = 0;
  char quote = '\0';
  std::size_t match = std::string_view::npos;
  for (std::size_t index = 0; index + operation.size() <= expression.size();
       ++index) {
    const char character = expression[index];
    if (quote != '\0') {
      if (character == quote) quote = '\0';
      continue;
    }
    if (character == '\'' || character == '"') {  // GG_COV_EXCL_BRANCH
      quote = character;
    } else if (character == '(') {
      ++depth;
    } else if (character == ')') {
      --depth;
    } else if (depth == 0 && expression.substr(index, operation.size()) ==
                                      operation) {
      if (operation == "~" &&
          (index + 1 == expression.size() ||
           (expression[index + 1] >= '0' && expression[index + 1] <= '9'))) {
        continue;
      }
      match = index;
    }
  }
  return match;
}

bool outer_parentheses(std::string_view expression) {
  if (expression.size() < 2 || expression.front() != '(' ||
      expression.back() != ')') {
    return false;
  }
  int depth = 0;
  char quote = '\0';
  for (std::size_t index = 0; index < expression.size(); ++index) {
    const char character = expression[index];
    if (quote != '\0') {
      if (character == quote) quote = '\0';
      continue;
    }
    if (character == '\'' || character == '"') {  // GG_COV_EXCL_BRANCH
      quote = character;
    } else if (character == '(') {
      ++depth;
    } else if (character == ')' && --depth == 0) {
      return index + 1 == expression.size();
    }
  }
  return false;
}

void append_unique(std::vector<git_oid>& target,
                   const std::vector<git_oid>& values) {
  std::set<git_oid, OidLess> seen(target.begin(), target.end());
  for (const git_oid& value : values) {
    if (seen.insert(value).second) target.push_back(value);
  }
}

std::size_t unique_prefix_length(
    std::string_view value,
    const std::vector<std::string_view>& others) {
  std::size_t length = std::min<std::size_t>(1, value.size());
  for (const std::string_view other : others) {
    if (other == value) continue;
    std::size_t common = 0;
    while (common < value.size() && common < other.size() &&
           value[common] == other[common]) {
      ++common;
    }
    length = std::max(length, std::min(value.size(), common + 1));
  }
  return length;
}

}  // namespace

const std::map<std::string, git_oid>& Repository::changes() const {
  if (!ref_cache_enabled_) changes_cache_.reset();
  if (changes_cache_.has_value()) return *changes_cache_;
  changes_cache_.emplace();
  auto& result = *changes_cache_;
  for (const auto& [name, oid] : data_refs()) {
    if (starts_with(name, kChangePrefix)) {
      result.emplace(name.substr(kChangePrefix.size()), oid);
    }
  }
  return result;
}

std::string Repository::new_change_id() const {
  std::string result;
  do {
    result = random_change_id();
  } while (changes().contains(result));  // GG_COV_EXCL_BRANCH
  return result;
}

std::map<std::string, git_oid> Repository::missing_change_ids() const {
  std::set<std::string> ids;
  std::set<git_oid, OidLess> assigned;
  for (const auto& [id, oid] : changes()) {
    if (!valid_change_id(id)) continue;
    ids.insert(id);
    assigned.insert(oid);
  }

  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "create change ID walk");
  RevwalkPtr walk(raw_walk);
  const auto push = [&](std::string_view reference, const git_oid& oid) {
    git_object* raw_object = nullptr;
    check(git_object_lookup(&raw_object, repo_.get(), &oid, GIT_OBJECT_ANY),
          "read change ID root " + std::string(reference));
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    if (git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT) < 0) {
      git_error_clear();
      return;
    }
    ObjectPtr commit(raw_commit);
    check(git_revwalk_push(walk.get(), git_object_id(commit.get())),
          "walk change ID roots");
  };
  for (const auto& [reference, oid] : data_refs()) {
    push(reference, oid);
  }
  if (const auto head = head_oid(); head.has_value()) push("HEAD", *head);

  std::map<std::string, git_oid> updates;
  while (true) {
    git_oid oid{};
    const int result = git_revwalk_next(&oid, walk.get());
    if (result == GIT_ITEROVER) break;
    check(result, "walk commits for change IDs");
    if (assigned.contains(oid)) continue;
    std::string id;
    do {
      id = random_change_id();
    } while (ids.contains(id));  // GG_COV_EXCL_BRANCH
    ids.insert(id);
    assigned.insert(oid);
    updates.emplace(std::string(kChangePrefix) + id, oid);
  }
  return updates;
}

std::set<std::string> Repository::invalid_change_id_refs() const {
  std::set<std::string> refs;
  for (const auto& [id, oid] : changes()) {
    (void)oid;
    if (!valid_change_id(id)) {
      refs.insert(std::string(kChangePrefix) + id);
    }
  }
  return refs;
}

void Repository::import_git_history(std::ostream* progress) const {
  const bool initializing = !operation().has_value();
  if (initializing && progress != nullptr && head_oid().has_value()) {
    *progress << "Initializing gg for this repository; this may take a moment...\n";
  }
  std::set<std::string> deletes = invalid_change_id_refs();
  if (!initializing && deletes.empty()) return;
  std::map<std::string, git_oid> updates = missing_change_ids();
  if (updates.empty() && deletes.empty()) return;
  record(std::move(updates), std::move(deletes), head_state(),
         "gg import history");
}

std::string Repository::short_change_id(std::string_view id) const {
  return short_change_id_parts(id).value;
}

ShortId Repository::short_change_id_parts(std::string_view id) const {
  std::vector<std::string_view> ids;
  if (scoped_change_ids_.has_value()) {
    ids.reserve(scoped_change_ids_->size());
    for (const std::string& other : *scoped_change_ids_) {
      ids.push_back(other);
    }
  } else {
    const auto& values = changes();
    ids.reserve(values.size());
    for (const auto& [other, oid] : values) {
      (void)oid;
      ids.push_back(other);
    }
  }
  const std::size_t unique = unique_prefix_length(id, ids);
  return {std::string(id.substr(0, std::max<std::size_t>(8, unique))), unique};
}

ShortId Repository::short_commit_id(const git_oid& oid) const {
  const std::string value = oid_string(oid);
  std::vector<std::string> storage;
  if (scoped_commit_ids_.has_value()) {
    storage = *scoped_commit_ids_;
  } else {
    const auto& revisions = changes();
    storage.reserve(revisions.size());
    for (const auto& [id, target] : revisions) {
      (void)id;
      storage.push_back(oid_string(target));
    }
  }
  std::vector<std::string_view> ids(storage.begin(), storage.end());
  const std::size_t unique = unique_prefix_length(value, ids);
  return {value.substr(0, std::max<std::size_t>(8, unique)), unique};
}

void Repository::set_short_id_scope(std::span<const git_oid> revisions) {
  scoped_change_ids_.emplace();
  scoped_commit_ids_.emplace();
  scoped_change_ids_->reserve(revisions.size());
  scoped_commit_ids_->reserve(revisions.size());
  for (const git_oid& revision : revisions) {
    scoped_commit_ids_->push_back(oid_string(revision));
    if (const auto id = change_id(revision); id.has_value()) {
      scoped_change_ids_->push_back(*id);
    }
  }
}

std::optional<std::string> Repository::change_id(const git_oid& oid) const {
  if (!ref_cache_enabled_) change_ids_by_oid_cache_.reset();
  if (!change_ids_by_oid_cache_.has_value()) {
    change_ids_by_oid_cache_.emplace();
    for (const auto& [id, target] : changes()) {
      change_ids_by_oid_cache_->emplace(target, id);
    }
  }
  const auto found = change_ids_by_oid_cache_->find(oid);
  return found == change_ids_by_oid_cache_->end()
             ? std::nullopt
             : std::optional<std::string>{found->second};
}

git_oid Repository::resolve_atom(std::string_view revision) const {
  if (revision == "@" || starts_with(revision, "@-")) {
    const auto workspace = this->workspace();
    if (!workspace.has_value()) {
      throw UserError("no gg working-copy change; run `gg new` first");
    }
    git_oid current = *workspace;
    for (std::size_t index = 1; index < revision.size(); ++index) {
      if (revision[index] != '-') {
        throw UserError("invalid working-copy revision: " +
                        std::string(revision));
      }
      const auto current_parents = parents(current);
      if (current_parents.empty()) {
        throw UserError("revision has no parent: " + std::string(revision));
      }
      current = current_parents.front();
    }
    return current;
  }

  std::optional<git_oid> match;
  for (const auto& [id, oid] : changes()) {
    if (starts_with(id, revision)) {
      if (match.has_value() && !(*match == oid)) {
        throw UserError("ambiguous change ID: " + std::string(revision));
      }
      match = oid;
    }
  }
  if (match.has_value()) {
    return *match;
  }

  const std::string bookmark = "refs/heads/" + std::string(revision);
  int valid_bookmark = 0;
  check(git_reference_name_is_valid(&valid_bookmark, bookmark.c_str()),
        "validate bookmark reference");
  if (valid_bookmark != 0) {
    const auto bookmark_target = ref_target(bookmark);
    if (bookmark_target.has_value()) {
      return *bookmark_target;
    }
  }

  git_object* raw_object = nullptr;
  const int result = git_revparse_single(&raw_object, repo_.get(),
                                         std::string(revision).c_str());
  if (result < 0) {
    git_error_clear();
    throw UserError("revision not found: " + std::string(revision));
  }
  ObjectPtr object(raw_object);
  git_object* raw_commit = nullptr;
  check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
        "resolve revision");
  ObjectPtr commit_object(raw_commit);
  return *git_object_id(commit_object.get());
}

std::vector<git_oid> Repository::resolve_set(std::string_view revisions) const {
  using Selection = std::vector<git_oid>;
  const auto ordered_set = [](const Selection& values) {
    return std::set<git_oid, OidLess>(values.begin(), values.end());
  };
  const auto ancestors = [&](const Selection& seeds) {
    git_revwalk* raw_walk = nullptr;
    check(git_revwalk_new(&raw_walk, repo_.get()), "create revset walk");
    RevwalkPtr walk(raw_walk);
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    for (const git_oid& seed : seeds) {
      check(git_revwalk_push(walk.get(), &seed), "walk revset ancestors");
    }
    Selection result;
    git_oid oid{};
    while (git_revwalk_next(&oid, walk.get()) == 0) result.push_back(oid);
    return result;
  };
  const auto all = [&] {
    Selection seeds;
    for (const auto& [reference, oid] : rewrite_refs()) {
      (void)reference;
      seeds.push_back(oid);
    }
    return ancestors(seeds);
  };

  std::function<Selection(std::string_view)> evaluate;
  evaluate = [&](std::string_view expression) -> Selection {
    expression = trim(expression);
    if (expression.empty()) throw UserError("revision set must not be empty");
    while (outer_parentheses(expression)) {
      expression = trim(expression.substr(1, expression.size() - 2));
    }

    if (const std::size_t separator = top_level_operator(expression, "|");
        separator != std::string_view::npos) {
      Selection result = evaluate(expression.substr(0, separator));
      append_unique(result, evaluate(expression.substr(separator + 1)));
      return result;
    }
    for (const std::string_view operation : {"&", "~"}) {
      const std::size_t separator = top_level_operator(expression, operation);
      if (separator == std::string_view::npos) continue;
      const Selection left = evaluate(expression.substr(0, separator));
      const auto right = ordered_set(evaluate(expression.substr(separator + 1)));
      Selection result;
      for (const git_oid& oid : left) {
        const bool contained = right.contains(oid);
        if ((operation == "&" && contained) ||
            (operation == "~" && !contained)) {
          result.push_back(oid);
        }
      }
      return result;
    }
    for (const std::string_view operation : {"..", "::"}) {
      const std::size_t separator = top_level_operator(expression, operation);
      if (separator == std::string_view::npos) continue;
      const Selection left = evaluate(expression.substr(0, separator));
      const Selection right = evaluate(
          expression.substr(separator + operation.size()));
      if (operation == "..") {
        const auto excluded = ordered_set(ancestors(left));
        Selection result;
        for (const git_oid& oid : ancestors(right)) {
          if (!excluded.contains(oid)) result.push_back(oid);
        }
        return result;
      }
      const auto upper = ordered_set(ancestors(right));
      Selection result;
      for (const git_oid& candidate : all()) {
        if (!upper.contains(candidate)) continue;
        for (const git_oid& lower : left) {
          const int descendant =
              git_graph_descendant_of(repo_.get(), &candidate, &lower);
          check(descendant, "evaluate DAG range");
          if (candidate == lower || descendant != 0) {
            result.push_back(candidate);
            break;
          }
        }
      }
      return result;
    }

    const std::size_t open = expression.find('(');
    if (open != std::string_view::npos && expression.back() == ')') {
      const std::string_view function = trim(expression.substr(0, open));
      const std::string_view argument =
          trim(expression.substr(open + 1, expression.size() - open - 2));
      if (function == "all") {
        if (!argument.empty()) throw UserError("all() takes no arguments");
        return all();
      }
      if (function == "none") {
        if (!argument.empty()) throw UserError("none() takes no arguments");
        return {};
      }
      if (function == "ancestors") return ancestors(evaluate(argument));
      if (function == "parents" || function == "children") {
        Selection result;
        for (const git_oid& oid : evaluate(argument)) {
          append_unique(result, function == "parents" ? parents(oid)
                                                       : children(oid));
        }
        return result;
      }
      if (function == "descendants") {
        const Selection seeds = evaluate(argument);
        Selection result;
        for (const git_oid& candidate : all()) {
          for (const git_oid& seed : seeds) {
            const int descendant =
                git_graph_descendant_of(repo_.get(), &candidate, &seed);
            check(descendant, "evaluate descendants");
            if (candidate == seed || descendant != 0) {
              result.push_back(candidate);
              break;
            }
          }
        }
        return result;
      }
      if (function == "roots" || function == "heads") {
        const Selection selected = argument.empty() ? all() : evaluate(argument);
        const auto members = ordered_set(selected);
        Selection result;
        for (const git_oid& oid : selected) {
          const Selection adjacent =
              function == "roots" ? parents(oid) : children(oid);
          if (std::ranges::none_of(adjacent, [&](const git_oid& candidate) {
                return members.contains(candidate);
              })) {
            result.push_back(oid);
          }
        }
        return result;
      }
      if (function == "root") {
        if (!argument.empty()) throw UserError("root() takes no arguments");
        return evaluate("roots(all())");
      }
      if (function == "bookmarks" || function == "tags") {
        const std::string_view pattern =
            argument.empty() ? std::string_view("*") : unquote(argument);
        const std::string_view prefix =
            function == "bookmarks" ? "refs/heads/" : "refs/tags/";
        Selection result;
        for (const auto& [reference, oid] : data_refs()) {
          if (starts_with(reference, prefix) &&
              string_pattern_matches(pattern,
                                     reference.substr(prefix.size()))) {
            if (function == "bookmarks") {
              append_unique(result, {oid});
            } else {
              git_object* raw_object = nullptr;
              check(git_object_lookup(&raw_object, repo_.get(), &oid,
                                      GIT_OBJECT_ANY),
                    "read tag target");
              ObjectPtr object(raw_object);
              git_object* raw_commit = nullptr;
              check(git_object_peel(&raw_commit, object.get(),
                                    GIT_OBJECT_COMMIT),
                    "resolve tag target");
              ObjectPtr commit_object(raw_commit);
              append_unique(result, {*git_object_id(commit_object.get())});
            }
          }
        }
        return result;
      }
      throw UserError("unknown revision-set function: " +
                      std::string(function));
    }
    return {resolve_atom(expression)};
  };
  return evaluate(revisions);
}

git_oid Repository::resolve(std::string_view revision) const {
  const std::vector<git_oid> resolved = resolve_set(revision);
  if (resolved.empty()) throw UserError("revision set is empty");
  if (resolved.size() != 1) {
    throw UserError("revision set contains multiple revisions");
  }
  return resolved.front();
}

std::vector<std::string> Repository::bookmarks(const git_oid& oid) const {
  std::vector<std::string> result;
  for (const auto& [name, target] : data_refs()) {
    if (starts_with(name, "refs/heads/") && target == oid) {
      result.push_back(name.substr(std::string_view("refs/heads/").size()));
    }
  }
  return result;
}

std::vector<git_oid> Repository::children(const git_oid& oid) const {
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  for (const auto& [name, target] : rewrite_refs()) {
    (void)name;
    check(git_revwalk_push(walk.get(), &target), "walk revisions");
  }
  std::set<git_oid, OidLess> result;
  git_oid candidate{};
  while (git_revwalk_next(&candidate, walk.get()) == 0) {
    for (const git_oid& parent : parents(candidate)) {
      if (parent == oid) {
        result.insert(candidate);
      }
    }
  }
  return {result.begin(), result.end()};
}

}  // namespace gg::detail
