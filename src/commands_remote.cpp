// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "commands.hpp"

#include <git2.h>
#include <git2/sys/errors.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <utility>

namespace gg::detail {
namespace {

constexpr std::string_view kUndoPrefix = "undo: restore to operation ";
constexpr std::string_view kRedoPrefix = "redo: restore to operation ";

struct RefListItem {
  std::string display_name;
  git_oid oid;
  std::string author_name;
  std::string author_email;
  git_time_t author_date;
  std::string committer_name;
  std::string committer_email;
  git_time_t committer_date;
};

int compare_text(std::string_view left, std::string_view right) {
  return left.compare(right);
}

int compare_date(git_time_t left, git_time_t right) {
  return (left > right) - (left < right);
}

int compare_refs(const RefListItem& left,
                 const RefListItem& right,
                 std::string_view key) {
  if (key == "name") return compare_text(left.display_name, right.display_name);
  if (key == "author-name") {
    return compare_text(left.author_name, right.author_name);
  }
  if (key == "author-email") {
    return compare_text(left.author_email, right.author_email);
  }
  if (key == "author-date") {
    return compare_date(left.author_date, right.author_date);
  }
  if (key == "committer-name") {
    return compare_text(left.committer_name, right.committer_name);
  }
  if (key == "committer-email") {
    return compare_text(left.committer_email, right.committer_email);
  }
  return compare_date(left.committer_date, right.committer_date);
}

RefListItem ref_list_item(std::string display_name,
                          const git_oid& oid,
                          const git_commit* commit) {
  const git_signature* author = git_commit_author(commit);
  const git_signature* committer = git_commit_committer(commit);
  return {std::move(display_name),
          oid,
          author->name,
          author->email,
          author->when.time,
          committer->name,
          committer->email,
          committer->when.time};
}

void sort_refs(std::vector<RefListItem>& items,
               const std::vector<std::string>& requested) {
  const std::vector<std::string> default_sort{"name"};
  const std::vector<std::string>& sort =
      requested.empty() ? default_sort : requested;
  for (auto iterator = sort.rbegin(); iterator != sort.rend(); ++iterator) {
    const bool descending = iterator->ends_with('-');
    const std::string_view key = descending
                                     ? std::string_view(*iterator).substr(
                                           0, iterator->size() - 1)
                                     : std::string_view(*iterator);
    std::stable_sort(items.begin(), items.end(), [&](const auto& left,
                                                     const auto& right) {
      const int comparison = compare_refs(left, right, key);
      return descending ? comparison > 0 : comparison < 0;
    });
  }
}

std::string operation_timestamp(const git_commit* operation) {
  const int offset = git_commit_time_offset(operation);
  const std::chrono::sys_seconds local_time{
      std::chrono::seconds{git_commit_time(operation) + offset * 60}};
  const std::chrono::sys_days day = std::chrono::floor<std::chrono::days>(local_time);
  const std::chrono::year_month_day date{day};
  const std::chrono::hh_mm_ss clock{local_time - day};
  const int absolute_offset = std::abs(offset);
  std::ostringstream output;
  output << std::setfill('0') << std::setw(4) << static_cast<int>(date.year())
         << '-' << std::setw(2) << static_cast<unsigned>(date.month()) << '-'
         << std::setw(2) << static_cast<unsigned>(date.day()) << 'T'
         << std::setw(2) << clock.hours().count() << ':' << std::setw(2)
         << clock.minutes().count() << ':' << std::setw(2)
         << clock.seconds().count()
         << (offset < 0 ? '-' : '+')  // GG_COV_EXCL_BRANCH
         << std::setw(2)  // GG_COV_EXCL_BRANCH
         << absolute_offset / 60 << ':' << std::setw(2) << absolute_offset % 60;
  return output.str();
}

std::string head_text(const HeadState& head) {
  return std::string(head.symbolic ? "symbolic " : "detached ") + head.value;
}

void render_operation_diff(Repository& repo,
                           const git_oid& operation,
                           std::ostream& output) {
  const OperationState after = repo.parse_operation(operation);
  const auto previous = repo.operation_previous(operation);
  const std::optional<OperationState> before =
      previous.has_value()
          ? std::optional<OperationState>{repo.parse_operation(*previous)}
          : std::nullopt;
  if (!before.has_value()) {
    output << styled(output, "  + HEAD " + head_text(after.head),
                     OutputStyle::added)
           << '\n';
  } else if (before->head.symbolic != after.head.symbolic ||
             before->head.value != after.head.value) {
    output << styled(output,
                     "  ~ HEAD " + head_text(before->head) + " -> " +
                         head_text(after.head),
                     OutputStyle::modified)
           << '\n';
  }

  std::set<std::string> names;
  if (before.has_value()) {
    for (const auto& [name, oid] : before->refs) {
      (void)oid;
      names.insert(name);
    }
  }
  for (const auto& [name, oid] : after.refs) {
    (void)oid;
    names.insert(name);
  }
  for (const std::string& name : names) {
    const auto old = before.has_value() ? before->refs.find(name)
                                        : after.refs.end();
    const auto current = after.refs.find(name);
    if (!before.has_value() || old == before->refs.end()) {
      output << styled(output,
                       "  + " + name + " " + oid_string(current->second, 8),
                       OutputStyle::added)
             << '\n';
    } else if (current == after.refs.end()) {
      output << styled(output,
                       "  - " + name + " " + oid_string(old->second, 8),
                       OutputStyle::removed)
             << '\n';
    } else if (!(old->second == current->second)) {
      output << styled(output,
                       "  ~ " + name + " " + oid_string(old->second, 8) +
                           " -> " + oid_string(current->second, 8),
                       OutputStyle::modified)
             << '\n';
    }
  }
}

}  // namespace

void command_bookmark(Repository& repo,
                      const BookmarkCommand& options,
                      std::ostream& output) {
  repo.sync_workspace();
  if (options.action == BookmarkAction::list) {
    if (options.conflicted) return;
    if (!options.template_value.empty()) {
      throw UserError("bookmark templates are not supported yet");
    }

    std::set<git_oid, OidLess> revisions;
    for (const std::string& revision : options.revisions) {
      revisions.insert(repo.resolve(revision));
    }
    std::vector<RefListItem> items;
    for (const auto& [reference, oid] : repo.data_refs()) {
      constexpr std::string_view local_prefix = "refs/heads/";
      constexpr std::string_view remote_prefix = "refs/remotes/";
      const bool local = starts_with(reference, local_prefix);
      if (!local && !starts_with(reference, remote_prefix)) continue;

      std::string name;
      std::string display_name;
      if (local) {
        if (options.tracked || !options.remotes.empty()) continue;
        name = reference.substr(local_prefix.size());
        display_name = name;
      } else {
        if (!options.tracked && !options.all_remotes &&
            options.remotes.empty()) {
          continue;
        }
        const std::string remote_bookmark =
            reference.substr(remote_prefix.size());
        const std::size_t slash = remote_bookmark.find('/');
        if (slash == std::string::npos) continue;  // GG_COV_EXCL_BRANCH
        const std::string remote = remote_bookmark.substr(0, slash);
        name = remote_bookmark.substr(slash + 1);
        if (name == "HEAD") continue;
        if (!options.remotes.empty() &&
            !any_string_pattern_matches(options.remotes, remote)) {
          continue;
        }
        display_name = name + "@" + remote;
      }
      const bool name_matches =
          !options.names.empty() &&
          any_string_pattern_matches(options.names, name);
      const bool revision_matches =
          !revisions.empty() && revisions.contains(oid);
      if ((!options.names.empty() || !revisions.empty()) && !name_matches &&
          !revision_matches) {
        continue;
      }

      CommitPtr commit = repo.commit(oid);
      items.push_back(ref_list_item(display_name, oid, commit.get()));
    }
    sort_refs(items, options.sort);
    for (const RefListItem& item : items) {
      output << styled(output, item.display_name, OutputStyle::bookmark)
             << ": "
             << styled(output, oid_string(item.oid, 8), OutputStyle::commit_id)
             << '\n';
    }
    return;
  }
  if (options.action == BookmarkAction::erase ||
      options.action == BookmarkAction::forget) {
    std::set<std::string> deletes;
    std::set<std::string> matched_names;
    for (const std::string& pattern : options.names) {
      bool matched = false;
      for (const auto& [reference, oid] : repo.data_refs()) {
        (void)oid;
        constexpr std::string_view prefix = "refs/heads/";
        if (!starts_with(reference, prefix)) continue;
        const std::string name = reference.substr(prefix.size());
        if (!string_pattern_matches(pattern, name)) continue;
        matched = true;
        matched_names.insert(name);
        deletes.insert(reference);
      }
      if (!matched) throw UserError("bookmark not found: " + pattern);
    }
    if (options.include_remotes) {
      for (const std::string& name : matched_names) {
        const std::string suffix = "/" + name;
        for (const auto& [remote, oid] : repo.data_refs()) {
          (void)oid;
          if (starts_with(remote, "refs/remotes/") &&
              remote.ends_with(suffix)) {
            deletes.insert(remote);
          }
        }
      }
    }
    repo.record({}, deletes, repo.head_state(),
                options.action == BookmarkAction::erase ? "gg bookmark delete"
                                                        : "gg bookmark forget");
    output << (options.action == BookmarkAction::erase ? "Deleted " : "Forgot ")
           << deletes.size() << " bookmark ref(s).\n";
    return;
  }
  if (options.action == BookmarkAction::rename) {
    if (options.names[0] == options.names[1]) {
      throw UserError("bookmark names must differ");
    }
    const std::string old_reference = "refs/heads/" + options.names[0];
    const std::string new_reference = "refs/heads/" + options.names[1];
    const auto target = repo.ref_target(old_reference);
    if (!target.has_value()) {
      throw UserError("bookmark not found: " + options.names[0]);
    }
    int valid = 0;
    check(git_reference_name_is_valid(&valid, new_reference.c_str()),
          "validate bookmark name");
    if (valid == 0) {
      throw UserError("invalid bookmark name: " + options.names[1]);
    }
    if (!options.overwrite_existing &&
        repo.ref_target(new_reference).has_value()) {
      throw UserError("bookmark already exists: " + options.names[1]);
    }
    repo.record({{new_reference, *target}}, {old_reference}, repo.head_state(),
                "gg bookmark rename");
    output << "Renamed " << options.names[0] << " to " << options.names[1]
           << ".\n";
    return;
  }
  if (options.action == BookmarkAction::advance ||
      options.action == BookmarkAction::move) {
    if (options.action == BookmarkAction::move && options.names.empty() &&
        options.from.empty()) {
      throw UserError("bookmark move requires a name or --from revision");
    }
    const git_oid target =
        repo.resolve(options.revision.empty() ? "@" : options.revision);
    std::set<git_oid, OidLess> sources;
    for (const std::string& revision : options.from) {
      sources.insert(repo.resolve(revision));
    }
    std::vector<std::pair<std::string, git_oid>> matches;
    for (const auto& [reference, oid] : repo.data_refs()) {
      constexpr std::string_view prefix = "refs/heads/";
      if (!starts_with(reference, prefix) || oid == target) continue;
      const std::string name = reference.substr(prefix.size());
      if (!options.names.empty() &&
          !any_string_pattern_matches(options.names, name)) {
        continue;
      }
      if (!sources.empty() && !sources.contains(oid)) continue;
      if (options.action == BookmarkAction::advance &&
          options.names.empty()) {
        const int ancestor = git_graph_descendant_of(repo.raw(), &target, &oid);
        check(ancestor, "select bookmarks to advance");
        if (ancestor == 0) continue;
      }
      matches.emplace_back(name, oid);
    }
    if (options.action == BookmarkAction::advance && options.names.empty()) {
      std::vector<std::pair<std::string, git_oid>> closest;
      for (const auto& candidate : matches) {
        const bool shadowed =
            std::ranges::any_of(matches, [&](const auto& other) {
              if (candidate.second == other.second) return false;
              const int closer = git_graph_descendant_of(
                  repo.raw(), &other.second, &candidate.second);
              check(closer, "select closest bookmarks");
              return closer != 0;
            });
        if (!shadowed) closest.push_back(candidate);
      }
      matches = std::move(closest);
    }
    if (matches.empty()) {
      output << "No bookmarks to update.\n";
      return;
    }
    if (!options.allow_backwards) {
      for (const auto& [name, oid] : matches) {
        const int forward =
            git_graph_descendant_of(repo.raw(), &target, &oid);
        check(forward, "check bookmark movement");
        if (forward == 0) {
          throw UserError("refusing to move bookmark backwards or sideways: " +
                          name);
        }
      }
    }
    std::map<std::string, git_oid> updates;
    for (const auto& [name, oid] : matches) {
      (void)oid;
      updates.emplace("refs/heads/" + name, target);
    }
    const bool advancing = options.action == BookmarkAction::advance;
    repo.record(std::move(updates), {}, repo.head_state(),
                advancing ? "gg bookmark advance" : "gg bookmark move");
    output << (advancing ? "Advanced " : "Moved ") << matches.size()
           << " bookmark(s) to " << oid_string(target, 8) << '\n';
    return;
  }
  const git_oid target =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  std::map<std::string, git_oid> updates;
  for (const std::string& name : options.names) {
    const std::string reference = "refs/heads/" + name;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, reference.c_str()),
          "validate bookmark name");
    if (valid == 0) {
      throw UserError("invalid bookmark name: " + name);
    }
    if (options.action == BookmarkAction::create &&
        repo.ref_target(reference).has_value()) {
      throw UserError("bookmark already exists: " + name);
    }
    const auto current = repo.ref_target(reference);
    if (options.action == BookmarkAction::set && current.has_value() &&
        !(*current == target) && !options.allow_backwards) {
      const int is_descendant =
          git_graph_descendant_of(repo.raw(), &target, &*current);
      check(is_descendant, "check bookmark movement");
      if (is_descendant == 0) {
        throw UserError("refusing to move bookmark backwards: " + name);
      }
    }
    updates.emplace(reference, target);
  }
  repo.record(std::move(updates), {}, repo.head_state(), "gg bookmark");
  for (const std::string& name : options.names) {
    output << (options.action == BookmarkAction::create ? "Created " : "Moved ")
           << name << " at " << oid_string(target, 8) << '\n';
  }
}

