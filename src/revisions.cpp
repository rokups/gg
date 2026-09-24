// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <git2/sys/errors.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <limits>
#include <sstream>

namespace gg::detail {
namespace {

constexpr std::string_view kAliasMapV1 = "gg-commit-aliases-v1";
constexpr auto kAliasLifetime = std::chrono::hours(24 * 7);

}  // namespace

std::int64_t commit_alias_time() {
  if (const char* test_time = std::getenv("GG_TEST_ALIAS_TIME");
      test_time != nullptr) {
    std::int64_t value = 0;
    const std::string_view text(test_time);
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()) {
      return value;
    }
  }
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

namespace {

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

std::vector<std::string_view> function_arguments(std::string_view value) {
  std::vector<std::string_view> result;
  int depth = 0;
  char quote = '\0';
  std::size_t begin = 0;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char character = value[index];
    if (quote != '\0') {
      if (character == quote) quote = '\0';
    } else if (character == '\'' || character == '"') {
      quote = character;
    } else if (character == '(') {
      ++depth;
    } else if (character == ')') {
      --depth;
    } else if (character == ',' && depth == 0) {  // GG_COV_EXCL_BRANCH
      result.push_back(trim(value.substr(begin, index - begin)));
      begin = index + 1;
    }
  }
  if (!value.empty()) result.push_back(trim(value.substr(begin)));
  return result;
}

std::size_t positive_integer(std::string_view value,
                             std::string_view function) {
  std::size_t result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {  // GG_COV_EXCL_BRANCH
    throw UserError(std::string(function) + " depth must be an integer");
  }
  return result;
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

std::map<std::string, CommitAlias> Repository::read_alias_map() const {
  const auto map_oid = ref_target(kAliasMapRef);
  if (!map_oid.has_value()) return {};

  CommitPtr map_commit = commit(*map_oid);
  git_tree* raw_tree = nullptr;
  check(git_commit_tree(&raw_tree, map_commit.get()), "read change map tree");
  TreePtr map_tree(raw_tree);
  git_tree_entry* raw_entry = nullptr;
  check(git_tree_entry_bypath(&raw_entry, map_tree.get(), "aliases"),
        "read commit aliases");
  TreeEntryPtr entry(raw_entry);
  if (git_tree_entry_type(entry.get()) != GIT_OBJECT_BLOB) {
    throw GitError("invalid commit alias map");
  }
  git_blob* raw_blob = nullptr;
  check(git_blob_lookup(&raw_blob, repo_.get(), git_tree_entry_id(entry.get())),
        "read commit aliases");
  BlobPtr blob(raw_blob);
  std::istringstream input(std::string(
      static_cast<const char*>(git_blob_rawcontent(blob.get())),
      git_blob_rawsize(blob.get())));
  std::string line;
  if (!std::getline(input, line) || line != kAliasMapV1) {
    throw GitError("invalid commit alias map");
  }

  std::map<std::string, CommitAlias> result;
  std::string alias_text;
  std::string target_text;
  std::int64_t last_used = 0;
  while (input >> alias_text >> target_text >> last_used) {
    git_oid alias{};
    git_oid target{};
    check(git_oid_fromstr(&alias, alias_text.c_str(),
                          git_repository_oid_type(repo_.get())),
          "parse commit alias");
    check(git_oid_fromstr(&target, target_text.c_str(),
                          git_repository_oid_type(repo_.get())),
          "parse commit alias target");
    if (!result.emplace(oid_string(alias), CommitAlias{target, last_used})
             .second) {
      throw GitError("duplicate commit alias");
    }
  }
  if (!input.eof()) throw GitError("invalid commit alias map");
  return result;
}

git_oid Repository::write_alias_map(
    const std::map<std::string, CommitAlias>& values) const {
  std::ostringstream output;
  output << kAliasMapV1 << '\n';
  for (const auto& [alias, value] : values) {
    output << alias << ' ' << oid_string(value.target) << ' '
           << value.last_used << '\n';
  }
  const std::string serialized = output.str();
  git_oid blob_oid{};
  check(git_blob_create_from_buffer(&blob_oid, repo_.get(), serialized.data(),
                                    serialized.size()),
        "write commit aliases");
  git_treebuilder* raw_builder = nullptr;
  check(git_treebuilder_new(&raw_builder, repo_.get(), nullptr),
        "create commit alias tree");
  GitPtr<git_treebuilder, git_treebuilder_free> builder(raw_builder);
  check(git_treebuilder_insert(nullptr, builder.get(), "aliases", &blob_oid,
                               GIT_FILEMODE_BLOB),
        "add commit aliases");
  git_oid tree_oid{};
  check(git_treebuilder_write(&tree_oid, builder.get()),
        "write commit alias tree");
  return create_commit(tree_oid, {}, "gg commit aliases");
}

std::set<std::string> Repository::legacy_change_refs() const {
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_glob_new(&raw_iterator, repo_.get(),
                                        "refs/gg/changes/*"),
        "list legacy change references");
  ReferenceIteratorPtr iterator(raw_iterator);
  std::set<std::string> result;
  while (true) {
    const char* name = nullptr;
    const int next = git_reference_next_name(&name, iterator.get());
    if (next == GIT_ITEROVER) break;
    check(next, "list legacy change references");
    result.emplace(name);
  }
  return result;
}

void Repository::migrate_legacy_branch_tracking() const {
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_glob_new(&raw_iterator, repo_.get(),
                                        "refs/gg/tracking/bookmarks/*"),
        "list legacy branch tracking references");
  ReferenceIteratorPtr iterator(raw_iterator);
  std::map<std::string, git_oid> updates;
  std::set<std::string> deletes;
  while (true) {
    const char* name = nullptr;
    const int next = git_reference_next_name(&name, iterator.get());
    if (next == GIT_ITEROVER) break;
    check(next, "list legacy branch tracking references");
    const std::string legacy(name);
    const auto target = ref_target(legacy);
    if (!target.has_value()) continue;  // GG_COV_EXCL_LINE
    updates.emplace(std::string(kBranchTrackingPrefix) +
                        legacy.substr(kLegacyBranchTrackingPrefix.size()),
                    *target);
    deletes.insert(legacy);
  }
  if (deletes.empty()) return;
  apply_refs(updates, deletes, "gg migrate branch tracking");
}

