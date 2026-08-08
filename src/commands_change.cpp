// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <regex>
#include <set>
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

std::pair<git_time_t, int> parse_rfc3339_timestamp(std::string_view value) {
  if (value.size() < 20) {
    throw UserError("author timestamp must use RFC 3339 or RFC 2822 form");
  }
  std::tm fields{};
  std::string owned(value);
  char* suffix = strptime(owned.c_str(), "%Y-%m-%dT%H:%M:%S", &fields);
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
  const std::time_t local = timegm(&fields);
  return {static_cast<git_time_t>(local) - offset * 60, offset};
}

std::pair<git_time_t, int> parse_rfc2822_timestamp(std::string_view value) {
  std::tm fields{};
  std::string owned(value);
  char* zone = strptime(owned.c_str(), "%a, %d %b %Y %H:%M:%S ", &fields);
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
  const std::time_t local = timegm(&fields);
  return {static_cast<git_time_t>(local) - offset * 60, offset};
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

std::string graph_prefix(std::vector<git_oid>& lanes,
                         const git_oid& revision,
                         const std::vector<git_oid>& successors,
                         std::string_view marker) {
  auto current = std::find_if(lanes.begin(), lanes.end(),
                              [&](const git_oid& oid) { return oid == revision; });
  if (current == lanes.end()) {
    lanes.push_back(revision);
    current = std::prev(lanes.end());
  }
  const std::size_t lane = std::distance(lanes.begin(), current);
  std::optional<std::size_t> convergence;
  if (successors.size() == 1) {
    const auto existing = std::find_if(
        lanes.begin(), lanes.end(),
        [&](const git_oid& oid) { return oid == successors.front(); });
    if (existing != lanes.end()) {
      convergence = std::distance(lanes.begin(), existing);
    }
  }

  std::ostringstream prefix;
  if (convergence.has_value()) {
    const std::size_t first = std::min(lane, *convergence);
    const std::size_t last = std::max(lane, *convergence);
    for (std::size_t index = 0; index < first; ++index) {
      prefix << "│ ";
    }
    prefix << "╰";
    for (std::size_t index = first; index < last; ++index) prefix << "─";
    prefix << marker;
  } else {
    for (std::size_t index = 0; index < lane; ++index) {
      prefix << "│ ";
    }
    prefix << marker;
    if (successors.size() > 1) {
      for (std::size_t index = 1; index < successors.size(); ++index) {
        prefix << "─" << (index + 1 == successors.size() ? "╮" : "┬");
      }
    }
  }
  prefix << "  ";
  for (std::size_t index = lane + 1; index < lanes.size(); ++index) {
    prefix << "│ ";
  }

  lanes.erase(current);
  std::size_t insertion = std::min(lane, lanes.size());
  for (const git_oid& successor : successors) {
    const bool present = std::ranges::any_of(
        lanes, [&](const git_oid& oid) { return oid == successor; });
    if (!present) {
      lanes.insert(lanes.begin() + insertion, successor);
      ++insertion;
    }
  }
  return prefix.str();
}

}  // namespace

void command_new(Repository& repo,
                 const NewCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
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
  const git_oid change =
      repo.create_commit(combined_tree(repo, parents), parents, options.message);
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
  }
  auto missing_ids = repo.missing_change_ids();
  std::string id;
  do {
    id = repo.new_change_id();
  } while (missing_ids.contains(std::string(kChangePrefix) + id));  // GG_COV_EXCL_BRANCH
  for (auto& [reference, oid] : missing_ids) {
    if (plan.commits.contains(oid)) oid = plan.commits.at(oid);
    plan.updates.emplace(reference, oid);
  }
  plan.updates[std::string(kChangePrefix) + id] = change;
  if (!options.no_edit) {
    finish_workspace(repo, change, std::move(plan.updates), {}, "gg new");
  } else if (old_workspace.has_value()) {
    const git_oid workspace = plan.commits.contains(*old_workspace)
                                  ? plan.commits.at(*old_workspace)
                                  : *old_workspace;
    finish_workspace(repo, workspace, std::move(plan.updates), {}, "gg new");
  } else {
    repo.record(std::move(plan.updates), {}, repo.head_state(), "gg new");
  }
  output << (options.no_edit ? "Created change: " : "Working copy now at: ")
         << repo.short_change_id(id) << ' '
         << oid_string(change, 8) << ' '
         << (options.message.empty() ? "(no description set)" : options.message)
         << '\n';
}