void command_tag(Repository& repo,
                 const TagCommand& options,
                 std::ostream& output) {
  repo.sync_workspace();
  const auto tag_target = [&](std::string_view reference)
      -> std::optional<git_oid> {
    git_object* raw_object = nullptr;
    const std::string owned_reference(reference);
    const int lookup =
        git_revparse_single(&raw_object, repo.raw(), owned_reference.c_str());
    if (lookup == GIT_ENOTFOUND) {
      git_error_clear();
      return std::nullopt;
    }
    check(lookup, "read tag");
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
          "resolve tag");
    ObjectPtr commit(raw_commit);
    return *git_object_id(commit.get());
  };

  if (options.action == TagAction::list) {
    if (options.conflicted) return;
    if (!options.template_value.empty()) {
      throw UserError("tag templates are not supported yet");
    }
    std::set<git_oid, OidLess> revisions;
    for (const std::string& revision : options.revisions) {
      revisions.insert(repo.resolve(revision));
    }
    std::vector<RefListItem> items;
    for (const auto& [reference, ref_oid] : repo.data_refs()) {
      (void)ref_oid;
      constexpr std::string_view local_prefix = "refs/tags/";
      const bool local = starts_with(reference, local_prefix);
      if (!local && !starts_with(reference, kRemoteTagPrefix)) continue;

      std::string name;
      std::string display_name;
      if (local) {
        if (options.tracked || !options.remotes.empty()) continue;
        name = reference.substr(local_prefix.size());
        display_name = name;
      } else {
        const bool selected = options.tracked | options.all_remotes |
                              !options.remotes.empty();
        if (!selected) continue;
        const std::string remote_tag =
            reference.substr(kRemoteTagPrefix.size());
        constexpr std::string_view marker = "/tags/";
        const std::size_t separator = remote_tag.find(marker);
        if (separator == std::string::npos) continue;  // GG_COV_EXCL_BRANCH
        const std::string remote = remote_tag.substr(0, separator);
        name = remote_tag.substr(separator + marker.size());
        if (!options.remotes.empty() &&
            !any_string_pattern_matches(options.remotes, remote)) {
          continue;
        }
        display_name = name + "@" + remote;
      }
      const std::optional<git_oid> target = tag_target(reference);
      const bool name_matches =
          !options.names.empty() &&
          any_string_pattern_matches(options.names, name);
      const bool revision_matches =
          !revisions.empty() && revisions.contains(*target);
      if ((!options.names.empty() || !revisions.empty()) && !name_matches &&
          !revision_matches) {
        continue;
      }
      CommitPtr commit = repo.commit(*target);
      items.push_back(ref_list_item(display_name, *target, commit.get()));
    }
    sort_refs(items, options.sort);
    for (const RefListItem& item : items) {
      output << styled(output, item.display_name, OutputStyle::tag) << ": "
             << styled(output, oid_string(item.oid, 8), OutputStyle::commit_id)
             << '\n';
    }
    return;
  }

  if (options.action == TagAction::erase) {
    std::set<std::string> deletes;
    for (const std::string& pattern : options.names) {
      bool matched = false;
      for (const auto& [reference, oid] : repo.data_refs()) {
        (void)oid;
        constexpr std::string_view prefix = "refs/tags/";
        if (!starts_with(reference, prefix)) continue;
        const std::string name = reference.substr(prefix.size());
        if (!string_pattern_matches(pattern, name)) continue;
        matched = true;
        deletes.insert(reference);
      }
      if (!matched) throw UserError("tag not found: " + pattern);
    }
    repo.record({}, deletes, repo.head_state(), "gg tag delete");
    output << "Deleted " << deletes.size() << " tag(s).\n";
    return;
  }

  const git_oid target =
      repo.resolve(options.revision.empty() ? "@" : options.revision);
  std::map<std::string, git_oid> updates;
  for (const std::string& name : options.names) {
    const std::string reference = "refs/tags/" + name;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, reference.c_str()),
          "validate tag name");
    if (valid == 0) {
      throw UserError("invalid tag name: " + name);
    }
    const std::optional<git_oid> current = tag_target(reference);
    if (current.has_value() && !(*current == target) && !options.allow_move) {
      throw UserError("tag already exists at a different revision: " + name);
    }
    updates.emplace(reference, target);
  }
  repo.record(std::move(updates), {}, repo.head_state(), "gg tag set");
  for (const std::string& name : options.names) {
    output << "Set " << name << " at " << oid_string(target, 8) << '\n';
  }
}