// Earlier gg versions kept unnamed heads visible through commit aliases. Mark
// every alias target no branch, workspace, or marker reaches as a user head
// once; afterwards aliases only redirect rewritten commit IDs.
void Repository::migrate_alias_heads() const {
  if (operation_view_.has_value()) return;
  git_config* raw_config = nullptr;
  check(git_repository_config(&raw_config, repo_.get()), "open Git configuration");
  GitPtr<git_config, git_config_free> config(raw_config);
  git_config* raw_local = nullptr;
  check(git_config_open_level(&raw_local, config.get(), GIT_CONFIG_LEVEL_LOCAL),
        "open repository configuration");
  GitPtr<git_config, git_config_free> local(raw_local);
  constexpr const char* key = "gg.unnamedheads";
  git_buf value = GIT_BUF_INIT;
  const int found = git_config_get_string_buf(&value, local.get(), key);
  git_buf_dispose(&value);
  if (found == 0) return;
  git_error_clear();

  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo_.get()), "walk unnamed heads");
  RevwalkPtr walk(raw_walk);
  std::set<git_oid, OidLess> candidates;
  for (const auto& [alias, target] : aliases()) {
    (void)alias;
    if (git_revwalk_push(walk.get(), &target) == 0) candidates.insert(target);
    git_error_clear();
  }
  std::map<std::string, git_oid> updates;
  if (!candidates.empty()) {
    for (const auto& [reference, oid] : data_refs()) {
      if (!starts_with(reference, "refs/heads/") &&
          !starts_with(reference, kWorkspacePrefix) &&
          !starts_with(reference, kVisibleHeadPrefix)) {
        continue;
      }
      if (git_revwalk_hide(walk.get(), &oid) != 0) git_error_clear();
    }
    git_oid oid{};
    while (git_revwalk_next(&oid, walk.get()) == 0) {
      if (candidates.contains(oid)) updates.emplace(user_head_ref(oid), oid);
    }
  }
  if (!updates.empty()) {
    record(std::move(updates), {}, head_state(), "gg migrate unnamed heads");
  }
  check(git_config_set_string(local.get(), key, "markers"),
        "record unnamed head migration");
}