void command_commit(Repository& repo,
                    const CommitCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  std::vector<std::string> paths;
  bool select_all = false;
  for (const std::string& path : options.paths) {
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

  CommitPtr current = repo.commit(*workspace);
  const std::vector<git_oid> parents = repo.parents(*workspace);
  const git_oid base_tree = combined_tree(repo, parents);
  const git_oid full_tree = *git_commit_tree_id(current.get());
  git_oid selected_tree =
      options.paths.empty() || select_all
          ? full_tree
          : repo.selected_tree(base_tree, full_tree, paths);
  if (options.interactive || !options.tool.empty()) {
    selected_tree = select_diff_tree(
        repo, base_tree, selected_tree,
        options.paths.empty() || select_all ? std::vector<std::string>{}
                                            : paths,
        options.tool);
  }
  const char* old_message = git_commit_message(current.get());
  std::string message = options.message_provided
                            ? options.message
                            : (old_message == nullptr ? "" : old_message);  // GG_COV_EXCL_BRANCH
  if (options.editor) message = edit_text(message);
  const git_oid committed =
      repo.rewrite_commit(*workspace, parents, selected_tree, message);
  const git_oid new_workspace = repo.create_commit(full_tree, {committed}, "");
  const std::string id = repo.new_change_id();
  RewritePlan plan = repo.descendants({{*workspace, committed}});
  plan.updates[std::string(kChangePrefix) + id] = new_workspace;
  finish_workspace(repo, new_workspace, std::move(plan.updates), {},
                   "gg commit");
  output << "Committed as " << oid_string(committed, 8) << '\n'
         << "Working copy now at: " << repo.short_change_id(id) << ' '
         << oid_string(new_workspace, 8) << '\n';
}

void command_status(Repository& repo,
                    const StatusCommand& options,
                    std::ostream& output) {
  repo.sync_workspace();
  std::vector<std::string> paths;
  for (const std::string& value : options.paths) {
    const std::filesystem::path path(value);
    if (value.empty() || path.is_absolute()) {
      throw UserError("status paths must be repository-relative");
    }
    for (const auto& component : path) {
      if (component == "..") {
        throw UserError("status paths must not contain '..'");
      }
    }
    const std::string normalized = path.lexically_normal().generic_string();
    if (normalized != ".") {
      paths.push_back(normalized);
    }
  }
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    output << "No working-copy change. Run `gg new` to create one.\n";
    return;
  }
  CommitPtr change = repo.commit(*workspace);
  const auto id = repo.change_id(*workspace);
  output << "Working copy (@): "
         << styled(output,
                   id.has_value() ? repo.short_change_id(*id) : "--------",
                   OutputStyle::working_change_id)
         << ' '
         << styled(output, oid_string(*workspace, 8),
                   OutputStyle::working_commit_id)
         << ' ';
  const std::string description = first_line(git_commit_message(change.get()));
  output << (description.empty() ? "(no description set)" : description) << '\n';
  const auto parents = repo.parents(*workspace);
  git_oid base_tree_oid{};
  if (!parents.empty()) {
    CommitPtr parent = repo.commit(parents.front());
    output << "Parent commit (@-): "
           << styled(output, oid_string(parents.front(), 8),
                     OutputStyle::commit_id)
           << ' '
           << first_line(git_commit_message(parent.get())) << '\n';
    base_tree_oid = *git_commit_tree_id(parent.get());
  } else {
    output << "Root working-copy change.\n";
    base_tree_oid = repo.empty_tree();
  }

  git_diff* raw_diff = nullptr;
  TreePtr base_tree = repo.tree(base_tree_oid);
  TreePtr change_tree = repo.tree(*git_commit_tree_id(change.get()));
  check(git_diff_tree_to_tree(&raw_diff, repo.raw(), base_tree.get(),
                              change_tree.get(), nullptr),
        "compare working change");
  DiffPtr diff(raw_diff);
  git_diff_find_options find_options = GIT_DIFF_FIND_OPTIONS_INIT;
  check(git_diff_find_similar(diff.get(), &find_options), "find renamed files");
  std::vector<const git_diff_delta*> deltas;
  for (std::size_t index = 0; index < git_diff_num_deltas(diff.get()); ++index) {
    const git_diff_delta* delta = git_diff_get_delta(diff.get(), index);
    const auto selected = [&](const char* raw_path) {
      const std::string_view path = raw_path;
      if (paths.empty()) {
        return true;
      }
      for (const std::string& prefix : paths) {
        if (path == prefix) {
          return true;
        }
        if (path.starts_with(prefix + "/")) {
          return true;
        }
      }
      return false;
    };
    const bool old_selected = selected(delta->old_file.path);
    const bool new_selected = selected(delta->new_file.path);
    if (old_selected | new_selected) {
      deltas.push_back(delta);
    }
  }
  if (deltas.empty()) {
    output << "The working copy has no changes.\n";
  } else {
    output << "Working copy changes:\n";
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
  }
}