int credentials(git_credential** output,
                const char* url,
                const char* username,
                unsigned int allowed,
                void* payload) {
  (void)url;
  (void)payload;
  if ((allowed & GIT_CREDENTIAL_SSH_KEY) != 0 && username != nullptr) {
    return git_credential_ssh_key_from_agent(output, username);
  }
  if ((allowed & GIT_CREDENTIAL_DEFAULT) != 0) {
    return git_credential_default_new(output);
  }
  return GIT_PASSTHROUGH;
}

git_remote_callbacks remote_callbacks() {
  git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
  callbacks.credentials = credentials;
  return callbacks;
}

std::map<std::string, git_oid> advertised_remote_tags(git_remote* remote) {
  git_remote_callbacks callbacks = remote_callbacks();
  check(git_remote_connect(remote, GIT_DIRECTION_FETCH, &callbacks, nullptr,
                           nullptr),
        "connect to remote");
  const git_remote_head** heads = nullptr;
  std::size_t count = 0;
  const int result = git_remote_ls(&heads, &count, remote);
  git_remote_disconnect(remote);
  check(result, "list remote refs");
  std::map<std::string, git_oid> tags;
  constexpr std::string_view prefix = "refs/tags/";
  for (std::size_t index = 0; index < count; ++index) {
    const std::string_view reference(heads[index]->name);
    if (!starts_with(reference, prefix)) continue;
    if (reference.ends_with("^{}")) continue;
    tags.emplace(reference.substr(prefix.size()), heads[index]->oid);
  }
  return tags;
}

