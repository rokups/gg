// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>
#include <git2/sys/errors.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

std::pair<std::string, std::string> parse_author(std::string_view value) {
  const std::size_t separator = value.rfind(" <");
  if (separator == std::string_view::npos) {
    throw UserError("author must have the form 'Name <email>'");
  }
  if (separator == 0) {
    throw UserError("author must have the form 'Name <email>'");
  }
  if (value.back() != '>') {
    throw UserError("author must have the form 'Name <email>'");
  }
  std::string name(value.substr(0, separator));
  std::string email(value.substr(separator + 2, value.size() - separator - 3));
  if (email.empty()) {
    throw UserError("author must have the form 'Name <email>'");
  }
  return {std::move(name), std::move(email)};
}

const char* parse_time(const std::string& value,
                       const char* format,
                       std::tm& fields) {
  std::istringstream input(value);
  input >> std::get_time(&fields, format);
  if (input.fail()) return nullptr;
  const std::streampos position = input.tellg();
  return value.c_str() +
         (position == std::streampos(-1) ? value.size()
                                         : static_cast<std::size_t>(position));
}

git_time_t utc_time(const std::tm& fields) {
  using namespace std::chrono;
  const year_month_day date{year(fields.tm_year + 1900),
                            month(static_cast<unsigned>(fields.tm_mon + 1)),
                            day(static_cast<unsigned>(fields.tm_mday))};
  if (!date.ok() || fields.tm_hour < 0 || fields.tm_hour > 23 ||
      fields.tm_min < 0 || fields.tm_min > 59 || fields.tm_sec < 0 ||
      fields.tm_sec > 60) {
    throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
  }
  return duration_cast<seconds>(sys_days(date).time_since_epoch()).count() +
         fields.tm_hour * 3600 + fields.tm_min * 60 + fields.tm_sec;
}

std::pair<git_time_t, int> parse_rfc3339_timestamp(std::string_view value) {
  if (value.size() < 20) {
    throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
  }
  std::tm fields{};
  std::string owned(value);
  const char* suffix = parse_time(owned, "%Y-%m-%dT%H:%M:%S", fields);
  if (suffix != owned.c_str() + 19) {
    throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
  }
  int offset = 0;
  if (*suffix == 'Z') {
    if (suffix[1] != '\0') {
      throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
    }
    offset = 0;
  } else {
    if (*suffix != '+' && *suffix != '-') {
      throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
    }
    if (std::string_view(suffix).size() != 6 || suffix[3] != ':') {
      throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
    }
    for (const int index : {1, 2, 4, 5}) {
      if (std::isdigit(static_cast<unsigned char>(suffix[index])) == 0) {
        throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
      }
    }
    const int hours = (suffix[1] - '0') * 10 + suffix[2] - '0';
    const int minutes = (suffix[4] - '0') * 10 + suffix[5] - '0';
    if (hours > 23) {
      throw UserError("author timestamp has an invalid UTC offset");
    }
    if (minutes > 59) {
      throw UserError("author timestamp has an invalid UTC offset");
    }
    offset = hours * 60 + minutes;
    if (*suffix == '-') offset = -offset;
  }
  return {utc_time(fields) - offset * 60, offset};
}

std::pair<git_time_t, int> parse_rfc2822_timestamp(std::string_view value) {
  std::tm fields{};
  std::string owned(value);
  const char* zone = parse_time(owned, "%a, %d %b %Y %H:%M:%S ", fields);
  if (zone == nullptr || *zone == '\0') {
    throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
  }

  int offset = 0;
  std::cmatch numeric;
  static const std::regex numeric_zone(  // GG_COV_EXCL_BRANCH
      R"(([+-])([0-9]{2})([0-9]{2}))");  // GG_COV_EXCL_BRANCH
  if (std::regex_match(zone, numeric, numeric_zone)) {
    const int hours = std::stoi(numeric[2].str());
    const int minutes = std::stoi(numeric[3].str());
    if (hours > 23) {
      throw UserError("author timestamp has an invalid UTC offset");
    }
    if (minutes > 59) {
      throw UserError("author timestamp has an invalid UTC offset");
    }
    offset = hours * 60 + minutes;
    if (numeric[1].str() == "-") offset = -offset;
  } else {
    static constexpr std::array named_zones{
        std::pair<std::string_view, int>{"UT", 0},
        std::pair<std::string_view, int>{"GMT", 0},
        std::pair<std::string_view, int>{"EST", -300},
        std::pair<std::string_view, int>{"EDT", -240},
        std::pair<std::string_view, int>{"CST", -360},
        std::pair<std::string_view, int>{"CDT", -300},
        std::pair<std::string_view, int>{"MST", -420},
        std::pair<std::string_view, int>{"MDT", -360},
        std::pair<std::string_view, int>{"PST", -480},
        std::pair<std::string_view, int>{"PDT", -420},
    };
    const auto named = std::ranges::find_if(
        named_zones, [&](const auto& item) { return item.first == zone; });
    if (named == named_zones.end()) {
      throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
    }
    offset = named->second;
  }
  return {utc_time(fields) - offset * 60, offset};
}