void command_log(Repository& repo,
                 const LogCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
  git_revwalk* raw_walk = nullptr;
  check(git_revwalk_new(&raw_walk, repo.raw()), "walk revisions");
  RevwalkPtr walk(raw_walk);
  git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
  std::optional<std::set<git_oid, OidLess>> selected_revisions;
  if (!options.revision.empty()) {
    const std::vector<git_oid> selected = repo.resolve_set(options.revision);
    selected_revisions.emplace(selected.begin(), selected.end());
    for (const git_oid& oid : selected) {
      check(git_revwalk_push(walk.get(), &oid), "walk revisions");
    }
  } else {
    const auto workspace = repo.workspace();
    if (workspace.has_value()) {
      check(git_revwalk_push(walk.get(), &*workspace), "walk revisions");
    }
    for (const auto& [name, oid] : repo.data_refs()) {
      if (starts_with(name, "refs/heads/")) {
        check(git_revwalk_push(walk.get(), &oid), "walk revisions");
      }
    }
  }
  const auto workspace = repo.workspace();
  std::vector<git_oid> revisions;
  git_oid oid{};
  while (revisions.size() < options.limit &&
         git_revwalk_next(&oid, walk.get()) == 0) {
    if (selected_revisions.has_value() &&
        !selected_revisions->contains(oid)) {
      continue;
    }
    if (!options.paths.empty() &&
        !revision_matches_paths(repo, oid, options.paths, options.format)) {
      continue;
    }
    revisions.push_back(oid);
  }
  if (options.count) {
    output << revisions.size() << '\n';
    return;
  }
  if (options.reversed) {
    std::reverse(revisions.begin(), revisions.end());
  }
  std::map<git_oid, std::vector<git_oid>, OidLess> graph_successors;
  if (options.reversed) {
    const std::set<git_oid, OidLess> visible(revisions.begin(), revisions.end());
    for (const git_oid& child : revisions) {
      for (const git_oid& parent : repo.parents(child)) {
        if (visible.contains(parent)) {
          graph_successors[parent].push_back(child);
        }
      }
    }
  } else {
    for (const git_oid& revision : revisions) {
      graph_successors.emplace(revision, repo.parents(revision));
    }
  }
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
  std::vector<git_oid> graph_lanes;
  for (const git_oid& revision : revisions) {
    const git_oid oid = revision;
    CommitPtr value = repo.commit(oid);
    const auto id = repo.change_id(oid);
    const auto bookmarks = repo.bookmarks(oid);
    if (!options.no_graph) {
      const bool working = workspace.has_value() && *workspace == oid;
      const std::string marker =
          working ? styled(output, "@", OutputStyle::working_copy)
          : id.has_value() ? styled(output, "○", OutputStyle::change_id)
                           : "*";
      output << graph_prefix(graph_lanes, oid, graph_successors[oid], marker);
    }
    if (!options.template_value.empty()) {
      output << render_template(options.template_value,
                                revision_template_values(repo, oid));
    } else {
      const bool working = workspace.has_value() && *workspace == oid;
      output << styled(output,
                       id.has_value() ? repo.short_change_id(*id)
                                      : oid_string(oid, 8),
                       working ? OutputStyle::working_change_id
                               : id.has_value() ? OutputStyle::change_id
                                                : OutputStyle::commit_id)
             << ' '
             << styled(output, oid_string(oid, 8),
                       working ? OutputStyle::working_commit_id
                               : OutputStyle::commit_id);
      for (const std::string& bookmark : bookmarks) {
        output << " " << styled(output, bookmark, OutputStyle::bookmark);
      }
      const std::string description =
          first_line(git_commit_message(value.get()));
      output << " "
             << (description.empty() ? "(no description set)" : description)
             << '\n';
    }
    if (show_diff) {
      render_revision_diff(repo, oid, options.paths, options.format, output);
    }
  }
}