int create_clone_remote(git_remote** output,
                        git_repository* repository,
                        const char* default_name,
                        const char* url,
                        void* payload) {
  (void)default_name;
  const auto* name = static_cast<const std::string*>(payload);
  return git_remote_create(output, repository, name->c_str(), url);
}

void command_fetch(Repository& repo,
                   const GitFetchCommand& options,
                   std::ostream& output) {
  repo.sync_workspace();
  const auto validate_names = [](const std::vector<std::string>& names,
                                 std::string_view prefix,
                                 std::string_view kind) {
    for (const std::string& name : names) {
      int valid = 0;
      const std::string reference = std::string(prefix) + name;
      check(git_reference_name_is_valid(&valid, reference.c_str()),
            "validate fetch selector");
      if (valid == 0) {
        throw UserError("invalid " + std::string(kind) + " name: " + name);
      }
    }
  };
  validate_names(options.branches, "refs/heads/", "branch");
  validate_names(options.tags, "refs/tags/", "tag");

  std::vector<std::string> names = options.remotes;
  if (options.all_remotes) {
    git_strarray listed{};
    check(git_remote_list(&listed, repo.raw()), "list remotes");
    for (std::size_t index = 0; index < listed.count; ++index) {
      names.emplace_back(listed.strings[index]);
    }
    git_strarray_dispose(&listed);
    if (names.empty()) throw UserError("no remotes found");
  } else if (names.empty()) {
    names.emplace_back("origin");
  }

  std::map<std::string, git_oid> tracking_updates;
  std::set<std::string> tracking_deletes;
  for (const std::string& name : names) {
    git_remote* raw_remote = nullptr;
    check(git_remote_lookup(&raw_remote, repo.raw(), name.c_str()),
          "find remote");
    RemotePtr remote(raw_remote);
    std::vector<std::string> branches = options.branches;
    const std::string remote_tag_prefix =
        std::string(kRemoteTagPrefix) + name + "/tags/";
    std::set<std::string> tracked_tags;
    for (const auto& [reference, oid] : repo.data_refs()) {
      (void)oid;
      if (starts_with(reference, remote_tag_prefix)) {
        tracked_tags.insert(reference.substr(remote_tag_prefix.size()));
      }
    }
    if (options.tracked) {
      const std::string prefix = "refs/remotes/" + name + "/";
      for (const auto& [reference, oid] : repo.data_refs()) {
        (void)oid;
        if (starts_with(reference, prefix) && reference != prefix + "HEAD") {
          branches.push_back(reference.substr(prefix.size()));
        }
      }
      if (branches.empty() && tracked_tags.empty()) {
        output << "No tracked refs to fetch from " << name << '\n';
        continue;
      }
    }
    const bool default_selection = options.branches.empty() &&
                                   options.tags.empty() && !options.tracked;
    std::map<std::string, git_oid> remote_tags;
    if (default_selection || !options.tags.empty() || !tracked_tags.empty()) {
      remote_tags = advertised_remote_tags(remote.get());
    }
    std::vector<std::string> tags = options.tags;
    if (options.tracked) {
      for (const std::string& tag : tracked_tags) {
        if (remote_tags.contains(tag)) {
          tags.push_back(tag);
        } else {
          tracking_deletes.insert(remote_tag_prefix + tag);
        }
      }
    }
    std::vector<std::string> storage;
    for (const std::string& branch : branches) {
      storage.push_back("+refs/heads/" + branch + ":refs/remotes/" + name +
                        "/" + branch);
    }
    for (const std::string& tag : tags) {
      storage.push_back("+refs/tags/" + tag + ":refs/tags/" + tag);
    }
    std::vector<char*> values;
    for (std::string& refspec : storage) values.push_back(refspec.data());
    git_strarray refspecs{values.data(), values.size()};
    git_fetch_options fetch_options = GIT_FETCH_OPTIONS_INIT;
    fetch_options.callbacks = remote_callbacks();
    fetch_options.prune = GIT_FETCH_PRUNE;
    if (!storage.empty()) {
      fetch_options.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_NONE;
    }
    if (!options.tracked || !storage.empty()) {
      check(git_remote_fetch(remote.get(),
                             storage.empty() ? nullptr : &refspecs,
                             &fetch_options, "gg fetch"),
            "fetch remote");
    }
    if (default_selection) {
      for (const std::string& tag : tracked_tags) {
        if (!remote_tags.contains(tag)) {
          tracking_deletes.insert(remote_tag_prefix + tag);
        }
      }
      tags.clear();
      for (const auto& [tag, oid] : remote_tags) {
        (void)oid;
        tags.push_back(tag);
      }
    }
    for (const std::string& tag : tags) {
      const git_oid advertised = remote_tags.at(tag);
      const auto local = repo.ref_target("refs/tags/" + tag);
      if (!local.has_value()) continue;
      if (!(*local == advertised)) continue;
      tracking_updates[remote_tag_prefix + tag] = advertised;
    }
    output << "Fetched " << name << '\n';
  }
  repo.apply_refs(tracking_updates, tracking_deletes, "track fetched tags");
  repo.record(repo.missing_change_ids(), {}, repo.head_state(), "gg fetch");
}

