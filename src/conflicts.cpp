// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "repository.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace gg::detail {

std::optional<PendingRewrite> Repository::pending() const {
  const auto pending_oid = ref_target(rewrite_ref_name());
  if (!pending_oid.has_value()) {
    return std::nullopt;
  }
  CommitPtr value = commit(*pending_oid);
  std::istringstream input(git_commit_message(value.get()));
  std::string line;
  if (!std::getline(input, line) || line != "gg-rewrite-v1") {  // GG_COV_EXCL_BRANCH
    throw GitError("invalid pending rewrite");
  }
  PendingRewrite pending{};
  std::string keyword;
  std::string text;
  while (input >> keyword) {
    if (keyword == "operation") {
      input >> text;
      check(git_oid_fromstr(&pending.operation, text.c_str()),
            "parse pending operation");
    } else if (keyword == "arg") {
      input >> std::quoted(text);
      pending.arguments.push_back(text);
    } else if (keyword == "resolution") {
      Resolution resolution{};
      std::array<git_oid*, 4> values{&resolution.ancestor, &resolution.ours,
                                    &resolution.theirs, &resolution.result};
      for (git_oid* oid : values) {
        input >> text;
        check(git_oid_fromstr(oid, text.c_str()), "parse rewrite resolution");
      }
      pending.resolutions.push_back(resolution);
    } else if (keyword == "conflict") {
      std::array<git_oid*, 4> values{&pending.ancestor, &pending.ours,
                                    &pending.theirs, &pending.marker_tree};
      for (git_oid* oid : values) {
        input >> text;
        check(git_oid_fromstr(oid, text.c_str()), "parse rewrite conflict");
      }
    } else if (keyword == "path") {
      input >> std::quoted(text);
      pending.paths.push_back(text);
    } else {
      throw GitError("invalid pending rewrite field");
    }
  }
  return pending;
}

std::string Repository::serialize(const PendingRewrite& pending) const {
  std::ostringstream output;
  output << "gg-rewrite-v1\noperation " << oid_string(pending.operation) << '\n';
  for (const std::string& argument : pending.arguments) {
    output << "arg " << std::quoted(argument) << '\n';
  }
  for (const Resolution& resolution : pending.resolutions) {
    output << "resolution " << oid_string(resolution.ancestor) << ' '
           << oid_string(resolution.ours) << ' '
           << oid_string(resolution.theirs) << ' '
           << oid_string(resolution.result) << '\n';
  }
  output << "conflict " << oid_string(pending.ancestor) << ' '
         << oid_string(pending.ours) << ' ' << oid_string(pending.theirs) << ' '
         << oid_string(pending.marker_tree) << '\n';
  for (const std::string& path : pending.paths) {
    output << "path " << std::quoted(path) << '\n';
  }
  return output.str();
}

void Repository::write_pending(const PendingRewrite& pending) const {
  const git_oid holder =
      create_commit(pending.marker_tree, {pending.operation}, serialize(pending));
  apply_refs({{rewrite_ref_name(), holder}}, {}, "gg pause rewrite");
}

void Repository::pause(const std::vector<std::string_view>& arguments,
           MergeConflict& conflict) const {
  git_checkout_options options = GIT_CHECKOUT_OPTIONS_INIT;
  options.checkout_strategy = GIT_CHECKOUT_FORCE |
                              GIT_CHECKOUT_RECREATE_MISSING |
                              GIT_CHECKOUT_ALLOW_CONFLICTS |
                              GIT_CHECKOUT_CONFLICT_STYLE_MERGE |
                              GIT_CHECKOUT_DONT_UPDATE_INDEX;
  options.ancestor_label = "base";
  options.our_label = "destination";
  options.their_label = "change";
  check(git_checkout_index(repo_.get(), conflict.index.get(), &options),
        "write rewrite conflicts");
  PendingRewrite rewrite = pending().value_or(PendingRewrite{});
  if (rewrite.arguments.empty()) {
    rewrite.operation = ensure_operation();
    for (std::string_view argument : arguments) {
      rewrite.arguments.emplace_back(argument);
    }
  }
  rewrite.ancestor = conflict.ancestor;
  rewrite.ours = conflict.ours;
  rewrite.theirs = conflict.theirs;
  rewrite.paths = conflict.paths;
  rewrite.marker_tree = snapshot_tree(conflict.ours);
  write_pending(rewrite);
}

std::vector<std::string> Repository::prepare_continue() const {
  PendingRewrite rewrite =
      pending().value_or(PendingRewrite{});
  if (rewrite.arguments.empty()) {
    throw UserError("no rewrite is in progress");
  }
  const char* workdir = git_repository_workdir(repo_.get());
  bool markers = false;
  for (const std::string& path : rewrite.paths) {
    std::ifstream file(std::filesystem::path(workdir) / path,
                       std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
    markers = markers || content.find("<<<<<<<") != std::string::npos ||
              content.find("=======") != std::string::npos ||  // GG_COV_EXCL_BRANCH
              content.find(">>>>>>>") != std::string::npos;
  }
  const git_oid resolved = snapshot_tree(rewrite.ours);
  if (markers || resolved == rewrite.marker_tree) {
    throw UserError("rewrite conflicts are not resolved");
  }
  rewrite.resolutions.push_back(
      {rewrite.ancestor, rewrite.ours, rewrite.theirs, resolved});
  rewrite.marker_tree = resolved;
  write_pending(rewrite);
  restore_operation(rewrite.operation);
  return rewrite.arguments;
}

void Repository::finish_rewrite() const {
  const std::string reference = rewrite_ref_name();
  if (ref_target(reference).has_value()) {
    apply_refs({}, {reference}, "gg finish rewrite");
  }
}

void Repository::abort_rewrite() const {
  const auto rewrite = pending();
  if (!rewrite.has_value()) {
    throw UserError("no rewrite is in progress");
  }
  restore_operation(rewrite->operation);
  apply_refs({}, {rewrite_ref_name()}, "gg abort rewrite");
}

void Repository::pending_status(std::ostream& output) const {
  const auto rewrite = pending();
  if (!rewrite.has_value()) {
    return;
  }
  output << "A rewrite is paused with conflicts:\n";
  for (const std::string& path : rewrite->paths) {
    output << "C " << path << '\n';
  }
  output << "Resolve the files, then run `gg continue`, or run `gg abort`.\n";
}


}  // namespace gg::detail