std::pair<git_time_t, int> parse_author_timestamp(std::string_view value) {
  if (value.size() > 10 && value[10] == 'T') {
    return parse_rfc3339_timestamp(value);
  }
  return parse_rfc2822_timestamp(value);
}

SignaturePtr make_signature(std::string_view name,
                            std::string_view email,
                            git_time_t time,
                            int offset) {
  git_signature* raw = nullptr;
  const std::string owned_name(name);
  const std::string owned_email(email);
  check(git_signature_new(&raw, owned_name.c_str(), owned_email.c_str(), time,
                          offset),
        "create author signature");
  return SignaturePtr(raw);
}

bool same_signature(const git_signature* left, const git_signature* right) {
  if (std::string_view(left->name) != right->name) return false;
  if (std::string_view(left->email) != right->email) return false;
  if (left->when.time != right->when.time) return false;
  return left->when.offset == right->when.offset;
}

struct LogReference {
  git_oid oid;
  std::string name;
};

std::vector<LogReference> log_references(Repository& repo,
                                         std::string_view prefix) {
  std::vector<LogReference> result;
  for (const auto& [name, oid] : repo.refs_with_prefix(prefix)) {
    result.push_back({oid, name.substr(prefix.size())});
  }
  return result;
}

}  // namespace

void command_new(Repository& repo,
                 const NewCommand& options,
                 std::ostream& output) {
  repo.prepare_command();
  const auto old_workspace = repo.workspace();
  std::vector<git_oid> parents;
  const std::vector<git_oid> after =
      resolve_revision_arguments(repo, options.insert_after);
  const std::vector<git_oid> before =
      resolve_revision_arguments(repo, options.insert_before);
  const std::set<git_oid, OidLess> after_set(after.begin(), after.end());
  const std::set<git_oid, OidLess> before_set(before.begin(), before.end());
  for (const git_oid& oid : before_set) {
    if (after_set.contains(oid)) {
      throw UserError("cannot insert both before and after the same revision");
    }
  }

  if (!after.empty()) {
    parents = after;
  } else if (!before.empty()) {
    std::set<git_oid, OidLess> seen;
    for (const git_oid& oid : before) {
      for (const git_oid& parent : repo.parents(oid)) {
        if (seen.insert(parent).second) parents.push_back(parent);
      }
    }
  } else {
    std::vector<std::string> revisions = options.parents;
    if (revisions.empty()) {
      if (old_workspace.has_value()) {
        revisions.emplace_back("@");
      } else if (const auto head = repo.head_oid(); head.has_value()) {
        parents.push_back(*head);
      }
    }
    if (!revisions.empty()) parents = commit_parents(repo, revisions);
  }
  // Git-like: a new change on the tip of the current branch (or on a branch
  // named explicitly) continues that branch; anything else forks detached.
  std::optional<std::string> continued;
  if (!options.detach && !options.no_edit && after.empty() && before.empty() &&
      parents.size() == 1) {
    if (options.parents.empty() ||
        (options.parents.size() == 1 && options.parents.front() == "@")) {
      if (auto branch = repo.current_branch(); branch.has_value()) {
        const auto target = repo.ref_target(*branch);
        if (target.has_value() && *target == parents.front()) {
          continued = std::move(branch);
        }
      }
    } else if (options.parents.size() == 1) {
      continued = local_branch_named(repo, options.parents.front());
    }
  }
  const auto visible = repo.resolve_set("all()");
  const std::set<git_oid, OidLess> existing(visible.begin(), visible.end());
  const git_oid tree = combined_tree(repo, parents);
  SignaturePtr identity = repo.signature();
  git_oid change{};
  do {
    change = repo.create_commit(tree, parents, options.message,
                                identity.get(), identity.get());
    ++identity->when.time;
  } while (existing.contains(change));
  RewritePlan plan;
  if (!after.empty() || !before.empty()) {
    const auto refs = repo.rewrite_refs();
    git_revwalk* raw_walk = nullptr;
    check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
    RevwalkPtr walk(raw_walk);
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
    for (const auto& [name, oid] : refs) {
      (void)name;
      const int pushed = git_revwalk_push(walk.get(), &oid);
      if (pushed != GIT_EINVALIDSPEC) {  // GG_COV_EXCL_BRANCH
        check(pushed, "walk revisions");
      }
    }
    for (const git_oid& oid : after_set) {
      check(git_revwalk_push(walk.get(), &oid), "walk inserted revisions");
    }
    for (const git_oid& oid : before_set) {
      check(git_revwalk_push(walk.get(), &oid), "walk inserted revisions");
    }

    std::map<git_oid, git_oid, OidLess> replacements;
    git_oid oid{};
    while (git_revwalk_next(&oid, walk.get()) == 0) {
      const std::vector<git_oid> old_parents = repo.parents(oid);
      std::vector<git_oid> new_parents;
      bool parents_changed = false;
      for (const git_oid& parent : old_parents) {
        const auto replacement = replacements.find(parent);
        const git_oid next = replacement == replacements.end()
                                 ? parent
                                 : replacement->second;
        parents_changed |= !(next == parent);
        new_parents.push_back(next);
      }
      if (after_set.contains(oid)) {
        replacements[oid] = change;
        continue;
      }
      if (before_set.contains(oid)) {
        if (after.empty()) {
          new_parents = {change};
        } else if (std::ranges::none_of(new_parents, [&](const git_oid& parent) {
                     return parent == change;
                   })) {
          new_parents.insert(new_parents.begin(), change);
        }
        const git_oid rewritten = repo.rewrite_commit(
            oid, new_parents, *git_commit_tree_id(repo.commit(oid).get()));
        plan.commits[oid] = rewritten;
        replacements[oid] = rewritten;
      } else if (parents_changed) {
        const git_oid rewritten = repo.rewrite_commit(oid, new_parents);
        plan.commits[oid] = rewritten;
        replacements[oid] = rewritten;
      }
    }
    for (const auto& [name, target] : refs) {
      const auto replacement = plan.commits.find(target);
      if (replacement != plan.commits.end()) {
        plan.updates[name] = replacement->second;
      }
    }
    repo.add_alias_updates(plan);
  }
  std::set<std::string> deletes;
  if (continued.has_value()) {
    plan.updates[*continued] = change;
  } else if (after.empty() && before.empty() &&
             !(options.parents.empty() && parents.empty() &&
               repo.current_branch().has_value())) {
    // Unless an unborn branch is being born, the change is a new unnamed head
    // the user created; keep it visible.
    plan.updates[user_head_ref(change)] = change;
  }
  // A marked parent is no longer a head once the new change sits on it.
  if (after.empty() && before.empty()) {
    for (const git_oid& parent : parents) deletes.insert(user_head_ref(parent));
  }
  if (!options.no_edit) {
    finish_workspace(repo, change, std::move(plan.updates), std::move(deletes),
                     "gg new",
                     continued.has_value() ? HeadIntent::attach(*continued)
                     : options.detach      ? HeadIntent::detach()
                                           : HeadIntent::follow());
  } else if (old_workspace.has_value()) {
    const git_oid workspace = plan.commits.contains(*old_workspace)
                                  ? plan.commits.at(*old_workspace)
                                  : *old_workspace;
    finish_workspace(repo, workspace, std::move(plan.updates),
                     std::move(deletes), "gg new");
  } else {
    finish_without_workspace(repo, std::move(plan), std::move(deletes),
                             "gg new");
  }
  output << (options.no_edit ? "Created change: " : "Working copy now at: ")
         << repo.short_commit_id(change).value << ' '
         << (options.message.empty() ? "(no description set)" : options.message);
  if (continued.has_value()) {
    output << " (on branch " << continued->substr(11) << ')';
  }
  output << '\n';
}