void command_push(Repository& repo,
                  const GitPushCommand& options,
                  std::ostream& output) {
  repo.sync_workspace();
  if (!options.all && !options.tracked && !options.deleted &&
      options.bookmarks.empty() && options.tags.empty() &&
      options.revisions.empty() && options.changes.empty() &&
      options.named.empty()) {
    throw UserError("push requires a selection option");
  }
  const std::string name = options.remote.empty() ? "origin" : options.remote;
  const auto refs = repo.data_refs();
  std::map<std::string, std::string> updates;
  std::map<std::string, git_oid> local_updates;
  std::set<std::string> remote_deletes;
  const auto target_of = [&](const std::string& revision) {
    git_object* raw_object = nullptr;
    check(git_revparse_single(&raw_object, repo.raw(), revision.c_str()),
          "resolve push target");
    ObjectPtr object(raw_object);
    git_object* raw_commit = nullptr;
    check(git_object_peel(&raw_commit, object.get(), GIT_OBJECT_COMMIT),
          "resolve push commit");
    ObjectPtr commit(raw_commit);
    return *git_object_id(commit.get());
  };
  const auto add = [&](std::string_view kind, const std::string& ref_name) {
    const std::string reference = "refs/" + std::string(kind) + "/" + ref_name;
    if (!repo.ref_target(reference).has_value()) {
      throw UserError(std::string(kind == "heads" ? "bookmark" : "tag") +
                      " not found: " + ref_name);
    }
    updates.emplace(reference, reference);
  };
  for (const std::string& bookmark : options.bookmarks) {
    add("heads", bookmark);
  }
  for (const std::string& tag : options.tags) add("tags", tag);
  std::set<git_oid, OidLess> revisions;
  for (const std::string& revision : options.revisions) {
    revisions.insert(repo.resolve(revision));
  }
  for (const auto& [reference, oid] : refs) {
    (void)oid;
    if ((starts_with(reference, "refs/heads/") ||
         starts_with(reference, "refs/tags/")) &&
        revisions.contains(target_of(reference))) {
      updates.emplace(reference, reference);
    }
  }
  const auto create_bookmark = [&](const std::string& bookmark,
                                   const git_oid& target) {
    const std::string reference = "refs/heads/" + bookmark;
    int valid = 0;
    check(git_reference_name_is_valid(&valid, reference.c_str()),
          "validate push bookmark");
    if (valid == 0) throw UserError("invalid bookmark name: " + bookmark);
    if (repo.ref_target(reference).has_value()) {
      throw UserError("bookmark already exists: " + bookmark);
    }
    updates.emplace(reference, oid_string(target));
    local_updates.emplace(reference, target);
  };
  for (const std::string& revision : options.changes) {
    const git_oid target = repo.resolve(revision);
    std::optional<std::string> id = repo.change_id(target);
    if (!id.has_value()) {
      id = repo.new_change_id();
      local_updates.emplace(std::string(kChangePrefix) + *id, target);
    }
    create_bookmark("push-" + repo.short_change_id(*id), target);
  }
  for (const std::string& named : options.named) {
    const std::size_t equals = named.find('=');
    if (equals == 0 || equals == std::string::npos ||
        equals + 1 == named.size()) {
      throw UserError("--named must be BOOKMARK=REVISION");
    }
    create_bookmark(named.substr(0, equals),
                    repo.resolve(named.substr(equals + 1)));
  }
  if (options.all) {
    for (const auto& [reference, oid] : refs) {
      (void)oid;
      if (starts_with(reference, "refs/heads/") ||
          starts_with(reference, "refs/tags/")) {
        updates.emplace(reference, reference);
      }
    }
  }
  const std::string remote_prefix = "refs/remotes/" + name + "/";
  const std::string remote_tag_prefix =
      std::string(kRemoteTagPrefix) + name + "/tags/";
  if (options.tracked || options.deleted) {
    for (const auto& [reference, oid] : refs) {
      (void)oid;
      std::string local;
      if (starts_with(reference, remote_prefix)) {
        const std::string bookmark = reference.substr(remote_prefix.size());
        if (bookmark == "HEAD") continue;
        local = "refs/heads/" + bookmark;
      } else if (starts_with(reference, remote_tag_prefix)) {
        local = "refs/tags/" + reference.substr(remote_tag_prefix.size());
      } else {
        continue;
      }
      if (options.tracked && repo.ref_target(local).has_value()) {
        updates.emplace(local, local);
      }
      if (options.deleted && !repo.ref_target(local).has_value()) {
        updates.emplace(local, "");
        remote_deletes.insert(reference);
      }
    }
  }

  std::map<std::string, git_oid> targets;
  for (const auto& [destination, source] : updates) {
    if (source.empty()) continue;
    const git_oid target = target_of(source);
    targets.emplace(destination, target);
    if (!options.allow_empty_description) {
      CommitPtr commit = repo.commit(target);
      if (first_line(git_commit_message(commit.get())).empty()) {
        throw UserError("refusing to push an empty description: " + source);
      }
    }
  }

  git_remote* raw_remote = nullptr;
  check(git_remote_lookup(&raw_remote, repo.raw(), name.c_str()), "find remote");
  RemotePtr remote(raw_remote);
  if (updates.empty()) {
    output << "No refs to push.\n";
    return;
  }

  std::vector<std::string> storage;
  for (const auto& [destination, source] : updates) {
    storage.push_back(source + ":" + destination);
    output << (options.dry_run ? "Would push " : "Pushing ") << destination
           << " to " << name << '\n';
  }
  if (options.dry_run) return;
  std::vector<char*> values;
  for (std::string& refspec : storage) values.push_back(refspec.data());
  git_strarray refspecs{values.data(), values.size()};
  std::vector<char*> push_option_values;
  for (const std::string& option : options.options) {
    push_option_values.push_back(const_cast<char*>(option.c_str()));
  }
  git_strarray push_options_array{push_option_values.data(),
                                  push_option_values.size()};
  git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
  push_options.callbacks = remote_callbacks();
  push_options.remote_push_options = push_options_array;
  check(git_remote_push(remote.get(), &refspecs, &push_options),
        "push refs");
  for (const auto& [destination, target] : targets) {
    constexpr std::string_view head_prefix = "refs/heads/";
    constexpr std::string_view tag_prefix = "refs/tags/";
    if (starts_with(destination, head_prefix)) {
      local_updates.emplace(
          remote_prefix + destination.substr(head_prefix.size()), target);
    } else {
      local_updates.emplace(
          remote_tag_prefix + destination.substr(tag_prefix.size()),
          *repo.ref_target(destination));
    }
  }
  repo.record(std::move(local_updates), std::move(remote_deletes),
              repo.head_state(), "gg push");
}