void command_metaedit(Repository& repo,
                      const MetaeditCommand& options,
                      std::ostream& output) {
  repo.sync_workspace();
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
    bool metadata_changed = options.update_change_id;
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
  std::set<std::string> deletes;
  if (options.update_change_id) {
    for (const git_oid& old : selected) {
      for (const auto& [name, target] : refs) {
        if (target == old && starts_with(name, kChangePrefix)) {
          plan.updates.erase(name);
          deletes.insert(name);
        }
      }
      const git_oid target = plan.commits.at(old);
      plan.updates[std::string(kChangePrefix) + repo.new_change_id()] = target;
    }
  }
  const auto workspace = repo.workspace();
  if (workspace.has_value()) {
    const git_oid next = plan.commits.contains(*workspace)
                             ? plan.commits.at(*workspace)
                             : *workspace;
    finish_workspace(repo, next, std::move(plan.updates), std::move(deletes),
                     "gg metaedit");
  } else {
    repo.record(std::move(plan.updates), std::move(deletes), repo.head_state(),
                "gg metaedit");
  }
  output << "Modified " << modified << " revision(s).\n";
  if (reparented > 0) {
    output << "Rebased " << reparented << " descendant revision(s).\n";
  }
}

void command_edit(Repository& repo,
                  const EditCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  const git_oid target = repo.resolve(options.revision);
  std::map<std::string, git_oid> updates;
  auto id = repo.change_id(target);
  if (!id.has_value()) {
    id = repo.new_change_id();
    updates[std::string(kChangePrefix) + *id] = target;
  }
  finish_workspace(repo, target, std::move(updates), {}, "gg edit");
  output << "Working copy now at: " << repo.short_change_id(*id) << ' '
         << oid_string(target, 8) << '\n';
}

void command_describe(Repository& repo,
                      const DescribeCommand& options,
                      std::ostream& output) {
  repo.sync_workspace();
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
    common_message = edit_text(*common_message);
  }

  std::map<git_oid, std::string, OidLess> messages;
  for (const git_oid& oid : selected_values) {
    if (common_message.has_value()) {
      messages.emplace(oid, *common_message);
    } else {
      CommitPtr value = repo.commit(oid);
      const char* original = git_commit_message(value.get());
      messages.emplace(
          oid, edit_text(original == nullptr ? "" : original));  // GG_COV_EXCL_BRANCH
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
  const auto workspace = repo.workspace();
  if (workspace.has_value()) {
    const git_oid next = plan.commits.contains(*workspace)
                             ? plan.commits.at(*workspace)
                             : *workspace;
    finish_workspace(repo, next, std::move(plan.updates), {}, "gg describe");
  } else {
    repo.record(std::move(plan.updates), {}, repo.head_state(), "gg describe");
  }
  output << "Rewrote " << modified << " revision(s).\n";
}

void command_move(Repository& repo,
                  const MovementCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  const auto workspace = repo.workspace();
  if (!workspace.has_value()) {
    throw UserError("this command requires a working-copy change");
  }
  if (options.conflict) {
    throw UserError("gg has no first-class conflicted revisions");
  }
  bool edit = options.edit;
  if (!edit && !options.no_edit) {
    const std::string configured =
        config_value(repo, "ui.movement.edit").value_or("false");
    if (configured == "true") {
      edit = true;
    } else if (configured != "false") {
      throw UserError("ui.movement.edit must be true or false");
    }
  }
  if (!edit && !repo.children(*workspace).empty()) {
    throw UserError(
        "the working-copy change has children; create a new change or use --edit");
  }

  std::set<git_oid, OidLess> frontier;
  if (edit) {
    frontier.insert(*workspace);
  } else {
    const auto parents = repo.parents(*workspace);
    frontier.insert(parents.begin(), parents.end());
  }
  for (std::uint64_t step = 0; step < options.offset; ++step) {
    std::set<git_oid, OidLess> next;
    for (const git_oid& oid : frontier) {
      const auto candidates =
          options.direction == MovementDirection::next ? repo.children(oid)
                                                       : repo.parents(oid);
      for (const git_oid& candidate : candidates) {
        if (!(options.direction == MovementDirection::next && !edit &&  // GG_COV_EXCL_BRANCH
              step == 0 && candidate == *workspace)) {
          next.insert(candidate);
        }
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

  std::map<std::string, git_oid> updates;
  git_oid destination = target;
  std::string id;
  if (edit) {
    const auto existing = repo.change_id(target);
    id = existing.value_or(repo.new_change_id());
    if (!existing.has_value()) {
      updates[std::string(kChangePrefix) + id] = target;
    }
  } else {
    id = repo.new_change_id();
    CommitPtr target_commit = repo.commit(target);
    destination = repo.create_commit(*git_commit_tree_id(target_commit.get()),
                                     {target}, "");
    updates[std::string(kChangePrefix) + id] = destination;
  }
  finish_workspace(repo, destination, std::move(updates), {},
                   options.direction == MovementDirection::next ? "gg next"
                                                                : "gg prev");
  output << "Working copy now at: " << repo.short_change_id(id) << ' '
         << oid_string(destination, 8) << '\n';
}

}  // namespace gg::detail