namespace {

std::vector<std::string> normalized_worktree_paths(
    const std::vector<std::string>& values, bool& select_all) {
  std::vector<std::string> paths;
  select_all = values.empty();
  for (const std::string& path : values) {
    const std::filesystem::path parsed_path(path);
    if (path.empty() || path.front() == '/') {
      throw UserError("commit paths must be repository-relative");
    }
    for (const auto& component : parsed_path) {
      if (component == "..") {
        throw UserError("commit paths must not contain '..'");
      }
    }
    const std::string normalized = parsed_path.lexically_normal().generic_string();
    if (normalized == ".") {
      select_all = true;
    } else {
      paths.push_back(normalized);
    }
  }
  return paths;
}

struct WorktreeSelection {
  git_oid full;
  git_oid selected;
};

// Snapshot the working tree relative to `base_tree` and select the part the
// caller asked for. The index is left matching the selected tree so Git
// reports the unselected edits as unstaged.
WorktreeSelection select_worktree(Repository& repo,
                                  const git_oid& base_tree,
                                  const std::vector<std::string>& values,
                                  bool interactive,
                                  std::string_view tool) {
  bool select_all = false;
  const std::vector<std::string> paths =
      normalized_worktree_paths(values, select_all);
  const git_oid full_tree = repo.snapshot_tree(base_tree);
  git_oid selected_tree =
      select_all ? full_tree : repo.selected_tree(base_tree, full_tree, paths);
  if (interactive || !tool.empty()) {
    selected_tree = select_diff_tree(
        repo, base_tree, selected_tree,
        select_all ? std::vector<std::string>{} : paths, tool);
  }
  return {full_tree, selected_tree};
}

void stage_tree(Repository& repo, const git_oid& tree_oid) {
  git_index* raw_index = nullptr;
  check(git_repository_index(&raw_index, repo.raw()), "open index");
  IndexPtr index(raw_index);
  TreePtr tree = repo.tree(tree_oid);
  check(git_index_read_tree(index.get(), tree.get()), "stage committed tree");
  check(git_index_write(index.get()), "write index");
}

std::optional<git_oid> active_commit(Repository& repo) {
  return repo.workspace().has_value() ? repo.workspace() : repo.head_oid();
}

}  // namespace