void command_undo(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  const auto current = repo.operation();
  if (!current.has_value()) {
    throw UserError("nothing to undo");
  }
  const git_oid target =
      repo.operation_target(*current, kUndoPrefix).value_or(*current);
  auto previous = repo.operation_previous(target);
  if (!previous.has_value()) {
    throw UserError("nothing to undo");
  }
  previous = repo.operation_target(*previous, kUndoPrefix).value_or(*previous);
  repo.restore_operation(
      *previous, std::string(kUndoPrefix) + oid_string(*previous));
  output << "Undid operation.\n";
}

void command_redo(Repository& repo, std::ostream& output) {
  repo.sync_workspace();
  const auto current = repo.operation();
  if (!current.has_value()) {
    throw UserError("nothing to redo");
  }
  const git_oid target =
      repo.operation_target(*current, kRedoPrefix).value_or(*current);
  if (!repo.operation_target(target, kUndoPrefix).has_value()) {
    throw UserError("nothing to redo");
  }
  auto restored = repo.operation_previous(target);
  if (!restored.has_value()) {  // GG_COV_EXCL_BRANCH
    throw GitError("undo operation has no predecessor");
  }
  restored = repo.operation_target(*restored, kRedoPrefix).value_or(*restored);
  repo.restore_operation(
      *restored, std::string(kRedoPrefix) + oid_string(*restored));
  output << "Redid operation.\n";
}

