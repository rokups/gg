// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"

#include <algorithm>

namespace gg::test {

TEST_F(RepositoryTest, ManagesBranchesAndRejectsInvalidRequests) {
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  EXPECT_EQ(invoke({"branch", "create", "topic"}).code, 0);
  EXPECT_EQ(invoke({"branch", "create", "forward", "-r", "main"}).code, 0);
  EXPECT_EQ(invoke({"branch", "set", "forward", "-r", "@"}).code, 0);
  EXPECT_EQ(invoke({"branch", "set", "forward", "-r", "@"}).code, 0);
  EXPECT_EQ(invoke({"branch", "create", "one", "two", "--to", "main"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/one"));
  EXPECT_TRUE(has_ref("refs/heads/two"));
  EXPECT_NE(invoke({"branch", "list"}).output.find("topic:"),
            std::string::npos);
  EXPECT_EQ(invoke({"branch", "create", "topic"}).code, 2);
  EXPECT_EQ(invoke({"branch", "create", "bad name"}).code, 2);
  EXPECT_EQ(invoke({"branch", "create", "valid", "bad name"}).code, 2);
  EXPECT_FALSE(has_ref("refs/heads/valid"));
  EXPECT_EQ(invoke({"branch", "set", "topic", "three", "-r", "main"})
                .code,
            2);
  EXPECT_EQ(invoke({"branch", "set", "topic", "three", "-B", "-r", "main"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/three"));
  EXPECT_EQ(invoke({"branch", "delete", "missing"}).code, 2);
  EXPECT_EQ(invoke({"branch", "delete", "glob:t*", "one", "forward"})
                .code,
            0);
}

TEST_F(RepositoryTest, ListsFilteredRemoteAndSortedBranches) {
  ASSERT_EQ(invoke({"new", "-m", "first", main_id()}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "alpha"}).code, 0);
  const git_oid first = ref("refs/heads/alpha");
  ASSERT_EQ(invoke({"new", "-d", "-m", "second"}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "beta"}).code, 0);
  const git_oid second = ref("refs/heads/beta");
  set_ref("refs/remotes/origin/alpha", first);
  set_ref("refs/remotes/origin/HEAD", second);
  set_ref("refs/remotes/origin/remote-only", second);
  set_ref("refs/remotes/backup/beta", second);

  const Result local = invoke({"branch", "list"});
  ASSERT_EQ(local.code, 0) << local.error;
  EXPECT_NE(local.output.find("alpha:"), std::string::npos);
  EXPECT_NE(local.output.find("beta:"), std::string::npos);
  EXPECT_EQ(local.output.find("@origin"), std::string::npos);
  const Result colored =
      invoke({"--color", "debug", "branch", "list", "alpha"});
  EXPECT_NE(colored.output.find("<<branch::alpha>>"), std::string::npos);
  EXPECT_NE(colored.output.find("<<commit_id::"), std::string::npos);

  const Result named = invoke({"branch", "list", "alpha"});
  EXPECT_NE(named.output.find("alpha:"), std::string::npos);
  EXPECT_EQ(named.output.find("beta:"), std::string::npos);
  EXPECT_NE(invoke({"branch", "list", "glob:a*"})
                .output.find("alpha:"),
            std::string::npos);
  EXPECT_NE(invoke({"branch", "list", "exact:beta"})
                .output.find("beta:"),
            std::string::npos);
  EXPECT_NE(invoke({"branch", "list", "substring:lph"})
                .output.find("alpha:"),
            std::string::npos);
  EXPECT_NE(invoke({"branch", "list", "regex:^be"})
                .output.find("beta:"),
            std::string::npos);
  EXPECT_EQ(invoke({"branch", "list", "regex:["}).code, 2);
  EXPECT_EQ(invoke({"branch", "list", "unknown:alpha"}).code, 2);
  const Result revised = invoke({"branch", "list", "-r", "alpha"});
  EXPECT_NE(revised.output.find("alpha:"), std::string::npos);
  EXPECT_EQ(revised.output.find("beta:"), std::string::npos);
  const Result unioned =
      invoke({"branch", "list", "beta", "-r", "alpha"});
  EXPECT_NE(unioned.output.find("alpha:"), std::string::npos);
  EXPECT_NE(unioned.output.find("beta:"), std::string::npos);
  const Result revision_set =
      invoke({"branch", "list", "-r", "alpha | beta"});
  EXPECT_NE(revision_set.output.find("alpha:"), std::string::npos);
  EXPECT_NE(revision_set.output.find("beta:"), std::string::npos);
  const Result all = invoke({"branch", "list", "--all-remotes"});
  EXPECT_NE(all.output.find("alpha@origin:"), std::string::npos);
  EXPECT_NE(all.output.find("beta@backup:"), std::string::npos);
  EXPECT_EQ(all.output.find("HEAD@origin"), std::string::npos);
  const Result origin =
      invoke({"branch", "list", "--remote", "origin"});
  EXPECT_NE(origin.output.find("alpha@origin:"), std::string::npos);
  EXPECT_EQ(origin.output.find("beta@backup:"), std::string::npos);
  EXPECT_EQ(origin.output.find("alpha:"), std::string::npos);
  EXPECT_NE(invoke({"branch", "list", "--remote", "glob:ori*"})
                .output.find("alpha@origin:"),
            std::string::npos);
  const Result remotes = invoke(
      {"branch", "list", "--remote", "origin", "--remote", "backup"});
  EXPECT_NE(remotes.output.find("alpha@origin:"), std::string::npos);
  EXPECT_NE(remotes.output.find("beta@backup:"), std::string::npos);
  EXPECT_TRUE(invoke({"branch", "list", "--remote", "missing"})
                  .output.empty());

  const Result descending =
      invoke({"branch", "list", "--sort", "name-"});
  EXPECT_LT(descending.output.find("beta:"), descending.output.find("alpha:"));
  const std::string all_sort_keys =
      "name,name-,author-name,author-name-,author-email,author-email-,"
      "author-date,author-date-,committer-name,committer-name-,"
      "committer-email,committer-email-,committer-date,committer-date-";
  EXPECT_EQ(invoke({"branch", "list", "--sort", all_sort_keys}).code, 0);
  EXPECT_EQ(invoke({"branch", "list", "--sort", "unknown"}).code, 2);

  ASSERT_EQ(invoke({"branch", "track", "alpha@origin"}).code, 0);
  ASSERT_EQ(invoke({"branch", "track", "glob:b*", "--remote",
                    "glob:back*"})
                .code,
            0);
  ASSERT_EQ(invoke({"branch", "track", "glob:remote-*"}).code, 0);
  EXPECT_TRUE(has_ref("refs/heads/remote-only"));
  const Result tracked = invoke({"branch", "list", "--tracked"});
  ASSERT_EQ(tracked.code, 0) << tracked.error;
  EXPECT_NE(tracked.output.find("alpha@origin:"), std::string::npos);
  EXPECT_NE(tracked.output.find("beta@backup:"), std::string::npos);
  EXPECT_EQ(tracked.output.find("alpha:"), std::string::npos);
  const Result tracked_origin =
      invoke({"branch", "list", "--tracked", "--remote", "origin"});
  EXPECT_NE(tracked_origin.output.find("alpha@origin:"), std::string::npos);
  EXPECT_EQ(tracked_origin.output.find("beta@backup:"), std::string::npos);
  const Result conflicted = invoke({"branch", "list", "--conflicted"});
  EXPECT_EQ(conflicted.code, 0);
  EXPECT_TRUE(conflicted.output.empty());
  EXPECT_EQ(invoke({"branch", "list", "--all-remotes", "--remote",
                    "origin"})
                .code,
            2);
  EXPECT_EQ(invoke({"branch", "list", "--all-remotes", "--tracked"})
                .code,
            2);
  EXPECT_EQ(invoke({"branch", "list", "--all-remotes", "--conflicted"})
                .code,
            2);

  EXPECT_TRUE(has_ref("refs/gg/tracking/branches/origin/alpha"));
  EXPECT_TRUE(has_ref("refs/gg/tracking/branches/backup/beta"));
  const Result explicitly_tracked = invoke({"branch", "list", "--tracked"});
  EXPECT_NE(explicitly_tracked.output.find("alpha@origin:"),
            std::string::npos);
  EXPECT_NE(explicitly_tracked.output.find("beta@backup:"),
            std::string::npos);
  ASSERT_EQ(invoke({"branch", "untrack", "alpha@origin"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/tracking/branches/origin/alpha"));
  EXPECT_EQ(invoke({"branch", "list", "--tracked", "--remote", "origin"})
                .output.find("alpha@origin:"),
            std::string::npos);
  EXPECT_EQ(invoke({"branch", "untrack", "alpha@origin"}).code, 2);
  EXPECT_EQ(invoke({"branch", "track", "@origin"}).code, 2);
  EXPECT_EQ(invoke({"branch", "track", "alpha@"}).code, 2);
  EXPECT_EQ(invoke({"branch", "track", "alpha@origin", "glob:b*"}).code,
            2);
  EXPECT_EQ(invoke({"branch", "track", "alpha@origin", "--remote",
                    "origin"})
                .code,
            2);
}

TEST_F(RepositoryTest, FailedAttachedBranchMutationPreservesExistingAliases) {
  const git_oid original = ref("HEAD");
  const git_oid alias_source = raw_commit("previous identity");
  const std::string alias = std::string(detail::kAliasPrefix) + detail::oid_string(alias_source);
  set_ref(alias, original);
  ASSERT_EQ(invoke({"status"}).code, 0);
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  write(".git/HEAD.lock", "held by another process\n");
  for (const auto& command : {std::vector<std::string>{"branch", "rename", "main", "renamed"},
                              std::vector<std::string>{"branch", "delete", "main"}}) {
    SCOPED_TRACE(command[1]);
    const Result failed = invoke(command);
    EXPECT_NE(failed.code, 0);
    EXPECT_NE(failed.error.find("HEAD.lock"), std::string::npos);
    EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
    detail::Repository after(path_);
    const auto retained_alias = after.ref_target(alias);
    ASSERT_TRUE(retained_alias.has_value());
    EXPECT_NE(git_oid_equal(&*retained_alias, &original), 0);
    const auto retained_operation = after.operation();
    ASSERT_TRUE(retained_operation.has_value());
    EXPECT_NE(git_oid_equal(&*retained_operation, &operation), 0);
    EXPECT_TRUE(has_ref("refs/heads/main"));
    EXPECT_FALSE(has_ref("refs/heads/renamed"));
  }
}

TEST_F(RepositoryTest, LockedHeadRejectsBranchRenameAndDeletionWithoutChangingState) {
  ASSERT_EQ(invoke({"status"}).code, 0);
  detail::Repository before(path_);
  const git_oid operation = before.ensure_operation();
  const git_oid original = ref("HEAD");
  write(".git/HEAD.lock", "held by another process\n");
  for (const auto& command : {std::vector<std::string>{"branch", "rename", "main", "renamed"},
                              std::vector<std::string>{"branch", "delete", "main"},
                              std::vector<std::string>{"branch", "forget", "main"}}) {
    SCOPED_TRACE(command[1]);
    const Result failed = invoke(command);
    EXPECT_NE(failed.code, 0);
    EXPECT_NE(failed.error.find("HEAD.lock"), std::string::npos);
    EXPECT_EQ(failed.error.find("could not restore"), std::string::npos) << failed.error;
    EXPECT_TRUE(has_ref("refs/heads/main"));
    EXPECT_FALSE(has_ref("refs/heads/renamed"));
    const git_oid head = ref("HEAD");
    EXPECT_NE(git_oid_equal(&head, &original), 0);
    detail::Repository after(path_);
    EXPECT_TRUE(after.head_state().symbolic);
    EXPECT_EQ(after.head_state().value, "refs/heads/main");
    const auto actual_operation = after.operation();
    ASSERT_TRUE(actual_operation.has_value());
    EXPECT_NE(git_oid_equal(&*actual_operation, &operation), 0);
    EXPECT_EQ(read_path(path_ / ".git/HEAD.lock"), "held by another process\n");
  }
}

TEST_F(RepositoryTest, MovingAttachedBranchPreservesCheckoutAndUndoState) {
  const git_oid original = ref("HEAD");
  write("tracked.txt", "target contents\n");
  ASSERT_EQ(invoke_git({"commit", "-am", "target"}).code, 0);
  const git_oid target = ref("HEAD");
  ASSERT_EQ(invoke_git({"branch", "target"}).code, 0);
  ASSERT_EQ(invoke_git({"reset", "--hard", detail::oid_string(original)}).code, 0);
  ASSERT_EQ(invoke({"status"}).code, 0);
  for (const char* action : {"move", "set"}) {
    SCOPED_TRACE(action);
    detail::Repository before(path_);
    const git_oid previous_operation = before.ensure_operation();
    write(".git/HEAD.lock", "held by another process\n");
    const Result rejected = invoke({"branch", action, "main", "--to", "target"});
    EXPECT_NE(rejected.code, 0);
    EXPECT_NE(rejected.error.find("HEAD.lock"), std::string::npos);
    EXPECT_EQ(rejected.error.find("could not restore"), std::string::npos) << rejected.error;
    const git_oid unchanged = ref("refs/heads/main");
    EXPECT_NE(git_oid_equal(&unchanged, &original), 0);
    {
      detail::Repository after(path_);
      const auto operation = after.operation();
      ASSERT_TRUE(operation.has_value());
      EXPECT_NE(git_oid_equal(&*operation, &previous_operation), 0);
      EXPECT_TRUE(after.head_state().symbolic);
      EXPECT_EQ(after.head_state().value, "refs/heads/main");
    }
    std::filesystem::remove(path_ / ".git/HEAD.lock");

    const Result moved = invoke({"branch", action, "main", "--to", "target"});
    ASSERT_EQ(moved.code, 0) << moved.error;
    const git_oid branch = ref("refs/heads/main");
    EXPECT_NE(git_oid_equal(&branch, &target), 0);
    const git_oid head = ref("HEAD");
    EXPECT_NE(git_oid_equal(&head, &original), 0);
    {
      detail::Repository after(path_);
      EXPECT_FALSE(after.head_state().symbolic);
      // @ stays checked out and remains visible as an unnamed head.
      EXPECT_EQ(detail::oid_string(*after.workspace()), detail::oid_string(original));
      EXPECT_TRUE(after.ref_target(detail::user_head_ref(original)).has_value());
    }
    EXPECT_EQ(file(), "base\n");
    EXPECT_EQ(invoke_git({"diff", "--quiet"}).code, 0);
    ASSERT_EQ(invoke({"squash"}).code, 0);
    ASSERT_EQ(invoke({"undo"}).code, 0);
    const git_oid restored = ref("refs/heads/main");
    EXPECT_NE(git_oid_equal(&restored, &original), 0);
    EXPECT_TRUE(detail::Repository(path_).head_state().symbolic);
    EXPECT_EQ(file(), "base\n");
    ASSERT_EQ(invoke({"redo"}).code, 0);
    const git_oid redone = ref("refs/heads/main");
    EXPECT_NE(git_oid_equal(&redone, &target), 0);
    EXPECT_FALSE(detail::Repository(path_).head_state().symbolic);
    EXPECT_EQ(file(), "base\n");
    ASSERT_EQ(invoke({"undo"}).code, 0);
  }
}

TEST_F(RepositoryTest, RenamesAttachedHeadBranchAndRestoresItThroughUndo) {
  const git_oid original = ref("HEAD");
  const auto expect_head = [&](bool symbolic, const std::string& value) {
    detail::Repository repository(path_);
    const detail::HeadState head = repository.head_state();
    EXPECT_EQ(head.symbolic, symbolic);
    EXPECT_EQ(head.value, value);
    const auto target = repository.head_oid();
    ASSERT_TRUE(target.has_value());
    EXPECT_NE(git_oid_equal(&*target, &original), 0);
    EXPECT_EQ(detail::oid_string(*repository.workspace()), detail::oid_string(original));
    EXPECT_EQ(file(), "base\n");
  };

  ASSERT_EQ(invoke({"branch", "rename", "main", "renamed"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/main"));
  EXPECT_TRUE(has_ref("refs/heads/renamed"));
  expect_head(true, "refs/heads/renamed");
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_TRUE(has_ref("refs/heads/main"));
  EXPECT_FALSE(has_ref("refs/heads/renamed"));
  expect_head(true, "refs/heads/main");
  ASSERT_EQ(invoke({"redo"}).code, 0);
  expect_head(true, "refs/heads/renamed");
}

TEST_F(RepositoryTest, DetachesHeadWhenDeletingOrForgettingItsBranch) {
  const git_oid original = ref("HEAD");
  for (const char* action : {"delete", "forget"}) {
    SCOPED_TRACE(action);
    ASSERT_EQ(invoke({"branch", action, "main"}).code, 0);
    EXPECT_FALSE(has_ref("refs/heads/main"));
    {
      detail::Repository repository(path_);
      const detail::HeadState head = repository.head_state();
      EXPECT_FALSE(head.symbolic);
      EXPECT_EQ(head.value, detail::oid_string(original));
      const auto target = repository.head_oid();
      ASSERT_TRUE(target.has_value());
      EXPECT_NE(git_oid_equal(&*target, &original), 0);
      EXPECT_EQ(detail::oid_string(*repository.workspace()), detail::oid_string(original));
      EXPECT_TRUE(repository.ref_target(detail::user_head_ref(original))
                      .has_value());
    }
    EXPECT_EQ(file(), "base\n");
    ASSERT_EQ(invoke({"undo"}).code, 0);
    EXPECT_TRUE(has_ref("refs/heads/main"));
    {
      detail::Repository repository(path_);
      EXPECT_TRUE(repository.head_state().symbolic);
      EXPECT_EQ(repository.head_state().value, "refs/heads/main");
    }
    ASSERT_EQ(invoke({"redo"}).code, 0);
    EXPECT_FALSE(has_ref("refs/heads/main"));
    const git_oid redone_head = ref("HEAD");
    EXPECT_NE(git_oid_equal(&redone_head, &original), 0);
    ASSERT_EQ(invoke({"undo"}).code, 0);
  }
}

TEST_F(RepositoryTest, RenamesAndForgetsBranches) {
  ASSERT_EQ(invoke({"new", main_id()}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "old", "occupied", "plain"}).code,
            0);
  ASSERT_EQ(invoke({"branch", "rename", "plain", "renamed"}).code, 0);
  EXPECT_EQ(invoke({"branch", "rename", "old", "old"}).code, 2);
  EXPECT_EQ(invoke({"branch", "rename", "missing", "new"}).code, 2);
  EXPECT_EQ(invoke({"branch", "rename", "old", "bad name"}).code, 2);
  EXPECT_EQ(invoke({"branch", "rename", "old", "occupied"}).code, 2);
  ASSERT_EQ(invoke({"branch", "rename", "old", "occupied",
                    "--overwrite-existing"})
                .code,
            0);
  EXPECT_FALSE(has_ref("refs/heads/old"));
  EXPECT_TRUE(has_ref("refs/heads/occupied"));

  const git_oid target = ref("refs/heads/occupied");
  set_ref("refs/remotes/origin/occupied", target);
  set_ref("refs/remotes/origin/other", target);
  set_ref("refs/gg/tracking/branches/origin/occupied", target);
  ASSERT_EQ(invoke({"branch", "forget", "glob:occup*"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/occupied"));
  EXPECT_TRUE(has_ref("refs/remotes/origin/occupied"));
  EXPECT_FALSE(has_ref("refs/gg/tracking/branches/origin/occupied"));

  set_ref("refs/heads/occupied", target);
  ASSERT_EQ(invoke({"branch", "forget", "occupied", "--include-remotes"})
                .code,
            0);
  EXPECT_FALSE(has_ref("refs/heads/occupied"));
  EXPECT_FALSE(has_ref("refs/remotes/origin/occupied"));
  EXPECT_TRUE(has_ref("refs/remotes/origin/other"));
  EXPECT_EQ(invoke({"branch", "forget", "missing"}).code, 2);
}

TEST_F(RepositoryTest, MovesBranchesByNameAndSourceRevision) {
  const auto points_to = [&](std::string_view name, const git_oid& target) {
    const git_oid actual = ref(name);
    return git_oid_equal(&actual, &target) != 0;
  };
  ASSERT_EQ(invoke({"new", "-m", "first", main_id()}).code, 0);
  const git_oid first = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"branch", "create", "one", "two"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  ASSERT_EQ(invoke({"branch", "create", "tip"}).code, 0);

  ASSERT_EQ(invoke({"branch", "move", "glob:o*", "--to", "@"}).code, 0);
  EXPECT_TRUE(points_to("refs/heads/one", second));
  EXPECT_TRUE(points_to("refs/heads/two", first));

  ASSERT_EQ(
      invoke({"branch", "move", "--from", "two", "--to", "@"}).code,
      0);
  EXPECT_TRUE(points_to("refs/heads/two", second));
  EXPECT_NE(invoke({"branch", "move", "one", "--to", "@"})
                .output.find("No branches to update."),
            std::string::npos);
  EXPECT_NE(invoke({"branch", "move", "one"})
                .output.find("No branches to update."),
            std::string::npos);
  EXPECT_NE(invoke({"branch", "move", "missing", "--to", "@"})
                .output.find("No branches to update."),
            std::string::npos);

  EXPECT_EQ(invoke({"branch", "move", "one", "--to",
                    git_oid_tostr_s(&first)})
                .code,
            2);
  ASSERT_EQ(invoke({"branch", "move", "one", "--to",
                    git_oid_tostr_s(&first), "--allow-backwards"})
                .code,
            0);
  EXPECT_TRUE(points_to("refs/heads/one", first));

  ASSERT_EQ(invoke({"branch", "move", "--from", "one | two",
                    "--to", "main", "-B"})
                .code,
            0);
  const git_oid main = ref("refs/heads/main");
  EXPECT_TRUE(points_to("refs/heads/one", main));
  EXPECT_TRUE(points_to("refs/heads/two", main));
  EXPECT_TRUE(points_to("refs/heads/tip", main));
}

TEST_F(RepositoryTest, CheckedOutBranchesAdvanceWithNewChangesAndCommits) {
  const git_oid base = ref("refs/heads/main");
  // gg new continues the branch HEAD is attached to.
  ASSERT_EQ(invoke({"new", "-m", "first"}).code, 0);
  const git_oid first = ref("refs/gg/workspaces/default");
  EXPECT_EQ(git_oid_equal(&first, &base), 0);
  EXPECT_EQ(detail::oid_string(ref("refs/heads/main")), detail::oid_string(first));
  write("tracked.txt", "committed\n");
  ASSERT_EQ(invoke({"commit", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  EXPECT_EQ(detail::oid_string(ref("refs/heads/main")), detail::oid_string(second));

  // --detach forks an unnamed head and leaves the branch in place.
  const Result detached = invoke({"new", "-d", "-m", "experiment"});
  ASSERT_EQ(detached.code, 0) << detached.error;
  EXPECT_EQ(detached.output.find("on branch"), std::string::npos);
  const git_oid experiment = ref("refs/gg/workspaces/default");
  EXPECT_EQ(detail::oid_string(ref("refs/heads/main")), detail::oid_string(second));
  EXPECT_FALSE(detail::Repository(path_).head_state().symbolic);
  EXPECT_TRUE(has_ref(detail::user_head_ref(experiment)));
  // A new change on the unnamed head moves its marker forward.
  ASSERT_EQ(invoke({"new", "-m", "more"}).code, 0);
  const git_oid more = ref("refs/gg/workspaces/default");
  EXPECT_FALSE(has_ref(detail::user_head_ref(experiment)));
  EXPECT_TRUE(has_ref(detail::user_head_ref(more)));
  EXPECT_NE(invoke({"log"}).output.find("more"), std::string::npos);

  // Naming a branch checks it out and continues it.
  const Result continued = invoke({"new", "-m", "third", "main"});
  ASSERT_EQ(continued.code, 0) << continued.error;
  EXPECT_NE(continued.output.find("(on branch main)"), std::string::npos);
  const git_oid third = ref("refs/gg/workspaces/default");
  EXPECT_EQ(detail::oid_string(ref("refs/heads/main")), detail::oid_string(third));
  EXPECT_TRUE(detail::Repository(path_).head_state().symbolic);
  // The detached line stays visible without a name.
  const Result log = invoke({"log"});
  EXPECT_NE(log.output.find("more"), std::string::npos);
  EXPECT_NE(log.output.find("experiment"), std::string::npos);

  // A commit ID forks detached even when a branch points there.
  ASSERT_EQ(invoke({"new", "-m", "fork", detail::oid_string(third)}).code, 0);
  EXPECT_EQ(detail::oid_string(ref("refs/heads/main")), detail::oid_string(third));
  EXPECT_FALSE(detail::Repository(path_).head_state().symbolic);

  // Creating a branch at a detached @ checks it out.
  ASSERT_EQ(invoke({"branch", "create", "topic"}).code, 0);
  EXPECT_EQ(detail::Repository(path_).head_state().value, "refs/heads/topic");
  ASSERT_EQ(invoke({"new", "-m", "on topic"}).code, 0);
  EXPECT_EQ(detail::oid_string(ref("refs/heads/topic")),
            detail::oid_string(ref("refs/gg/workspaces/default")));
  EXPECT_EQ(invoke({"branch", "advance"}).code, 2);

  // Abandoning the checked-out commit keeps its branch checked out.
  ASSERT_EQ(invoke({"abandon"}).code, 0);
  EXPECT_EQ(detail::Repository(path_).head_state().value, "refs/heads/topic");
  EXPECT_EQ(detail::oid_string(ref("refs/heads/topic")),
            detail::oid_string(ref("refs/gg/workspaces/default")));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(detail::Repository(path_).head_state().value, "refs/heads/topic");
}

TEST_F(RepositoryTest, DefaultPushPublishesTheCheckedOutBranch) {
  const auto points_to = [&](std::string_view reference,
                             const git_oid& target) {
    const git_oid actual = ref(reference);
    return git_oid_equal(&actual, &target) != 0;
  };
  const auto remote_path =
      path_.parent_path() / (path_.filename().string() + "-default-push");
  std::filesystem::remove_all(remote_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
  git_repository_free(bare);
  git_remote* remote = nullptr;
  ASSERT_EQ(git_remote_create(&remote, repository_.get(), "origin",
                              remote_path.string().c_str()),
            0);
  git_remote_free(remote);

  ASSERT_EQ(invoke({"new", "-m", "first", main_id()}).code, 0);
  write("first.txt", "first\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  const Result detached = invoke({"push", "--dry-run"});
  EXPECT_EQ(detached.code, 2);
  EXPECT_NE(detached.error.find("HEAD is detached"), std::string::npos);

  ASSERT_EQ(invoke({"branch", "create", "near"}).code, 0);
  write("second.txt", "second\n");
  ASSERT_EQ(invoke({"commit", "-m", "second"}).code, 0);
  const git_oid second = ref("refs/gg/workspaces/default");
  EXPECT_TRUE(points_to("refs/heads/near", second));

  const Result dry_run = invoke({"push", "--dry-run"});
  ASSERT_EQ(dry_run.code, 0) << dry_run.error;
  EXPECT_NE(dry_run.output.find("Would push refs/heads/near"),
            std::string::npos);
  EXPECT_FALSE(has_ref("refs/remotes/origin/near"));

  const Result pushed = invoke({"push"});
  ASSERT_EQ(pushed.code, 0) << pushed.error;
  EXPECT_TRUE(points_to("refs/remotes/origin/near", second));
  EXPECT_FALSE(has_ref("refs/remotes/origin/main"));

  ASSERT_EQ(invoke({"new"}).code, 0);
  const Result empty = invoke({"push", "--dry-run"});
  EXPECT_EQ(empty.code, 2);
  EXPECT_NE(empty.error.find("empty description"), std::string::npos);

  std::filesystem::remove_all(remote_path);
}

TEST_F(RepositoryTest, PushesFetchesAndClonesBranches) {
  const auto remote_path = path_.parent_path() / (path_.filename().string() + "-bare");
  const auto clone_path = path_.parent_path() / (path_.filename().string() + "-clone");
  std::filesystem::remove_all(remote_path);
  std::filesystem::remove_all(clone_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
  git_repository_free(bare);
  git_remote* remote = nullptr;
  ASSERT_EQ(git_remote_create(&remote, repository_.get(), "origin",
                              remote_path.string().c_str()),
            0);
  git_remote_free(remote);

  ASSERT_EQ(invoke({"new", "-m", "publish", main_id()}).code, 0);
  write("published.txt", "published\n");
  ASSERT_EQ(invoke({"squash"}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "topic"}).code, 0);
  ASSERT_EQ(invoke({"push", "--branch", "topic"}).code, 0);
  EXPECT_TRUE(has_ref("refs/remotes/origin/topic"));
  EXPECT_TRUE(has_ref("refs/gg/tracking/branches/origin/topic"));
  const git_oid topic_target = ref("refs/heads/topic");
  set_ref("refs/remotes/origin/HEAD", topic_target);
  set_ref("refs/remotes/origin/ghost", topic_target);
  set_ref("refs/gg/tracking/branches/origin/ghost", topic_target);
  const Result tracked = invoke({"push", "--tracked", "--dry-run"});
  EXPECT_NE(tracked.output.find("refs/heads/topic"), std::string::npos);
  EXPECT_EQ(tracked.output.find("refs/heads/ghost"), std::string::npos);
  EXPECT_NE(invoke({"push", "--deleted", "--dry-run"})
                .output.find("refs/heads/ghost"),
            std::string::npos);
  const Result revised = invoke({"push", "-r", "@", "--dry-run"});
  EXPECT_NE(revised.output.find("refs/heads/topic"), std::string::npos);

  git_repository* bare_check = nullptr;
  ASSERT_EQ(git_repository_open(&bare_check, remote_path.string().c_str()), 0);
  git_oid remote_topic{};
  EXPECT_EQ(git_reference_name_to_id(&remote_topic, bare_check,
                                     "refs/heads/topic"),
            0);
  git_reference_iterator* iterator = nullptr;
  ASSERT_EQ(git_reference_iterator_new(&iterator, bare_check), 0);
  const char* remote_ref = nullptr;
  std::vector<std::string> remote_refs;
  while (git_reference_next_name(&remote_ref, iterator) == 0) {
    remote_refs.emplace_back(remote_ref);
  }
  git_reference_iterator_free(iterator);
  EXPECT_EQ(remote_refs, std::vector<std::string>{"refs/heads/topic"});
  git_repository_free(bare_check);

  EXPECT_EQ(invoke({"push", "--named", "named=@"}).code, 2);
  EXPECT_FALSE(has_ref("refs/heads/named"));
  EXPECT_EQ(invoke({"push", "--change", "main | @"}).code, 2);
  const git_oid raw_push = raw_commit("raw push", {ref("refs/heads/main")});
  EXPECT_EQ(invoke({"push", "--revision", git_oid_tostr_s(&raw_push),
                    "--dry-run"})
                .code,
            2);
  git_reference_iterator* generated_iterator = nullptr;
  ASSERT_EQ(git_reference_iterator_glob_new(&generated_iterator,
                                            repository_.get(),
                                            "refs/heads/push-*"),
            0);
  git_reference* generated_reference = nullptr;
  EXPECT_EQ(git_reference_next(&generated_reference, generated_iterator),
            GIT_ITEROVER);
  git_reference_iterator_free(generated_iterator);

  const Result fetched_default = invoke({"fetch"});
  EXPECT_EQ(fetched_default.code, 0) << fetched_default.error;
  const Result fetched_origin = invoke({"fetch", "--remote", "origin"});
  EXPECT_EQ(fetched_origin.code, 0) << fetched_origin.error;
  EXPECT_EQ(invoke({"push", "--branch", "topic", "--remote", "origin"})
                .code,
            0);

  ASSERT_EQ(invoke({"branch", "create", "second", "dry"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "release"}).code, 0);
  const Result multi_push =
      invoke({"push", "-b", "glob:to*", "-b", "regex:^second$", "-t",
              "glob:rel*"});
  ASSERT_EQ(multi_push.code, 0) << multi_push.error;
  EXPECT_TRUE(has_ref("refs/gg/remotes/origin/tags/release"));
  const Result tracked_tag = invoke({"push", "--tracked", "--dry-run"});
  EXPECT_NE(tracked_tag.output.find("refs/tags/release"), std::string::npos);
  EXPECT_EQ(invoke({"push", "-t", "release"}).code, 0);
  EXPECT_EQ(invoke({"push", "-b", "topic", "--option", "ci=1"}).code, 2);
  ASSERT_EQ(invoke({"branch", "create", "gone"}).code, 0);
  ASSERT_EQ(invoke({"push", "-b", "gone"}).code, 0);
  ASSERT_EQ(invoke({"branch", "delete", "gone"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "gone-tag"}).code, 0);
  ASSERT_EQ(invoke({"push", "-t", "gone-tag"}).code, 0);
  ASSERT_TRUE(has_ref("refs/gg/remotes/origin/tags/gone-tag"));
  ASSERT_EQ(invoke({"tag", "delete", "gone-tag"}).code, 0);
  ASSERT_EQ(invoke({"push", "--deleted"}).code, 0);
  EXPECT_FALSE(has_ref("refs/remotes/origin/gone"));
  EXPECT_FALSE(has_ref("refs/gg/remotes/origin/tags/gone-tag"));
  EXPECT_FALSE(has_ref("refs/gg/tracking/branches/origin/gone"));
  EXPECT_FALSE(has_ref("refs/gg/tracking/tags/origin/gone-tag"));
  EXPECT_NE(invoke({"push", "--deleted"}).output.find("No refs to push."),
            std::string::npos);
  const Result dry_run =
      invoke({"push", "-b", "dry", "--dry-run", "--option", "ignored=1"});
  ASSERT_EQ(dry_run.code, 0) << dry_run.error;
  EXPECT_NE(dry_run.output.find("Would push refs/heads/dry"),
            std::string::npos);
  EXPECT_EQ(invoke({"push", "-b", "glob:missing*", "--dry-run"}).code, 2);
  ASSERT_EQ(git_repository_open(&bare_check, remote_path.string().c_str()), 0);
  git_oid pushed{};
  EXPECT_EQ(git_reference_name_to_id(&pushed, bare_check,
                                     "refs/heads/second"),
            0);
  EXPECT_EQ(git_reference_name_to_id(&pushed, bare_check,
                                     "refs/tags/release"),
            0);
  EXPECT_NE(git_reference_name_to_id(&pushed, bare_check,
                                     "refs/tags/gone-tag"),
            0);
  EXPECT_NE(git_reference_name_to_id(&pushed, bare_check, "refs/heads/dry"),
            0);
  git_repository_free(bare_check);

  ASSERT_EQ(invoke({"new", "-m", ""}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "empty"}).code, 0);
  EXPECT_EQ(invoke({"push", "-b", "empty"}).code, 2);
  ASSERT_EQ(invoke({"push", "--all", "--allow-empty-description"}).code, 0);
  ASSERT_EQ(git_repository_open(&bare_check, remote_path.string().c_str()), 0);
  EXPECT_EQ(git_reference_name_to_id(&pushed, bare_check, "refs/heads/dry"),
            0);
  EXPECT_EQ(git_reference_name_to_id(&pushed, bare_check, "refs/heads/empty"),
            0);
  git_reference* ignored_tag = nullptr;
  ASSERT_EQ(git_reference_create(&ignored_tag, bare_check, "refs/tags/ignored",
                                 &topic_target, 0, "test tag"),
            0);
  git_reference_free(ignored_tag);
  ASSERT_EQ(git_repository_set_head(bare_check, "refs/heads/topic"), 0);
  git_repository_free(bare_check);

  const auto shallow_path = clone_path.string() + "-shallow";
  std::filesystem::remove_all(shallow_path);
  const Result shallow = run(
      {"clone", "--depth", "1", remote_path.string(), shallow_path});
  EXPECT_EQ(shallow.code, 1);
  EXPECT_NE(shallow.error.find("shallow fetch is not supported"),
            std::string::npos);
  const Result cloned =
      run({"clone", "--remote", "upstream", "--branch", "glob:top*", "-t",
           "regex:^rel", "--object-hash", "sha1", remote_path.string(),
           clone_path.string()});
  EXPECT_EQ(cloned.code, 0) << cloned.error;
  EXPECT_TRUE(std::filesystem::exists(clone_path / ".git"));
  {
    detail::Repository cloned_repo(clone_path);
    const std::optional<git_oid> workspace =
        cloned_repo.ref_target(detail::kWorkspaceRef);
    ASSERT_TRUE(workspace.has_value());
    EXPECT_TRUE(cloned_repo.commit_aliases(*workspace).empty());
    EXPECT_TRUE(cloned_repo.ref_target("refs/heads/topic").has_value());
    EXPECT_TRUE(cloned_repo.ref_target("refs/tags/release").has_value());
    EXPECT_TRUE(cloned_repo
                    .ref_target("refs/gg/remotes/upstream/tags/release")
                    .has_value());
    EXPECT_TRUE(cloned_repo
                    .ref_target("refs/gg/tracking/branches/upstream/topic")
                    .has_value());
    EXPECT_TRUE(cloned_repo
                    .ref_target("refs/gg/tracking/tags/upstream/release")
                    .has_value());
    EXPECT_FALSE(cloned_repo.ref_target("refs/tags/ignored").has_value());
    EXPECT_FALSE(cloned_repo
                     .ref_target("refs/gg/remotes/upstream/tags/ignored")
                     .has_value());
    EXPECT_FALSE(cloned_repo.ref_target("refs/remotes/upstream/named").has_value());
    std::optional<git_oid> cursor =
        cloned_repo.ref_target("refs/remotes/upstream/topic");
    std::size_t revisions = 0;
    while (cursor.has_value()) {
      EXPECT_TRUE(cloned_repo.commit_aliases(*cursor).empty());
      const std::vector<git_oid> parents = cloned_repo.parents(*cursor);
      cursor = parents.empty() ? std::nullopt
                               : std::optional<git_oid>{parents.front()};
      ++revisions;
    }
    EXPECT_GE(revisions, 2U);
    const auto aliases = cloned_repo.aliases();
    EXPECT_TRUE(std::all_of(aliases.begin(), aliases.end(), [](const auto& alias) {
      return alias.first == detail::oid_string(alias.second);
    }));
  }

  const auto tag_clone_path = clone_path.string() + "-tag";
  std::filesystem::remove_all(tag_clone_path);
  const Result tag_clone = run(
      {"clone", "--tag", "release", remote_path.string(), tag_clone_path});
  ASSERT_EQ(tag_clone.code, 0) << tag_clone.error;
  {
    detail::Repository tag_repo(tag_clone_path);
    const auto tag_workspace = tag_repo.ref_target(detail::kWorkspaceRef);
    ASSERT_TRUE(tag_workspace.has_value());
    EXPECT_TRUE(tag_repo.parents(*tag_workspace).empty());
    EXPECT_TRUE(tag_repo.ref_target("refs/tags/release").has_value());
    EXPECT_TRUE(tag_repo
                    .ref_target("refs/gg/remotes/origin/tags/release")
                    .has_value());
    EXPECT_TRUE(tag_repo
                    .ref_target("refs/gg/tracking/tags/origin/release")
                    .has_value());
    EXPECT_FALSE(tag_repo.ref_target("refs/tags/ignored").has_value());
    EXPECT_FALSE(tag_repo.ref_target("refs/heads/topic").has_value());
    EXPECT_FALSE(tag_repo.ref_target("refs/remotes/origin/topic").has_value());
  }

  const auto missing_branch_path = clone_path.string() + "-missing-branch";
  std::filesystem::remove_all(missing_branch_path);
  EXPECT_EQ(run({"clone", "-b", "topic", "-b", "glob:missing*",
                 remote_path.string(), missing_branch_path})
                .code,
            2);
  const auto missing_tag_path = clone_path.string() + "-missing-tag";
  std::filesystem::remove_all(missing_tag_path);
  std::filesystem::create_directories(missing_tag_path);
  EXPECT_EQ(run({"clone", "-b", "topic", "-t", "glob:missing*",
                 remote_path.string(), missing_tag_path})
                .code,
            2);
  EXPECT_TRUE(std::filesystem::is_empty(missing_tag_path));

  expect_workspace_coherent();

  std::filesystem::remove_all(shallow_path);
  std::filesystem::remove_all(tag_clone_path);
  std::filesystem::remove_all(missing_branch_path);
  std::filesystem::remove_all(missing_tag_path);
  std::filesystem::remove_all(clone_path);
  std::filesystem::remove_all(remote_path);
}

TEST_F(RepositoryTest, FetchesSelectedRefsFromMultipleRemotes) {
  const auto origin_path =
      path_.parent_path() / (path_.filename().string() + "-fetch-origin");
  const auto backup_path =
      path_.parent_path() / (path_.filename().string() + "-fetch-backup");
  std::filesystem::remove_all(origin_path);
  std::filesystem::remove_all(backup_path);
  for (const auto& remote_path : {origin_path, backup_path}) {
    git_repository* bare = nullptr;
    ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
    git_repository_free(bare);
  }
  for (const auto& [name, remote_path] :
       std::vector<std::pair<std::string, std::filesystem::path>>{
           {"origin", origin_path}, {"backup", backup_path}}) {
    git_remote* remote = nullptr;
    ASSERT_EQ(git_remote_create(&remote, repository_.get(), name.c_str(),
                                remote_path.string().c_str()),
              0);
    git_remote_free(remote);
  }

  ASSERT_EQ(invoke({"new", "-m", "publish", main_id()}).code, 0);
  ASSERT_EQ(invoke({"branch", "create", "one", "two"}).code, 0);
  ASSERT_EQ(invoke({"tag", "set", "release", "stable"}).code, 0);
  const git_oid target = ref("refs/heads/one");
  ASSERT_EQ(invoke({"new", "-m", "later"}).code, 0);
  ASSERT_EQ(invoke({"branch", "set", "two", "-r", "@"}).code, 0);
  const git_oid later = ref("refs/heads/two");
  for (const std::string_view remote : {"origin", "backup"}) {
    ASSERT_EQ(
        invoke({"push", "-b", "one", "--remote", std::string(remote)}).code,
        0);
    ASSERT_EQ(
        invoke({"push", "-b", "two", "--remote", std::string(remote)}).code,
        0);
  }
  for (const auto& remote_path : {origin_path, backup_path}) {
    git_repository* bare = nullptr;
    ASSERT_EQ(git_repository_open(&bare, remote_path.string().c_str()), 0);
    for (const std::string_view tag : {"release", "stable"}) {
      git_reference* reference = nullptr;
      ASSERT_EQ(git_reference_create(&reference, bare,
                                     ("refs/tags/" + std::string(tag)).c_str(),
                                     &target, 1,
                                     "test"),
                0);
      git_reference_free(reference);
    }
    if (remote_path == origin_path) {
      git_object* target_object = nullptr;
      ASSERT_EQ(git_object_lookup(&target_object, bare, &target,
                                  GIT_OBJECT_COMMIT),
                0);
      git_signature* signature = nullptr;
      ASSERT_EQ(git_signature_now(&signature, "GG Test", "gg@example.test"),
                0);
      git_oid annotated{};
      ASSERT_EQ(git_tag_create(&annotated, bare, "annotated", target_object,
                               signature, "annotated", 1),
                0);
      git_signature_free(signature);
      git_object_free(target_object);
    }
    if (remote_path == backup_path) {
      git_reference* conflict = nullptr;
      ASSERT_EQ(git_reference_create(&conflict, bare, "refs/tags/conflict",
                                     &target, 1, "test"),
                0);
      git_reference_free(conflict);
      git_commit* target_commit = nullptr;
      ASSERT_EQ(git_commit_lookup(&target_commit, bare, &target), 0);
      git_tree* tree = nullptr;
      ASSERT_EQ(git_tree_lookup(&tree, bare,
                                git_commit_tree_id(target_commit)),
                0);
      git_signature* signature = nullptr;
      ASSERT_EQ(git_signature_now(&signature, "Remote", "remote@example.test"),
                0);
      git_oid orphan{};
      ASSERT_EQ(git_commit_create(&orphan, bare, nullptr, signature, signature,
                                  nullptr, "remote-only", tree, 0, nullptr),
                0);
      git_reference* reference = nullptr;
      ASSERT_EQ(git_reference_create(&reference, bare, "refs/tags/orphan",
                                     &orphan, 1, "test"),
                0);
      git_reference_free(reference);
      git_signature_free(signature);
      git_tree_free(tree);
      git_commit_free(target_commit);
    }
    git_repository_free(bare);
  }

  const auto remove_ref = [&](std::string_view name) {
    if (has_ref(name)) {
      const std::string owned_name(name);
      ASSERT_EQ(git_reference_remove(repository_.get(), owned_name.c_str()), 0);
    }
  };
  remove_ref("refs/remotes/origin/one");
  remove_ref("refs/remotes/origin/two");
  remove_ref("refs/remotes/backup/one");
  remove_ref("refs/remotes/backup/two");
  remove_ref("refs/tags/release");
  remove_ref("refs/tags/stable");

  ASSERT_EQ(invoke({"fetch", "--remote", "origin", "-b", "one"}).code, 0);
  EXPECT_TRUE(has_ref("refs/remotes/origin/one"));
  EXPECT_TRUE(has_ref("refs/gg/tracking/branches/origin/one"));
  EXPECT_FALSE(has_ref("refs/remotes/origin/two"));
  EXPECT_FALSE(has_ref("refs/tags/release"));
  remove_ref("refs/remotes/origin/one");

  ASSERT_EQ(invoke({"fetch", "--remote", "glob:orig*", "-b", "glob:t*"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/remotes/origin/two"));
  remove_ref("refs/remotes/origin/two");

  ASSERT_EQ(invoke({"fetch", "--remote", "origin", "--remote", "backup",
                    "--branch", "one"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/remotes/origin/one"));
  EXPECT_TRUE(has_ref("refs/remotes/backup/one"));

  ASSERT_EQ(invoke({"fetch", "--all-remotes", "--branch", "one",
                    "--branch", "two"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/remotes/origin/two"));
  EXPECT_TRUE(has_ref("refs/remotes/backup/two"));

  set_ref("refs/remotes/origin/one", later);
  set_ref("refs/remotes/origin/HEAD", later);
  ASSERT_EQ(invoke({"fetch", "--tracked", "--remote", "origin"}).code, 0);
  const git_oid refreshed = ref("refs/remotes/origin/one");
  EXPECT_NE(git_oid_equal(&refreshed, &target), 0);

  ASSERT_EQ(invoke({"fetch", "--remote", "origin", "--tag", "glob:re*",
                    "--tag", "regex:^stable$"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/tags/release"));
  EXPECT_TRUE(has_ref("refs/tags/stable"));
  EXPECT_TRUE(has_ref("refs/gg/remotes/origin/tags/release"));
  EXPECT_TRUE(has_ref("refs/gg/remotes/origin/tags/stable"));
  EXPECT_TRUE(has_ref("refs/gg/tracking/tags/origin/release"));
  EXPECT_TRUE(has_ref("refs/gg/tracking/tags/origin/stable"));
  EXPECT_NE(invoke({"tag", "list", "--tracked", "--remote", "origin"})
                .output.find("release@origin:"),
            std::string::npos);

  git_repository* backup = nullptr;
  ASSERT_EQ(git_repository_open(&backup, backup_path.string().c_str()), 0);
  ASSERT_EQ(git_reference_remove(backup, "refs/heads/two"), 0);
  git_repository_free(backup);
  set_ref("refs/tags/conflict", later);
  set_ref("refs/gg/remotes/backup/tags/release", target);
  ASSERT_EQ(invoke({"fetch", "--remote", "backup"}).code, 0);
  EXPECT_FALSE(has_ref("refs/remotes/backup/two"));
  EXPECT_TRUE(has_ref("refs/gg/remotes/backup/tags/release"));
  EXPECT_TRUE(has_ref("refs/gg/remotes/backup/tags/stable"));
  EXPECT_FALSE(has_ref("refs/gg/remotes/backup/tags/orphan"));
  EXPECT_FALSE(has_ref("refs/gg/remotes/backup/tags/conflict"));

  remove_ref("refs/remotes/backup/one");
  const Result tracked_tags =
      invoke({"fetch", "--tracked", "--remote", "backup"});
  EXPECT_NE(tracked_tags.output.find("Fetched backup"), std::string::npos);
  EXPECT_TRUE(has_ref("refs/gg/remotes/backup/tags/release"));

  ASSERT_EQ(git_repository_open(&backup, backup_path.string().c_str()), 0);
  ASSERT_EQ(git_reference_remove(backup, "refs/tags/release"), 0);
  ASSERT_EQ(git_reference_remove(backup, "refs/tags/stable"), 0);
  git_repository_free(backup);
  ASSERT_EQ(invoke({"fetch", "--tracked", "--remote", "backup"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/remotes/backup/tags/release"));
  EXPECT_FALSE(has_ref("refs/gg/remotes/backup/tags/stable"));
  ASSERT_EQ(invoke({"branch", "untrack", "one@backup"}).code, 0);
  remove_ref("refs/remotes/backup/one");
  const Result no_tracked = invoke({"fetch", "--tracked", "--remote", "backup"});
  EXPECT_NE(no_tracked.output.find("No tracked refs to fetch"),
            std::string::npos);
  EXPECT_FALSE(has_ref("refs/remotes/backup/one"));

  set_ref("refs/gg/remotes/backup/tags/phantom", target);
  ASSERT_EQ(invoke({"fetch", "--remote", "backup"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/remotes/backup/tags/phantom"));
  EXPECT_EQ(invoke({"fetch", "--remote", "glob:missing*"}).code, 2);
  EXPECT_EQ(invoke({"fetch", "--remote", "origin", "-b", "glob:missing*"})
                .code,
            2);

  std::filesystem::remove_all(origin_path);
  std::filesystem::remove_all(backup_path);
}

TEST_F(RepositoryTest, InfersCloneDestinationFromDotGitUrl) {
  const auto remote_path =
      path_.parent_path() / (path_.filename().string() + "-inferred.git");
  const auto clone_path = path_.parent_path() /
                          (path_.filename().string() + "-inferred");
  std::filesystem::remove_all(remote_path);
  std::filesystem::remove_all(clone_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
  git_repository_free(bare);

  const auto original_directory = std::filesystem::current_path();
  std::filesystem::current_path(path_.parent_path());
  const Result cloned = run({"clone", remote_path.string()});
  std::filesystem::current_path(original_directory);

  EXPECT_EQ(cloned.code, 0) << cloned.error;
  EXPECT_TRUE(std::filesystem::exists(clone_path));
  std::filesystem::remove_all(clone_path);
  std::filesystem::remove_all(remote_path);
}

TEST_F(RepositoryTest, InfersCloneDestinationWithoutDotGitSuffix) {
  const auto remote_path = path_ / "plain-remote";
  const auto clone_path = path_.parent_path() / remote_path.filename();
  std::filesystem::remove_all(remote_path);
  std::filesystem::remove_all(clone_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
  git_repository_free(bare);

  const auto original_directory = std::filesystem::current_path();
  std::filesystem::current_path(path_.parent_path());
  const Result cloned = run({"clone", remote_path.string()});
  std::filesystem::current_path(original_directory);

  EXPECT_EQ(cloned.code, 0) << cloned.error;
  EXPECT_TRUE(std::filesystem::exists(clone_path));
  std::filesystem::remove_all(clone_path);
  std::filesystem::remove_all(remote_path);
}

TEST_F(RepositoryTest, InitializesNewAndExistingRepositories) {
  const git_oid base = ref("HEAD");
  Result initialized = run({"init", path_.string()});
  ASSERT_EQ(initialized.code, 0) << initialized.error;
  const git_oid adopted = ref("refs/gg/workspaces/default");
  const git_oid adopted_parent = commit_parent(adopted);
  EXPECT_NE(git_oid_equal(&adopted_parent, &base), 0);

  const auto target =
      path_.parent_path() / (path_.filename().string() + "-initialized");
  std::filesystem::remove_all(target);

  initialized = run({"init", "--object-hash", "sha1", target.string()});
  ASSERT_EQ(initialized.code, 0) << initialized.error;
  EXPECT_NE(initialized.output.find("Initialized repository"), std::string::npos);
  EXPECT_NE(run({"-R", target.string(), "status"})
                .output.find("Working copy (@)"),
            std::string::npos);

  git_repository* repository = nullptr;
  ASSERT_EQ(git_repository_open(&repository, target.string().c_str()), 0);
  git_oid before{};
  ASSERT_EQ(git_reference_name_to_id(&before, repository,
                                     "refs/gg/workspaces/default"),
            0);
  git_repository_free(repository);
  initialized = run({"init", target.string()});
  ASSERT_EQ(initialized.code, 0) << initialized.error;
  ASSERT_EQ(git_repository_open(&repository, target.string().c_str()), 0);
  git_oid after{};
  ASSERT_EQ(git_reference_name_to_id(&after, repository,
                                     "refs/gg/workspaces/default"),
            0);
  git_repository_free(repository);
  EXPECT_NE(git_oid_equal(&before, &after), 0);

  EXPECT_NE(run({"init", "--object-hash", "sha256", target.string()}).code,
            0);
  std::filesystem::remove_all(target);

  const auto sha256_target = path_.parent_path() /
                             (path_.filename().string() + "-sha256-init");
  std::filesystem::remove_all(sha256_target);
  const Result sha256 =
      run({"init", "--object-hash", "sha256", sha256_target.string()});
  ASSERT_EQ(sha256.code, 0) << sha256.error;
  ASSERT_EQ(git_repository_open(&repository, sha256_target.string().c_str()),
            0);
  EXPECT_EQ(git_repository_oid_type(repository), GIT_OID_SHA256);
  git_repository_free(repository);
  EXPECT_EQ(run({"-R", sha256_target.string(), "status"}).code, 0);
  std::filesystem::remove_all(sha256_target);

  const auto default_target =
      path_.parent_path() / (path_.filename().string() + "-default-init");
  std::filesystem::remove_all(default_target);
  std::filesystem::create_directories(default_target);
  const auto original_directory = std::filesystem::current_path();
  std::filesystem::current_path(default_target);
  const Result default_initialized = run({"init"});
  std::filesystem::current_path(original_directory);
  EXPECT_EQ(default_initialized.code, 0) << default_initialized.error;
  EXPECT_TRUE(std::filesystem::exists(default_target / ".git"));
  std::filesystem::remove_all(default_target);
}

}  // namespace gg::test