void command_commit(Repository& repo,
                    const CommitCommand& options,
                    std::ostream& output) {
  repo.prepare_command();
  repo.require_current_head();
  const auto active = active_commit(repo);
  const git_oid base_tree = active.has_value()
                                ? *git_commit_tree_id(repo.commit(*active).get())
                                : repo.empty_tree();
  const WorktreeSelection selection = select_worktree(
      repo, base_tree, options.paths, options.interactive, options.tool);
  if (selection.selected == base_tree) {
    throw UserError("no working-tree changes to commit");
  }
  std::string message = options.message;
  if (options.editor) message = edit_text(repo, message);
  const std::vector<git_oid> parents = active.has_value()
                                           ? std::vector<git_oid>{*active}
                                           : std::vector<git_oid>{};
  const git_oid created = repo.create_commit(selection.selected, parents, message);
  std::map<std::string, git_oid> updates;
  if (const auto branch = repo.current_branch(); branch.has_value()) {
    const auto target = repo.ref_target(*branch);
    if (!target.has_value() || (active.has_value() && *target == *active)) {
      updates.emplace(*branch, created);
    }
  }
  // A commit on a detached HEAD extends an unnamed head; keep it visible.
  if (updates.empty()) updates.emplace(user_head_ref(created), created);
  finish_workspace_preserving_worktree(repo, created, std::move(updates),
                                       "gg commit");
  if (!(selection.selected == selection.full)) {
    stage_tree(repo, selection.selected);
  }
  output << "Committed as " << repo.short_commit_id(created).value << ' '
         << (message.empty() ? "(no description set)" : first_line(message.c_str()))
         << '\n';
}

void command_squash_worktree(Repository& repo,
                             const std::vector<std::string>& paths,
                             bool interactive,
                             std::string_view tool,
                             std::optional<std::string_view> message,
                             std::ostream& output) {
  repo.prepare_command();
  repo.require_current_head();
  const auto active = active_commit(repo);
  if (!active.has_value()) {
    throw UserError("no active commit to amend");
  }
  CommitPtr original = repo.commit(*active);
  const git_oid old_tree = *git_commit_tree_id(original.get());
  const WorktreeSelection selection =
      select_worktree(repo, old_tree, paths, interactive, tool);
  const git_oid rewritten = repo.rewrite_commit(
      *active, repo.parents(*active), selection.selected, message);
  if (rewritten == *active) {
    output << "Nothing changed.\n";
    return;
  }
  RewritePlan plan = repo.descendants({{*active, rewritten}});
  finish_workspace_preserving_worktree(repo, rewritten,
                                       std::move(plan.updates),
                                       "gg squash working tree");
  if (!(selection.selected == selection.full)) {
    stage_tree(repo, selection.selected);
  }
  output << "Amended as " << repo.short_commit_id(rewritten).value << '\n';
}

void command_amend_worktree(Repository& repo,
                            std::optional<std::string_view> revision,
                            std::optional<std::string_view> message,
                            std::ostream& output) {
  repo.require_current_head();
  const auto active = repo.workspace().has_value() ? repo.workspace()
                                                    : repo.head_oid();
  if (!active.has_value()) {
    throw UserError("no active commit to amend");
  }
  if (revision.has_value()) {
    const std::string oid = oid_string(*active);
    if (!revision->empty() &&
        std::ranges::all_of(*revision, [](unsigned char c) {
          return std::isxdigit(c) != 0;
        }) && !oid.starts_with(*revision)) {
      throw UserError("selected revision is stale");
    }
    if (repo.resolve(*revision) != *active) {
      throw UserError("selected revision is not the active commit");
    }
  }
  CommitPtr original = repo.commit(*active);
  const git_oid old_tree = *git_commit_tree_id(original.get());
  const git_oid new_tree = repo.snapshot_tree(old_tree);
  const git_oid rewritten = repo.rewrite_commit(
      *active, repo.parents(*active), new_tree, message);
  if (rewritten == *active) {
    output << "Nothing changed.\n";
    return;
  }
  RewritePlan plan = repo.descendants({{*active, rewritten}});
  finish_workspace_preserving_worktree(repo, rewritten,
                                       std::move(plan.updates),
                                       "gg amend working tree");
  output << "Amended as " << repo.short_commit_id(rewritten).value << '\n';
}

void command_amend_tree_worktree(Repository& repo,
                                 std::string_view revision,
                                 const git_oid& tree_oid,
                                 std::ostream& output) {
  repo.require_current_head();
  const auto active = repo.workspace().has_value() ? repo.workspace()
                                                    : repo.head_oid();
  if (!active.has_value()) {
    throw UserError("no active commit to amend");
  }
  const std::string oid = oid_string(*active);
  if (revision.empty() ||
      (std::ranges::all_of(revision, [](unsigned char c) {
         return std::isxdigit(c) != 0;
       }) && !oid.starts_with(revision)) ||
      repo.resolve(revision) != *active) {
    throw UserError("selected revision is not the active commit");
  }
  (void)repo.tree(tree_oid);  // Validate object type before modifying refs.
  const git_oid old_tree = *git_commit_tree_id(repo.commit(*active).get());
  if (old_tree == tree_oid) {
    output << "Nothing changed.\n";
    return;
  }
  repo.preserve_conflicts(old_tree, tree_oid);
  const git_oid rewritten = repo.rewrite_commit(
      *active, repo.parents(*active), tree_oid);
  RewritePlan plan = repo.descendants({{*active, rewritten}});
  finish_workspace_preserving_worktree(
      repo, rewritten, std::move(plan.updates), "gg amend tree");
  output << "Amended as " << repo.short_commit_id(rewritten).value << '\n';
}