void command_operation_log(Repository& repo,
                           const OperationLogCommand& options,
                           std::ostream& output) {
  repo.sync_workspace();
  auto current = repo.operation();
  if (!current.has_value()) {
    output << "No operations.\n";
    return;
  }
  if (!options.template_value.empty()) {
    throw UserError("operation templates are not supported yet");
  }
  const git_oid newest = *current;
  std::vector<git_oid> operations;
  while (current.has_value() && operations.size() < options.limit) {
    operations.push_back(*current);
    current = repo.operation_previous(*current);
  }
  if (options.reversed) {
    std::reverse(operations.begin(), operations.end());
  }
  for (std::size_t index = 0; index < operations.size(); ++index) {
    const git_oid& oid = operations[index];
    CommitPtr operation = repo.commit(oid);
    const std::string description = repo.operation_description(oid);
    if (!options.no_graph) {
      output << styled(output, oid == newest ? "@" : "○",
                       oid == newest ? OutputStyle::working_copy
                                     : OutputStyle::change_id)
             << ' ';
    }
    output << styled(output, oid_string(oid, 8),
                     oid == newest ? OutputStyle::current_operation_id
                                   : OutputStyle::operation_id)
           << ' '
           << styled(output, operation_timestamp(operation.get()),
                     OutputStyle::timestamp)
           << ' ' << description << '\n';
    if (options.op_diff) render_operation_diff(repo, oid, output);
    if (!options.no_graph && index + 1 < operations.size()) {
      output << "│\n";
    }
  }
}

void command_operation_restore(Repository& repo,
                               const OperationRestoreCommand& options,
                               std::ostream& output) {
  repo.sync_workspace();
  const git_oid operation = repo.resolve_operation(options.operation);
  const bool all = options.what.empty();
  const bool restore_repository =
      all || std::find(options.what.begin(), options.what.end(), "repo") !=
                 options.what.end();
  const bool restore_remote_tracking =
      all || std::find(options.what.begin(), options.what.end(),
                       "remote-tracking") != options.what.end();
  repo.restore_operation(operation,
                         "restore to operation " + oid_string(operation),
                         restore_repository, restore_remote_tracking);
  output << "Restored operation " << oid_string(operation, 8) << ".\n";
}