// Earlier gg versions kept HEAD detached at @- and snapshotted the working
// tree into @. Project such a workspace onto HEAD == @ without touching the
// disk: an empty placeholder @ is dropped, anything else becomes HEAD.
bool Repository::migrate_legacy_workspace() const {
  const auto current_operation = operation();
  const auto workspace_oid = workspace();
  const auto head = head_oid();
  if (!current_operation.has_value() || !workspace_oid.has_value() ||
      !head.has_value() || *head == *workspace_oid) {
    return false;
  }
  const std::vector<git_oid> workspace_parents = parents(*workspace_oid);
  const OperationState recorded = parse_operation(*current_operation);
  const HeadState actual = head_state();
  if (workspace_parents.empty() || !(workspace_parents.front() == *head) ||
      actual.symbolic || recorded.head.symbolic ||
      recorded.head.value != actual.value) {
    return false;
  }
  CommitPtr change = commit(*workspace_oid);
  const bool placeholder =
      workspace_parents.size() == 1 &&
      *git_commit_tree_id(change.get()) ==
          *git_commit_tree_id(commit(*head).get()) &&
      first_line(git_commit_message(change.get())).empty() &&
      children(*workspace_oid).empty();
  const git_oid migrated = placeholder ? *head : *workspace_oid;
  std::map<std::string, git_oid> updates{{workspace_ref_name(), migrated}};
  std::vector<std::string> named;
  for (const auto& [reference, oid] : refs_with_prefix("refs/heads/")) {
    if (oid == migrated) named.push_back(reference);
  }
  HeadState next{false, oid_string(migrated)};
  if (named.size() == 1 && !branch_checked_out_elsewhere(named.front())) {
    next = {true, named.front()};
  } else if (named.empty()) {
    updates.emplace(user_head_ref(migrated), migrated);
  }
  record(std::move(updates), {}, next, "gg migrate working copy");
  set_head(next);
  return true;
}

const std::map<std::string, git_oid>& Repository::aliases() const {
  if (!ref_cache_enabled_) aliases_cache_.reset();
  if (aliases_cache_.has_value()) return *aliases_cache_;
  aliases_cache_.emplace();
  auto& result = *aliases_cache_;
  git_reference_iterator* raw_iterator = nullptr;
  check(git_reference_iterator_glob_new(&raw_iterator, repo_.get(),
                                        "refs/gg/aliases/*"),
        "list commit alias layers");
  ReferenceIteratorPtr iterator(raw_iterator);
  while (true) {
    git_reference* raw_reference = nullptr;
    const int next = git_reference_next(&raw_reference, iterator.get());
    if (next == GIT_ITEROVER) break;
    check(next, "list commit alias layers");
    ReferencePtr reference(raw_reference);
    if (const git_oid* target = git_reference_target(reference.get());
        target != nullptr) {
      const std::string_view name = git_reference_name(reference.get());
      result.emplace(std::string(name.substr(kAliasPrefix.size())), *target);
    }
  }
  for (auto& [alias, target] : result) {
    std::set<git_oid, OidLess> seen;
    while (seen.insert(target).second) {
      const auto next = result.find(oid_string(target));
      if (next == result.end() || next->second == target) break;
      target = next->second;
    }
  }
  return result;
}