std::optional<std::string> local_branch_named(Repository& repo,
                                              std::string_view revision) {
  if (revision.empty() || revision == "@") return std::nullopt;
  std::string name = "refs/heads/" + std::string(revision);
  int valid = 0;
  if (git_reference_name_is_valid(&valid, name.c_str()) != 0 || valid == 0) {
    git_error_clear();
    return std::nullopt;
  }
  if (!repo.ref_target(name).has_value()) return std::nullopt;
  return name;
}

HeadIntent checkout_intent(Repository& repo, std::string_view revision) {
  if (revision == "@") return HeadIntent::follow();
  if (auto branch = local_branch_named(repo, revision)) {
    return HeadIntent::attach(std::move(*branch));
  }
  return HeadIntent::detach();
}


void command_status(Repository& repo,
                    const StatusCommand& options,
                    std::ostream& output) {
  repo.prepare_command();
  std::vector<std::string> paths;
  for (const std::string& value : options.paths) {
    (void)fileset_matches(value, "");
    paths.push_back(value);
  }
  const auto branch = repo.current_branch();
  const auto workspace = active_commit(repo);
  if (!workspace.has_value()) {
    output << "On branch " << (branch.has_value() ? branch->substr(11) : "")
           << "\nNo commits yet.\n";
    return;
  }
  if (branch.has_value()) {
    output << "On branch " << styled(output, branch->substr(11), OutputStyle::branch)
           << '\n';
  } else {
    output << "HEAD detached at "
           << styled_short_commit_id(repo, output, *workspace, true) << '\n';
  }
  CommitPtr change = repo.commit(*workspace);
  output << "Working copy (@): "
         << styled_short_commit_id(repo, output, *workspace, true)
         << ' ';
  const std::string description = first_line(git_commit_message(change.get()));
  output << (description.empty() ? "(no description set)" : description) << '\n';
  const auto parents = repo.parents(*workspace);
  if (!parents.empty()) {
    CommitPtr parent = repo.commit(parents.front());
    output << "Parent commit (@-): "
           << styled_short_commit_id(repo, output, parents.front())
           << ' '
           << first_line(git_commit_message(parent.get())) << '\n';
  }

  const auto selected = [&](const char* raw_path) {
    if (paths.empty()) return true;
    const std::string_view path = raw_path == nullptr ? "" : raw_path;
    return std::ranges::any_of(paths, [&](const auto& fileset) {
      return fileset_matches(fileset, path);
    });
  };
  // Report what `gg commit` would record: the working tree as gg snapshots
  // it (honoring tracking rules and size limits), plus files it would skip.
  const git_oid base_tree_oid = *git_commit_tree_id(change.get());
  const git_oid current_tree_oid = repo.worktree_tree(base_tree_oid);
  TreePtr base_tree = repo.tree(base_tree_oid);
  TreePtr current_tree = repo.tree(current_tree_oid);
  git_diff* raw_diff = nullptr;
  check(git_diff_tree_to_tree(&raw_diff, repo.raw(), base_tree.get(),
                              current_tree.get(), nullptr),
        "compare working tree");
  DiffPtr diff(raw_diff);
  git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
  check(git_diff_find_similar(diff.get(), &find_options), "find renamed files");
  std::vector<const git_diff_delta*> deltas;
  for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
    if (selected(delta->old_file.path) || selected(delta->new_file.path)) {
      deltas.push_back(delta);
    }
  }
  std::vector<std::string> untracked = repo.untracked_paths();
  std::erase_if(untracked, [&](const std::string& path) {
    git_tree_entry* raw_entry = nullptr;
    const int found =
        git_tree_entry_bypath(&raw_entry, current_tree.get(), path.c_str());
    git_tree_entry_free(raw_entry);
    if (found != 0) git_error_clear();
    return found == 0 || !selected(path.c_str());
  });
  if (deltas.empty() && untracked.empty()) {
    output << "The working tree has no changes.\n";
  } else {
    output << "Working tree changes:\n";
    for (const git_diff_delta* delta : deltas) {
      const char status = delta->status == GIT_DELTA_ADDED      ? 'A'
                          : delta->status == GIT_DELTA_DELETED  ? 'D'
                          : delta->status == GIT_DELTA_RENAMED  ? 'R'
                                                                 : 'M';
      const OutputStyle style = delta->status == GIT_DELTA_ADDED
                                    ? OutputStyle::added
                                : delta->status == GIT_DELTA_DELETED
                                    ? OutputStyle::removed
                                    : OutputStyle::modified;
      output << styled(output,
                       std::string(1, status) + " " + delta->new_file.path,
                       style)
             << '\n';
    }
    for (const std::string& path : untracked) {
      output << styled(output, "? " + path, OutputStyle::added) << '\n';
    }
  }
  std::vector<std::string> conflicts = repo.conflict_paths(*workspace);
  std::erase_if(conflicts, [&](const std::string& path) {
    return !selected(path.c_str());
  });
  if (!conflicts.empty()) {
    output << "Unresolved conflicts:\n";
    for (const std::string& path : conflicts) output << "C " << path << '\n';
    output << "Edit the files to resolve them, then amend @ with `gg squash`.\n";
  }
}