int clone_command(const GitCloneCommand& options, std::ostream& output) {
  if (options.object_hash == "sha256") {
    throw UserError("gg does not support SHA-256 repositories");
  }
  const auto validate_names = [](const std::vector<std::string>& names,
                                 std::string_view prefix,
                                 std::string_view kind) {
    for (const std::string& name : names) {
      int valid = 0;
      const std::string reference = std::string(prefix) + name;
      check(git_reference_name_is_valid(&valid, reference.c_str()),
            "validate clone selector");
      if (valid == 0) {
        throw UserError("invalid " + std::string(kind) + " name: " + name);
      }
    }
  };
  validate_names(options.branches, "refs/heads/", "branch");
  validate_names(options.tags, "refs/tags/", "tag");
  std::string destination = options.destination;
  if (destination.empty()) {
    destination = std::filesystem::path(options.url).filename().string();
    if (destination.ends_with(".git")) {
      destination.resize(destination.size() - 4);
    }
  }
  const bool destination_existed = std::filesystem::exists(destination);
  git_clone_options clone_options = GIT_CLONE_OPTIONS_INIT;
  clone_options.checkout_opts.checkout_strategy = GIT_CHECKOUT_NONE;
  clone_options.fetch_opts.callbacks = remote_callbacks();
  clone_options.fetch_opts.depth = options.depth;
  if (options.depth != 0) clone_options.local = GIT_CLONE_NO_LOCAL;
  if (!options.branches.empty()) {
    clone_options.checkout_branch = options.branches.front().c_str();
  }
  std::string remote = options.remote;
  clone_options.remote_cb = create_clone_remote;
  clone_options.remote_cb_payload = &remote;
  git_repository* raw_repository = nullptr;
  check(git_clone(&raw_repository, options.url.c_str(), destination.c_str(),
                  &clone_options),
        "clone repository");
  RepositoryPtr repository(raw_repository);
  repository.reset();
  try {
    Repository repo(destination);
    const std::string remote_prefix = "refs/remotes/" + options.remote + "/";
    for (const std::string& branch : options.branches) {
      if (!repo.ref_target(remote_prefix + branch).has_value()) {
        throw UserError("branch not found in clone source: " + branch);
      }
    }
    for (const std::string& tag : options.tags) {
      if (!repo.ref_target("refs/tags/" + tag).has_value()) {
        throw UserError("tag not found in clone source: " + tag);
      }
    }
    if (!options.branches.empty() || !options.tags.empty()) {
      std::set<std::string> deletes;
      for (const auto& [reference, oid] : repo.data_refs()) {
        (void)oid;
        if (starts_with(reference, remote_prefix)) {
          const std::string name = reference.substr(remote_prefix.size());
          if (name == "HEAD" ||
              std::find(options.branches.begin(), options.branches.end(), name) ==
                  options.branches.end()) {
            deletes.insert(reference);
          }
        } else if (starts_with(reference, "refs/heads/")) {
          const std::string name =
              reference.substr(std::string_view("refs/heads/").size());
          if (std::find(options.branches.begin(), options.branches.end(), name) ==
              options.branches.end()) {
            deletes.insert(reference);
          }
        } else {
          // Before gg initialization, the only remaining clone refs are tags.
          const std::string name =
              reference.substr(std::string_view("refs/tags/").size());
          if (std::find(options.tags.begin(), options.tags.end(), name) ==
              options.tags.end()) {
            deletes.insert(reference);
          }
        }
      }
      repo.apply_refs({}, deletes, "filter clone refs");
      if (options.branches.empty()) {
        repo.set_head({true, "refs/heads/main"});
      }
    }
    std::map<std::string, git_oid> tracked_tags;
    for (const auto& [reference, oid] : repo.data_refs()) {
      constexpr std::string_view tag_prefix = "refs/tags/";
      if (starts_with(reference, tag_prefix)) {
        tracked_tags.emplace(std::string(kRemoteTagPrefix) + options.remote +
                                 "/tags/" +
                                 reference.substr(tag_prefix.size()),
                             oid);
      }
    }
    repo.apply_refs(tracked_tags, {}, "track cloned tags");
    command_new(repo, NewCommand{}, output);
  } catch (...) {
    std::error_code error;
    std::filesystem::remove_all(destination, error);
    if (destination_existed) {
      std::filesystem::create_directories(destination, error);
    }
    throw;
  }
  output << "Cloned into " << destination << '\n';
  return 0;
}

int init_command(const GitInitCommand& options, std::ostream& output) {
  git_repository_init_options init_options = GIT_REPOSITORY_INIT_OPTIONS_INIT;
  init_options.flags = GIT_REPOSITORY_INIT_MKPATH;
  if (options.object_hash == "sha256") {
    throw UserError("gg does not support SHA-256 repositories");
  }
  const std::filesystem::path destination =
      options.destination.empty() ? "." : options.destination;
  git_repository* raw_repository = nullptr;
  check(git_repository_init_ext(&raw_repository, destination.string().c_str(),
                                &init_options),
        "initialize repository");
  RepositoryPtr initialized(raw_repository);
  initialized.reset();

  Repository repo(destination);
  output << "Initialized repository at "
         << std::filesystem::weakly_canonical(destination).string() << '\n';
  if (!repo.ref_target(kWorkspaceRef).has_value()) {
    command_new(repo, NewCommand{}, output);
  }
  return 0;
}

}  // namespace gg::detail