void Repository::import_git_history(std::ostream* progress) const {
  const auto current_operation = operation();
  const bool initializing = !current_operation.has_value();
  const bool bootstrap_workspace = initializing ||
      operation_description(*current_operation) == "initialize repository";
  if (initializing && progress != nullptr && head_oid().has_value()) {
    *progress << "Initializing gg for this "
              << (linked_worktree_ ? "workspace" : "repository")
              << "; this may take a moment...\n";
  }
  const auto head = head_oid();
  // @ is Git's HEAD. Refresh must respect an undone or forgotten workspace;
  // a bare bootstrap operation still permits retry.
  const bool adopt_workspace = head.has_value() && !workspace().has_value() &&
                               bootstrap_workspace;
  if (initializing) {
    // The initial operation already includes @, so there is nothing to undo
    // before the first real command.
    if (adopt_workspace) {
      apply_refs({{workspace_ref_name(), *head}}, {}, "gg import history");
    }
    (void)ensure_operation();
    return;
  }
  (void)ensure_operation();
  migrate_alias_heads();
  if (migrate_legacy_workspace()) return;
  if (adopt_workspace) {
    record({{workspace_ref_name(), *head}}, {}, head_state(),
           "gg import history");
    return;
  }
  (void)adopt_external_head();
}

ShortId Repository::short_commit_id(const git_oid& oid) const {
  const std::string value = oid_string(oid);
  if (scoped_commit_ids_.has_value()) {
    const std::vector<std::string>& storage = *scoped_commit_ids_;
    std::vector<std::string_view> ids(storage.begin(), storage.end());
    const std::size_t unique = unique_prefix_length(value, ids);
    return {value.substr(0, std::max<std::size_t>(8, unique)), unique};
  }

  // Asking for a display ID must not enumerate every reachable commit. That
  // turns even a small mutation into a full-history operation in repositories
  // with many roots or tags. The ODB prefix lookup provides a conservative
  // uniqueness check (collisions with non-commit objects merely make the ID a
  // little longer), and layered aliases are cheap to check separately.
  git_odb* odb = nullptr;
  check(git_repository_odb(&odb, repo_.get()), "open object database");
  std::unique_ptr<git_odb, decltype(&git_odb_free)> owned_odb(odb, git_odb_free);
  const auto alias_values = aliases();
  std::size_t unique = value.size();
  for (std::size_t length = 1; length <= value.size(); ++length) {
    git_oid match{};
    const int found = git_odb_exists_prefix(&match, odb, &oid, length);
    if (found != GIT_OK || !(match == oid)) {
      if (found != GIT_EAMBIGUOUS && found != GIT_ENOTFOUND)
        check(found, "find unique commit prefix");
      continue;
    }
    const std::string_view prefix(value.data(), length);
    const bool alias_collision = std::ranges::any_of(
        alias_values, [&](const auto& item) {
          return item.first != value && item.first.starts_with(prefix);
        });
    if (!alias_collision) {
      unique = length;
      break;
    }
  }
  return {value.substr(0, std::max<std::size_t>(8, unique)), unique};
}

void Repository::set_short_id_scope(std::span<const git_oid> revisions) {
  const auto& alias_values = aliases();
  const std::set<git_oid, OidLess> displayed(revisions.begin(),
                                              revisions.end());
  std::map<git_oid, std::vector<std::string>, OidLess> aliases_by_target;
  for (const auto& [alias, target] : alias_values) {
    if (!displayed.contains(target) || alias == oid_string(target)) continue;
    git_oid parsed{};
    check(git_oid_fromstr(&parsed, alias.c_str(),
                          git_repository_oid_type(repo_.get())),
          "parse commit alias");
    aliases_by_target[target].push_back(oid_string(parsed));
  }
  scoped_commit_ids_.emplace();
  scoped_commit_ids_->reserve(revisions.size() + alias_values.size());
  for (const git_oid& revision : revisions) {
    scoped_commit_ids_->push_back(oid_string(revision));
    if (const auto aliases = aliases_by_target.find(revision);
        aliases != aliases_by_target.end()) {
      scoped_commit_ids_->insert(scoped_commit_ids_->end(),
                                 aliases->second.begin(), aliases->second.end());
    }
  }
}