void command_log(Repository& repo,
                 const LogCommand& options,
                 std::ostream& output) {
  repo.prepare_command();
  if (options.limit == 0) {
    if (options.count) output << "0\n";
    return;
  }
  struct Candidate {
    std::int64_t time;
    std::uint64_t order;
    git_oid oid;
    bool operator<(const Candidate& other) const {
      return time != other.time ? time < other.time : order < other.order;
    }
  };
  std::vector<git_oid> revisions;
  const auto matches_paths = [&](const git_oid& oid) {
    return options.paths.empty()
        || revision_matches_paths(repo, oid, options.paths, options.format);
  };
  std::vector<LogReference> local_branches;
  if (!options.revision.empty()) {
    const std::vector<git_oid> selected = repo.resolve_set(options.revision);
    const std::set<git_oid, OidLess> selected_revisions(selected.begin(),
                                                         selected.end());
    git_revwalk* raw_walk = nullptr;
    check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
    RevwalkPtr walk(raw_walk);
    git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
    for (const git_oid& oid : selected) {
      check(git_revwalk_push(walk.get(), &oid), "walk revisions");
    }
    git_oid oid{};
    while (revisions.size() < options.limit
           && git_revwalk_next(&oid, walk.get()) == 0) {
      if (selected_revisions.contains(oid) && matches_paths(oid)) {
        revisions.push_back(oid);
      }
    }
  } else {
    local_branches = log_references(repo, "refs/heads/");
    std::priority_queue<Candidate> frontier;
    std::set<git_oid, OidLess> queued;
    std::uint64_t queue_order = 0;
    const auto queue = [&](const git_oid& oid) {
      if (!queued.insert(oid).second) return;
      CommitPtr value = repo.commit(oid);
      frontier.push({git_commit_committer(value.get())->when.time,
                     ++queue_order, oid});
    };
    for (const LogReference& branch : local_branches) {
      queue(branch.oid);
    }
    const auto workspace = repo.workspace();
    if (workspace.has_value()) queue(*workspace);
    for (const auto& [name, oid] : repo.refs_with_prefix(kVisibleHeadPrefix)) {
      (void)name;
      queue(oid);
    }
    for (const auto& [name, oid] : repo.refs_with_prefix(kWorkspacePrefix)) {
      (void)name;
      queue(oid);
    }
    while (revisions.size() < options.limit && !frontier.empty()) {
      const git_oid oid = frontier.top().oid;
      frontier.pop();
      for (const git_oid& parent : repo.parents(oid)) queue(parent);
      if (matches_paths(oid)) revisions.push_back(oid);
    }
  }
  if (options.count) {
    output << revisions.size() << '\n';
    return;
  }
  if (revisions.empty()) return;
  if (!options.revision.empty()) {
    local_branches = log_references(repo, "refs/heads/");
  }
  if (options.reversed) {
    std::reverse(revisions.begin(), revisions.end());
  }
  repo.set_short_id_scope(revisions);
  bool show_diff = options.patch;
  show_diff |= options.format.summary;
  show_diff |= options.format.stat;
  show_diff |= options.format.types;
  show_diff |= options.format.name_only;
  show_diff |= options.format.git;
  show_diff |= options.format.color_words;
  show_diff |= !options.format.tool.empty();
  show_diff |= options.format.context != 3;
  show_diff |= options.format.ignore_all_space;
  show_diff |= options.format.ignore_space_change;
  std::map<git_oid, std::vector<std::string>, OidLess> tags;
  constexpr std::string_view tag_prefix = "refs/tags/";
  const std::set<git_oid, OidLess> loaded(revisions.begin(), revisions.end());
  std::map<git_oid, std::vector<std::string>, OidLess> branches;
  for (const LogReference& reference : local_branches) {
    if (loaded.contains(reference.oid)) {
      branches[reference.oid].push_back(reference.name);
    }
  }
  for (const LogReference& reference : log_references(repo, tag_prefix)) {
    if (loaded.contains(reference.oid)) {
      tags[reference.oid].push_back(reference.name);
    }
  }
  const auto workspace = repo.workspace();
  for (const git_oid& revision : revisions) {
    const git_oid oid = revision;
    CommitPtr value = repo.commit(oid);
    std::ostringstream content;
    set_output_color_mode(content, output_color_mode(output));
    const bool working = workspace.has_value() && *workspace == oid;
    content << styled_short_commit_id(repo, content, oid, working);
    if (const auto named = branches.find(oid); named != branches.end()) {
      for (const std::string& branch : named->second) {
        content << " " << styled(content, branch, OutputStyle::branch);
      }
    }
    if (const auto tagged = tags.find(oid); tagged != tags.end()) {
      for (const std::string& tag : tagged->second) {
        content << " " << styled(content, tag, OutputStyle::tag);
      }
    }
    const std::string description = first_line(git_commit_message(value.get()));
    if (repo.commit_has_conflicts(oid)) content << " conflict";
    content << " "
            << (description.empty() ? "(no description set)" : description)
            << '\n';
    if (show_diff) {
      render_revision_diff(repo, oid, options.paths, options.format, content);
    }
    output << content.str();
  }
}

void command_metaedit(Repository& repo,
                      const MetaeditCommand& options,
                      std::ostream& output) {
  repo.prepare_command();
  std::vector<std::string> revisions = options.revisions;
  revisions.insert(revisions.end(), options.revision_options.begin(),
                   options.revision_options.end());
  if (revisions.empty()) revisions.emplace_back("@");
  std::set<git_oid, OidLess> selected;
  const std::vector<git_oid> resolved =
      resolve_revision_arguments(repo, revisions);
  selected.insert(resolved.begin(), resolved.end());

  std::optional<std::pair<std::string, std::string>> explicit_author;
  if (options.author_provided) explicit_author = parse_author(options.author);
  std::optional<std::pair<git_time_t, int>> explicit_timestamp;
  if (options.author_timestamp_provided) {
    explicit_timestamp = parse_author_timestamp(options.author_timestamp);
  }
  SignaturePtr configured = repo.signature();
  const auto refs = repo.rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {  // GG_COV_EXCL_BRANCH
      check(pushed, "walk revisions");
    }
  }
  for (const git_oid& oid : selected) {
    check(git_revwalk_push(walk.get(), &oid), "walk selected revisions");
  }

  RewritePlan plan;
  std::size_t modified = 0;
  std::size_t reparented = 0;
  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    const std::vector<git_oid> old_parents = repo.parents(oid);
    std::vector<git_oid> new_parents;
    new_parents.reserve(old_parents.size());
    bool parents_changed = false;
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      const git_oid next = replacement == plan.commits.end()
                               ? parent
                               : replacement->second;
      if (!(next == parent)) parents_changed = true;
      new_parents.push_back(next);
    }
    const bool is_selected = selected.contains(oid);
    if (!is_selected && !parents_changed) continue;

    CommitPtr old = repo.commit(oid);
    std::optional<std::string_view> message;
    bool metadata_changed = false;
    if (is_selected && options.message_provided) {
      const char* old_message = git_commit_message(old.get());
      const std::string_view original =
          old_message == nullptr ? "" : old_message;  // GG_COV_EXCL_BRANCH
      if (options.message != original) {
        message = options.message;
        metadata_changed = true;
      }
    }

    SignaturePtr author;
    const bool edit_author =
        explicit_author.has_value() | options.update_author |
        explicit_timestamp.has_value() | options.update_author_timestamp;
    if (is_selected && edit_author) {
      const git_signature* old_author = git_commit_author(old.get());
      std::string name = old_author->name;
      std::string email = old_author->email;
      git_time_t time = old_author->when.time;
      int offset = old_author->when.offset;
      if (explicit_author.has_value()) {
        name = explicit_author->first;
        email = explicit_author->second;
      } else if (options.update_author) {
        name = configured->name;
        email = configured->email;
      }
      if (explicit_timestamp.has_value()) {
        time = explicit_timestamp->first;
        offset = explicit_timestamp->second;
      } else if (options.update_author_timestamp) {
        time = configured->when.time;
        offset = configured->when.offset;
      }
      author = make_signature(name, email, time, offset);
      if (!same_signature(author.get(), old_author)) {
        metadata_changed = true;
      } else {
        author.reset();
      }
    }

    if (is_selected && !parents_changed && !metadata_changed &&
        !options.force_rewrite) {
      continue;
    }
    SignaturePtr committer;
    if (is_selected) {
      git_time_t time = configured->when.time;
      const git_signature* old_committer = git_commit_committer(old.get());
      SignaturePtr candidate = make_signature(
          configured->name, configured->email, time, configured->when.offset);
      if (same_signature(candidate.get(), old_committer)) ++time;
      committer = make_signature(configured->name, configured->email, time,
                                 configured->when.offset);
    }
    const git_oid rewritten = repo.rewrite_commit(
        oid, new_parents, std::nullopt, message, author.get(), committer.get());
    plan.commits.emplace(oid, rewritten);
    if (is_selected) {
      ++modified;
    } else {
      ++reparented;
    }
  }

  if (plan.commits.empty()) {
    output << "Nothing changed.\n";
    return;
  }
  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  repo.add_alias_updates(plan);
  const auto workspace = repo.workspace();
  if (workspace.has_value()) {
    const git_oid next = plan.commits.contains(*workspace)
                             ? plan.commits.at(*workspace)
                             : *workspace;
    finish_workspace(repo, next, std::move(plan.updates), {},
                     "gg metaedit");
  } else {
    finish_without_workspace(repo, std::move(plan), {}, "gg metaedit");
  }
  output << "Modified " << modified << " revision(s).\n";
  if (reparented > 0) {
    output << "Rebased " << reparented << " descendant revision(s).\n";
  }
}

void command_edit(Repository& repo,
                  const EditCommand& options,
                  std::ostream& output) {
  repo.prepare_command();
  repo.require_current_head();
  const git_oid target = repo.resolve(options.revision);
  const HeadIntent intent = checkout_intent(repo, options.revision);
  finish_workspace(repo, target, {}, {}, "gg edit", intent);
  output << "Working copy now at: " << repo.short_commit_id(target).value;
  if (intent.kind == HeadIntent::Kind::attach) {
    output << " (on branch " << intent.branch.substr(11) << ')';
  }
  output << '\n';
}