std::vector<git_oid> Repository::commit_aliases(const git_oid& oid) const {
  std::vector<git_oid> result;
  for (const auto& [alias, target] : aliases()) {
    if (!(target == oid) || alias == oid_string(oid)) continue;
    git_oid value{};
    check(git_oid_fromstr(&value, alias.c_str(),
                          git_repository_oid_type(repo_.get())),
          "parse commit alias");
    result.push_back(value);
  }
  return result;
}

void Repository::add_alias_updates(RewritePlan& plan) const {
  for (const auto& [old_oid, new_oid] : plan.commits) {
    if (!(old_oid == new_oid)) {
      plan.updates[std::string(kAliasPrefix) + oid_string(old_oid)] = new_oid;
    }
  }
}

std::set<std::string> Repository::expired_alias_refs() const {
  std::set<std::string> result;
  const std::int64_t threshold =
      commit_alias_time() -
      std::chrono::duration_cast<std::chrono::seconds>(kAliasLifetime).count();
  for (const auto& [alias, value] : read_alias_map()) {
    if (alias != oid_string(value.target) && value.last_used <= threshold) {
      result.insert(std::string(kAliasPrefix) + alias);
    }
  }
  for (const auto& [alias, target] : aliases()) {
    (void)target;
    const std::string name = std::string(kAliasPrefix) + alias;
    git_reflog* raw_log = nullptr;
    const int read_result = git_reflog_read(&raw_log, repo_.get(), name.c_str());
    if (read_result == GIT_ENOTFOUND) {
      git_error_clear();
      continue;
    }
    check(read_result, "read commit alias usage");
    GitPtr<git_reflog, git_reflog_free> log(raw_log);
    if (git_reflog_entrycount(log.get()) == 0) continue;
    const git_reflog_entry* entry = git_reflog_entry_byindex(log.get(), 0);
    if (entry != nullptr && git_reflog_entry_committer(entry)->when.time <= threshold) {
      result.insert(name);
    }
  }
  return result;
}

bool Repository::collect_expired_aliases(std::string_view description) const {
  const std::int64_t threshold =
      commit_alias_time() -
      std::chrono::duration_cast<std::chrono::seconds>(kAliasLifetime).count();
  std::map<std::string, git_oid> updates;
  std::set<std::string> deletes;
  const std::set<std::string> expired = expired_alias_refs();
  const auto legacy = read_alias_map();
  for (const auto& [alias, value] : legacy) {
    if (alias != oid_string(value.target) && value.last_used > threshold) {
      updates[std::string(kAliasPrefix) + alias] = value.target;
    }
  }
  for (const auto& [alias, target] : aliases()) {
    const std::string name = std::string(kAliasPrefix) + alias;
    if (alias == oid_string(target) || expired.contains(name)) {
      deletes.insert(name);
    }
  }
  if (ref_target(kAliasMapRef).has_value()) {
    deletes.insert(std::string(kAliasMapRef));
  }
  if (updates.empty() && deletes.empty()) return false;
  apply_refs(updates, deletes, description);
  return true;
}

void Repository::touch_aliases(const std::vector<std::string>& touched) const {
  if (touched.empty() || operation_view_.has_value()) return;
  for (const std::string& alias : touched) {
    const std::string name = std::string(kAliasPrefix) + alias;
    const auto target = ref_target(name);
    if (!target.has_value()) continue;
    check(git_reference_ensure_log(repo_.get(), name.c_str()),
          "prepare commit alias usage log");
    git_reflog* raw_log = nullptr;
    check(git_reflog_read(&raw_log, repo_.get(), name.c_str()),
          "read commit alias usage log");
    GitPtr<git_reflog, git_reflog_free> log(raw_log);
    SignaturePtr configured = signature();
    git_signature* raw_actor = nullptr;
    check(git_signature_new(&raw_actor, configured->name, configured->email,
                            commit_alias_time(), configured->when.offset),
          "timestamp commit alias usage");
    SignaturePtr actor(raw_actor);
    check(git_reflog_append(log.get(), &*target, actor.get(),
                            "touch commit alias"),
          "touch commit alias");
    check(git_reflog_write(log.get()), "write commit alias usage log");
  }
}

git_oid Repository::resolve_atom(std::string_view revision) const {
  if (revision == "@" || starts_with(revision, "@-")) {
    auto workspace = this->workspace();
    if (!workspace.has_value()) workspace = head_oid();
    if (!workspace.has_value()) {
      throw UserError("HEAD has no commit yet");
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

  std::set<git_oid, OidLess> matches;
  std::vector<std::string> touched;
  for (const auto& [alias, oid] : aliases()) {
    if (starts_with(alias, revision)) {
      matches.insert(oid);
      touched.push_back(alias);
    }
  }
  if (!touched.empty()) {
    if (matches.size() > 1) {
      throw UserError("ambiguous commit ID: " + std::string(revision));
    }
    touch_aliases(touched);
    return *matches.begin();
  }
  const std::string branch = "refs/heads/" + std::string(revision);
  int valid_branch = 0;
  check(git_reference_name_is_valid(&valid_branch, branch.c_str()),
        "validate branch reference");
  if (valid_branch != 0) {
    const auto branch_target = ref_target(branch);
    if (branch_target.has_value()) {
      matches.insert(*branch_target);
    }
  }

  git_object* raw_object = nullptr;
  const int result = git_revparse_single(&raw_object, repo_.get(),
                                         std::string(revision).c_str());
  if (result == 0) {
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
          "resolve revision");
    ObjectPtr commit_object(raw_commit);
    matches.insert(*git_object_id(commit_object.get()));
  } else if (result == GIT_EAMBIGUOUS) {
    throw UserError("ambiguous commit ID: " + std::string(revision));
  } else {
    git_error_clear();
  }
  if (matches.size() > 1) {
    throw UserError("ambiguous commit ID: " + std::string(revision));
  }
  if (matches.empty()) {
    throw UserError("revision not found: " + std::string(revision));
  }
  touch_aliases(touched);
  return *matches.begin();
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
  const auto bounded_ancestors = [&](const Selection& seeds,
                                     std::size_t depth,
                                     bool first_parent_only) {
    Selection result;
    std::set<git_oid, OidLess> seen;
    std::vector<std::pair<git_oid, std::size_t>> pending;
    for (const git_oid& seed : seeds) pending.emplace_back(seed, 0);
    while (!pending.empty()) {
      const auto [oid, distance] = pending.back();
      pending.pop_back();
      if (!seen.insert(oid).second) continue;  // GG_COV_EXCL_BRANCH
      result.push_back(oid);
      if (distance >= depth) continue;
      const Selection values = parents(oid);
      if (!values.empty() && first_parent_only) {
        pending.emplace_back(values.front(), distance + 1);
      } else {
        for (const git_oid& parent : values) {
          pending.emplace_back(parent, distance + 1);
        }
      }
    }
    return result;
  };
  const auto all = [&] {
    Selection seeds;
    for (const auto& [reference, oid] : rewrite_refs()) {
      (void)reference;
      git_object* raw_object = nullptr;
      if (git_object_lookup(&raw_object, repo_.get(), &oid, GIT_OBJECT_ANY) <
          0) {
        git_error_clear();
        continue;
      }
      ObjectPtr object(raw_object);
      git_object* raw_commit = nullptr;
      if (git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT) < 0) {
        git_error_clear();
        continue;
      }
      ObjectPtr commit(raw_commit);
      seeds.push_back(*git_object_id(commit.get()));
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
      const std::vector<std::string_view> arguments =
          function_arguments(argument);
      if (function == "all") {
        if (!argument.empty()) throw UserError("all() takes no arguments");
        return all();
      }
      if (function == "none") {
        if (!argument.empty()) throw UserError("none() takes no arguments");
        return {};
      }
      if (function == "ancestors" || function == "first_ancestors") {
        if (arguments.empty() || arguments.size() > 2) {
          throw UserError(std::string(function) +
                          "() takes a revset and optional depth");
        }
        if (arguments.size() == 1) {
          if (function == "ancestors") return ancestors(evaluate(arguments[0]));
          return bounded_ancestors(
              evaluate(arguments[0]), std::numeric_limits<std::size_t>::max(),
              true);
        }
        return bounded_ancestors(evaluate(arguments[0]),
                                 positive_integer(arguments[1], function),
                                 function == "first_ancestors");
      }
      if (function == "present") {
        if (arguments.size() != 1) {
          throw UserError("present() takes one revset");
        }
        try {
          return evaluate(arguments.front());
        } catch (const UserError&) {  // GG_COV_EXCL_BRANCH
          return {};
        }
      }
      if (function == "visible_heads") {
        if (!arguments.empty()) {
          throw UserError("visible_heads() takes no arguments");
        }
        // The heads of an ancestry closure are necessarily among its seed
        // reference targets. Comparing those tips avoids materializing every
        // commit reachable from local refs, which is prohibitively expensive
        // in large repositories with long-lived tags.
        Selection tips;
        for (const auto& [reference, oid] : rewrite_refs()) {
          // Aliases only redirect rewritten commit IDs; unnamed heads the user
          // created are marked explicitly, so anything Git or gg leaves
          // behind internally stays hidden.
          const bool visible = starts_with(reference, "refs/heads/")
              || starts_with(reference, kWorkspacePrefix)
              || starts_with(reference, kVisibleHeadPrefix);
          // Tags name history but do not keep an unnamed line of work visible;
          // callers select tagged history explicitly. Excluding them also
          // prevents repositories with hundreds of release tags from turning
          // this head query into hundreds of deep reachability checks.
          if (!visible) continue;
          git_object* raw_object = nullptr;
          check(git_object_lookup(&raw_object, repo_.get(), &oid,
                                  GIT_OBJECT_ANY),
                "load visible-head reference");
          ObjectPtr object(raw_object);
          git_object* raw_commit = nullptr;
          check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
                "peel visible-head reference");
          ObjectPtr commit(raw_commit);
          append_unique(tips, {*git_object_id(commit.get())});
        }
        Selection heads;
        for (const git_oid& candidate : tips) {
          const bool ancestor_of_head = std::ranges::any_of(
              heads, [&](const git_oid& head) {
                if (candidate == head) return true;
                const int descendant =
                    git_graph_descendant_of(repo_.get(), &head, &candidate);
                check(descendant, "compare visible heads");
                return descendant != 0;
              });
          if (ancestor_of_head) continue;
          std::erase_if(heads, [&](const git_oid& head) {
            const int descendant =
                git_graph_descendant_of(repo_.get(), &candidate, &head);
            check(descendant, "compare visible heads");
            return descendant != 0;
          });
          heads.push_back(candidate);
        }
        return heads;
      }
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
      if (function == "merges") {
        if (!argument.empty()) throw UserError("merges() takes no arguments");
        Selection result;
        for (const git_oid& oid : all()) {
          if (parents(oid).size() > 1) result.push_back(oid);
        }
        return result;
      }
      if (function == "description" || function == "author" ||
          function == "committer") {
        if (arguments.size() != 1) {
          throw UserError(std::string(function) + "() takes one pattern");
        }
        const std::string_view pattern = unquote(arguments.front());
        Selection result;
        for (const git_oid& oid : all()) {
          CommitPtr commit = this->commit(oid);
          std::string value;
          if (function == "description") {
            const char* message = git_commit_message(commit.get());
            value = message == nullptr ? "" : message;  // GG_COV_EXCL_BRANCH
          } else {
            const git_signature* signature =
                function == "author" ? git_commit_author(commit.get())
                                     : git_commit_committer(commit.get());
            value = std::string(signature->name) + " <" + signature->email +
                    ">";
          }
          if (string_pattern_matches(pattern, value, "substring")) {
            result.push_back(oid);
          }
        }
        return result;
      }
      if (function == "conflicts") {
        if (!argument.empty()) {
          throw UserError("conflicts() takes no arguments");
        }
        Selection result;
        for (const git_oid& oid : all()) {
          if (commit_has_conflicts(oid)) result.push_back(oid);  // GG_COV_EXCL_BRANCH
        }
        return result;
      }
      if (function == "empty") {
        if (!argument.empty()) throw UserError("empty() takes no arguments");
        Selection result;
        for (const git_oid& oid : all()) {
          CommitPtr commit = this->commit(oid);
          const Selection commit_parents = parents(oid);
          if (commit_parents.size() == 1) {
            CommitPtr parent = this->commit(commit_parents.front());
            if (*git_commit_tree_id(commit.get()) ==  // GG_COV_EXCL_BRANCH
                *git_commit_tree_id(parent.get())) {
              result.push_back(oid);
            }
          }
        }
        return result;
      }
      if (function == "commit_id") {
        if (arguments.size() != 1) {
          throw UserError(std::string(function) + "() takes one prefix");
        }
        const std::string_view prefix = unquote(arguments.front());
        Selection result;
        std::vector<std::string> touched;
        for (const git_oid& oid : all()) {
          bool matches = starts_with(oid_string(oid), prefix);
          for (const git_oid& alias : commit_aliases(oid)) {
            const std::string value = oid_string(alias);
            if (starts_with(value, prefix)) {
              matches = true;
              touched.push_back(value);
            }
          }
          if (matches) result.push_back(oid);
        }
        touch_aliases(touched);
        return result;
      }
      if (function == "remote_branches" ||
          function == "tracked_remote_branches" ||
          function == "untracked_remote_branches") {
        const std::string_view pattern =
            argument.empty() ? std::string_view("*") : unquote(argument);
        Selection result;
        constexpr std::string_view prefix = "refs/remotes/";
        for (const auto& [reference, oid] : data_refs()) {
          if (!starts_with(reference, prefix) || reference.ends_with("/HEAD")) {
            continue;
          }
          const std::string remote_name = reference.substr(prefix.size());
          const std::size_t slash = remote_name.find('/');
          if (slash == std::string::npos) continue;
          const std::string tracking =
              std::string(kBranchTrackingPrefix) +
              remote_name.substr(0, slash) + "/" +
              remote_name.substr(slash + 1);
          const bool tracked = ref_target(tracking).has_value();
          if (function == "tracked_remote_branches" && !tracked) continue;
          if (function == "untracked_remote_branches" && tracked) continue;
          if (string_pattern_matches(pattern, remote_name.substr(slash + 1))) {
            append_unique(result, {oid});
          }
        }
        return result;
      }
      if (function == "root") {
        if (!argument.empty()) throw UserError("root() takes no arguments");
        return evaluate("roots(all())");
      }
      if (function == "branches" || function == "tags") {
        const std::string_view pattern =
            argument.empty() ? std::string_view("*") : unquote(argument);
        const std::string_view prefix =
            function == "branches" ? "refs/heads/" : "refs/tags/";
        Selection result;
        for (const auto& [reference, oid] : data_refs()) {
          if (starts_with(reference, prefix) &&
              string_pattern_matches(pattern,
                                     reference.substr(prefix.size()))) {
            if (function == "branches") {
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

std::vector<std::string> Repository::branches(const git_oid& oid) const {
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