void command_describe(Repository& repo,
                      const DescribeCommand& options,
                      std::ostream& output) {
  repo.prepare_command();
  std::vector<std::string> revisions = options.revisions;
  revisions.insert(revisions.end(), options.revision_options.begin(),
                   options.revision_options.end());
  if (revisions.empty()) revisions.emplace_back("@");
  const std::vector<git_oid> selected_values =
      resolve_revision_arguments(repo, revisions);
  const std::set<git_oid, OidLess> selected(selected_values.begin(),
                                            selected_values.end());
  std::optional<std::string> common_message;
  if (options.stdin_value) {
    common_message.emplace(std::istreambuf_iterator<char>(std::cin),
                           std::istreambuf_iterator<char>());
  } else if (options.message_provided) {
    common_message = options.message;
  }
  if (options.editor && common_message.has_value()) {
    common_message = edit_text(repo, *common_message);
  }

  std::map<git_oid, std::string, OidLess> messages;
  for (const git_oid& oid : selected_values) {
    if (common_message.has_value()) {
      messages.emplace(oid, *common_message);
    } else {
      CommitPtr value = repo.commit(oid);
      const char* original = git_commit_message(value.get());
      messages.emplace(
          oid, edit_text(repo, original == nullptr ? "" : original));  // GG_COV_EXCL_BRANCH
    }
  }

  const auto refs = repo.rewrite_refs();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_REVERSE);
  for (const auto& [name, oid] : refs) {
    (void)name;
    const int pushed = git_revwalk_push(walk.get(), &oid);
    if (pushed != GIT_EINVALIDSPEC) {  // GG_COV_EXCL_BRANCH
      check(pushed, "walk revisions");
    }
  }
  for (const git_oid& oid : selected) {
    check(git_revwalk_push(walk.get(), &oid), "walk selected revisions");
  }

  RewritePlan plan;
  std::size_t modified = 0;
  git_oid oid{};
  while (git_revwalk_next(&oid, walk.get()) == 0) {
    const std::vector<git_oid> old_parents = repo.parents(oid);
    std::vector<git_oid> new_parents;
    bool parents_changed = false;
    for (const git_oid& parent : old_parents) {
      const auto replacement = plan.commits.find(parent);
      const git_oid next = replacement == plan.commits.end()
                               ? parent
                               : replacement->second;
      parents_changed |= !(next == parent);
      new_parents.push_back(next);
    }
    const bool is_selected = selected.contains(oid);
    if (!is_selected && !parents_changed) continue;

    CommitPtr value = repo.commit(oid);
    const char* original = git_commit_message(value.get());
    const std::string_view old_message =
        original == nullptr ? "" : original;  // GG_COV_EXCL_BRANCH
    const std::string_view message =
        is_selected ? std::string_view(messages.at(oid)) : old_message;
    if (!parents_changed && message == old_message) continue;
    const git_oid rewritten =
        repo.rewrite_commit(oid, new_parents, std::nullopt, message);
    plan.commits.emplace(oid, rewritten);
    if (is_selected) ++modified;
  }

  if (plan.commits.empty()) {
    output << "Nothing changed.\n";
    return;
  }
  for (const auto& [name, target] : refs) {
    const auto replacement = plan.commits.find(target);
    if (replacement != plan.commits.end()) {
      plan.updates.emplace(name, replacement->second);
    }
  }
  repo.add_alias_updates(plan);
  const auto workspace = repo.workspace();
  if (workspace.has_value()) {
    const git_oid next = plan.commits.contains(*workspace)
                             ? plan.commits.at(*workspace)
                             : *workspace;
    finish_workspace(repo, next, std::move(plan.updates), {}, "gg describe");
  } else {
    finish_without_workspace(repo, std::move(plan), {}, "gg describe");
  }
  output << "Rewrote " << modified << " revision(s).\n";
}

void command_move(Repository& repo,
                  const MovementCommand& options,
                  std::ostream& output) {
  repo.prepare_command();
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  repo.require_current_head();
  std::set<git_oid, OidLess> frontier{*workspace};
  for (std::uint64_t step = 0; step < options.offset; ++step) {
    std::set<git_oid, OidLess> next;
    for (const git_oid& oid : frontier) {
      const auto candidates =
          options.direction == MovementDirection::next ? repo.children(oid)
                                                       : repo.parents(oid);
      for (const git_oid& candidate : candidates) {
        if (options.conflict && !repo.commit_has_conflicts(candidate)) {  // GG_COV_EXCL_BRANCH
          continue;
        }
        next.insert(candidate);
      }
    }
    frontier = std::move(next);
  }
  const std::string direction =
      options.direction == MovementDirection::next ? "next" : "previous";
  if (frontier.empty()) {
    throw UserError("no " + direction + " revision found");
  }
  if (frontier.size() != 1) {
    throw UserError("ambiguous " + direction + " revision");
  }
  const git_oid target = *frontier.begin();

  const git_oid destination = target;
  finish_workspace(repo, destination, {}, {},
                   options.direction == MovementDirection::next ? "gg next"
                                                                : "gg prev");
  output << "Working copy now at: " << repo.short_commit_id(destination).value
         << '\n';
}

}  // namespace gg::detail
