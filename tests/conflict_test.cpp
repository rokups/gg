// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"
#include "repository.hpp"
#include "gg/gg.h"

#include <iostream>

namespace gg::test {

TEST_F(RepositoryTest, RecordsAndResolvesConflictsWithoutPausing) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0) << source.error;
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0) << destination.error;
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result rebased =
      invoke({"rebase", "-s", source_id, "-d", destination_id});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  EXPECT_FALSE(has_ref("refs/gg/rewrite"));
  EXPECT_NE(invoke({"log", "-r", source_id}).output.find("conflict"),
            std::string::npos);
  EXPECT_EQ(invoke({"continue"}).code, 2);
  EXPECT_EQ(invoke({"abort"}).code, 2);

  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  git_oid conflicted_revision{};
  ASSERT_EQ(gg_repository_resolve(&conflicted_revision, api,
                                  source_id.c_str()),
            GIT_OK);
  gg_conflict_array conflicts{};
  ASSERT_EQ(gg_repository_conflicts(&conflicts, api, &conflicted_revision),
            GIT_OK);
  ASSERT_EQ(conflicts.count, 1U);
  EXPECT_STREQ(conflicts.items[0].path, "tracked.txt");
  EXPECT_GT(conflicts.items[0].remove_count, 0U);
  EXPECT_GT(conflicts.items[0].add_count, 1U);
  gg_conflict_array_dispose(&conflicts);
  gg_repository_free(api);

  ASSERT_EQ(invoke({"edit", source_id}).code, 0);
  EXPECT_NE(file().find("<<<<<<< Conflict"), std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("Unresolved conflicts"),
            std::string::npos);
  write("tracked.txt", "resolved\n");
  const Result resolved = invoke({"status"});
  ASSERT_EQ(resolved.code, 0) << resolved.error;
  EXPECT_EQ(resolved.output.find("Unresolved conflicts"), std::string::npos);
  EXPECT_EQ(file(), "resolved\n");
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_NE(file().find("<<<<<<< Conflict"), std::string::npos);
  ASSERT_EQ(invoke({"redo"}).code, 0);
  EXPECT_EQ(file(), "resolved\n");
}

TEST_F(RepositoryTest, ComposesConflictsWithoutNestedMarkers) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  write("overlap.txt", "source overlap\n");
  write("clean.sh", "#!/bin/sh\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result first = invoke({"new", "-m", "first", "main"});
  ASSERT_EQ(first.code, 0);
  const std::string first_id = token_after(first.output, "Working copy now at: ");
  write("tracked.txt", "first\n");
  write("overlap.txt", "first overlap\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", first_id}).code, 0);

  const Result left = invoke({"new", "-m", "left", source_id});
  ASSERT_EQ(left.code, 0);
  const std::string left_id = token_after(left.output, "Working copy now at: ");
  write("tracked.txt", "left resolution\n");
  write("overlap.txt", "left overlap resolution\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result right = invoke({"new", "-m", "right", source_id});
  ASSERT_EQ(right.code, 0);
  const std::string right_id =
      token_after(right.output, "Working copy now at: ");
  write("tracked.txt", "right resolution\n");
  write("overlap.txt", "right overlap resolution\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", left_id, "-d", right_id}).code, 0);
  ASSERT_EQ(invoke({"edit", left_id}).code, 0);
  const std::string materialized = file();
  const std::size_t first_marker = materialized.find("<<<<<<< Conflict");
  ASSERT_NE(first_marker, std::string::npos);
  EXPECT_EQ(materialized.find("<<<<<<< Conflict", first_marker + 1),
            std::string::npos);
  EXPECT_NE(materialized.find("left resolution"), std::string::npos);
  EXPECT_NE(materialized.find("right resolution"), std::string::npos);
  EXPECT_NE(materialized.find("Side #3"), std::string::npos) << materialized;
  const std::string second_materialized = read_path(path_ / "overlap.txt");
  const std::size_t second_marker =
      second_materialized.find("<<<<<<< Conflict");
  ASSERT_NE(second_marker, std::string::npos);
  EXPECT_EQ(second_materialized.find("<<<<<<< Conflict", second_marker + 1),
            std::string::npos);
  EXPECT_NE(second_materialized.find("left overlap resolution"),
            std::string::npos);
  EXPECT_NE(second_materialized.find("right overlap resolution"),
            std::string::npos);
  EXPECT_NE(second_materialized.find("Side #3"), std::string::npos)
      << second_materialized;
  ASSERT_EQ(invoke({"file", "chmod", "-r", left_id, "x", "clean.sh"}).code,
            0);
  const Result status = invoke({"status"});
  EXPECT_NE(status.output.find("C tracked.txt"), std::string::npos);
  EXPECT_NE(status.output.find("C overlap.txt"), std::string::npos);
  EXPECT_NE(invoke({"status", "tracked.txt"}).output.find("C tracked.txt"),
            std::string::npos);
  EXPECT_EQ(invoke({"status", "clean.sh"})
                .output.find("Unresolved conflicts:"),
            std::string::npos);
  const Result files = invoke({"file", "list", "-r", left_id});
  EXPECT_NE(files.output.find("tracked.txt"), std::string::npos);
  EXPECT_NE(files.output.find("overlap.txt"), std::string::npos);
  EXPECT_NE(invoke({"diff", "--from", right_id, "--to", left_id})
                .output.find("tracked.txt"),
            std::string::npos);
  EXPECT_NE(invoke({"diff", "--from", left_id, "--to", right_id})
                .output.find("tracked.txt"),
            std::string::npos);
  ASSERT_EQ(invoke({"commit", "-m", "partial", "tracked.txt"}).code, 0);
  EXPECT_NE(invoke({"status"}).output.find("C overlap.txt"),
            std::string::npos);
}

TEST_F(RepositoryTest, RejectsPushingConflictedAncestry) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0);
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "conflicted", "-r", source_id}).code,
            0);
  ASSERT_EQ(invoke({"tag", "set", "conflicted-tag", "-r", source_id}).code,
            0);
  EXPECT_NE(invoke({"bookmark", "list", "--conflicted"})
                .output.find("conflicted"),
            std::string::npos);
  EXPECT_NE(invoke({"tag", "list", "--conflicted"})
                .output.find("conflicted-tag"),
            std::string::npos);
  const Result push = invoke({"push", "-b", "conflicted", "--dry-run"});
  EXPECT_EQ(push.code, 2);
  EXPECT_NE(push.error.find("refusing to push conflicted history"),
            std::string::npos);
  EXPECT_EQ(invoke({"push", "-b", "conflicted", "--allow-conflicts"}).code,
            2);
}

TEST_F(RepositoryTest, PropagatesResolutionsThroughOverlappingDescendants) {
  write("tracked.txt", "one\nmiddle\ntwo\n");
  ASSERT_EQ(invoke_git({"add", "tracked.txt"}).code, 0);
  ASSERT_EQ(invoke_git({"commit", "-m", "two lines"}).code, 0);
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "SOURCE\nmiddle\ntwo\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result child = invoke({"new", "-m", "child"});
  ASSERT_EQ(child.code, 0);
  const std::string child_id = token_after(child.output, "Working copy now at: ");
  write("tracked.txt", "SOURCE\nmiddle\nCHILD\n");
  ASSERT_EQ(invoke({"status"}).code, 0);

  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0);
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "DEST\nmiddle\ntwo\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 0);

  ASSERT_EQ(invoke({"edit", source_id}).code, 0);
  write("tracked.txt", "RESOLVED\nmiddle\ntwo\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"edit", child_id}).code, 0);
  EXPECT_EQ(file(), "RESOLVED\nmiddle\nCHILD\n");
  EXPECT_EQ(invoke({"status"}).output.find("Unresolved conflicts"),
            std::string::npos);
}

TEST_F(RepositoryTest, RecordsAndResolvesDeleteModifyConflicts) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "modified\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result destination = invoke({"new", "-m", "delete", "main"});
  ASSERT_EQ(destination.code, 0);
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  ASSERT_TRUE(std::filesystem::remove(path_ / "tracked.txt"));
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code, 0);
  ASSERT_EQ(invoke({"edit", source_id}).code, 0);
  EXPECT_NE(file().find("(absent)"), std::string::npos);
  ASSERT_TRUE(std::filesystem::remove(path_ / "tracked.txt"));
  EXPECT_EQ(invoke({"status"}).output.find("Unresolved conflicts"),
            std::string::npos);
}

TEST_F(RepositoryTest, DefersGitlinkConflicts) {
  const git_oid target_base = raw_commit("target base");
  const git_oid target_source = raw_commit("target source");
  const git_oid target_destination = raw_commit("target destination");
  ASSERT_EQ(invoke_git({"update-index", "--add", "--cacheinfo",
                        "160000," + detail::oid_string(target_base) + ",sub"})
                .code,
            0);
  ASSERT_EQ(invoke_git({"commit", "-m", "gitlink base"}).code, 0);
  ASSERT_EQ(invoke_git({"branch", "source"}).code, 0);
  ASSERT_EQ(invoke_git({"branch", "destination"}).code, 0);
  ASSERT_EQ(invoke_git({"switch", "source"}).code, 0);
  ASSERT_EQ(invoke_git({"update-index", "--cacheinfo",
                        "160000," + detail::oid_string(target_source) +
                            ",sub"})
                .code,
            0);
  ASSERT_EQ(invoke_git({"commit", "-m", "gitlink source"}).code, 0);
  ASSERT_EQ(invoke_git({"switch", "destination"}).code, 0);
  ASSERT_EQ(invoke_git({"update-index", "--cacheinfo",
                        "160000," + detail::oid_string(target_destination) +
                            ",sub"})
                .code,
            0);
  ASSERT_EQ(invoke_git({"commit", "-m", "gitlink destination"}).code, 0);
  ASSERT_EQ(invoke({"log"}).code, 0);
  const Result rebased = invoke({"rebase", "-s", "source", "-d",
                                 "destination"});
  ASSERT_EQ(rebased.code, 0) << rebased.error;
  ASSERT_EQ(invoke({"edit", "source"}).code, 0);
  const std::string conflict = read_path(path_ / "sub");
  EXPECT_NE(conflict.find("git-submodule"), std::string::npos);
  EXPECT_NE(conflict.find(detail::oid_string(target_base)), std::string::npos);
  EXPECT_NE(conflict.find(detail::oid_string(target_source)), std::string::npos);
  EXPECT_NE(conflict.find(detail::oid_string(target_destination)),
            std::string::npos);
  EXPECT_NE(invoke({"status"}).output.find("C sub"), std::string::npos);
}

TEST_F(RepositoryTest, KeepsConflictMetadataAcrossWorkspaceOperationRestore) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0);
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid before_rebase = ref(detail::kOperationRef);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code,
            0);
  const std::filesystem::path linked =
      path_.parent_path() / (path_.filename().string() + "-conflicted");
  std::filesystem::remove_all(linked);
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "other",
                    "-r", source_id})
                .code,
            0);
  ASSERT_EQ(invoke({"operation", "restore", detail::oid_string(before_rebase)})
                .code,
            0);
  const Result linked_status = invoke_at(linked, {"status"});
  EXPECT_EQ(linked_status.code, 0) << linked_status.error;
  EXPECT_NE(linked_status.output.find("C tracked.txt"), std::string::npos);
  EXPECT_NE(read_path(linked / "tracked.txt").find("<<<<<<< Conflict"),
            std::string::npos);
  ASSERT_EQ(invoke({"workspace", "forget", "other"}).code, 0);
  ASSERT_EQ(invoke_git({"worktree", "remove", "--force", linked.string()})
                .code,
            0);
}

TEST_F(RepositoryTest, InstallsAChainedGitPrePushConflictGuard) {
  const Result source = invoke({"new", "-m", "source", "main"});
  ASSERT_EQ(source.code, 0);
  const std::string source_id =
      token_after(source.output, "Working copy now at: ");
  write("tracked.txt", "source\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  const Result destination = invoke({"new", "-m", "destination", "main"});
  ASSERT_EQ(destination.code, 0);
  const std::string destination_id =
      token_after(destination.output, "Working copy now at: ");
  write("tracked.txt", "destination\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"rebase", "-s", source_id, "-d", destination_id}).code,
            0);
  ASSERT_EQ(invoke({"bookmark", "create", "conflicted", "-r", source_id})
                .code,
            0);

  const std::filesystem::path hook = path_ / ".git/hooks/pre-push";
  write(".git/hooks/pre-push",
        "#!/bin/sh\ncat >/dev/null\n: > \"$0.ran\"\n");
  std::filesystem::permissions(
      hook, std::filesystem::perms::owner_exec |
                std::filesystem::perms::owner_read |
                std::filesystem::perms::owner_write);
  const Result installed = invoke({"util", "install-git-hooks"});
  ASSERT_EQ(installed.code, 0) << installed.error;
  EXPECT_TRUE(std::filesystem::is_regular_file(hook));
  EXPECT_TRUE(std::filesystem::is_regular_file(hook.string() + ".gg-user"));
  EXPECT_NE(read_path(hook).find("gg managed pre-push hook v1"),
            std::string::npos);
  EXPECT_NE(invoke({"util", "install-git-hooks"}).output.find("already"),
            std::string::npos);

  const std::filesystem::path remote = path_ / ".git/native-remote.git";
  ASSERT_EQ(invoke_git({"init", "--bare", remote.string()}).code, 0);
  ASSERT_EQ(invoke_git({"remote", "add", "origin", remote.string()}).code, 0);
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe").parent_path() / "gg";
  ASSERT_EQ(setenv("GG_EXECUTABLE", executable.c_str(), 1), 0);
  const Result rejected = invoke_git(
      {"push", "origin", "refs/heads/conflicted:refs/heads/conflicted"});
  EXPECT_NE(rejected.code, 0);
  EXPECT_NE(rejected.error.find("refusing to push conflicted history"),
            std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(hook.string() + ".gg-user.ran"));
  const Result clean = invoke_git({"push", "origin", "main:main"});
  EXPECT_EQ(clean.code, 0) << clean.error;

  std::istringstream deletion(
      "(delete) 0000000000000000000000000000000000000000 refs/heads/gone "
      "0000000000000000000000000000000000000000\n");
  std::streambuf* previous = std::cin.rdbuf(deletion.rdbuf());
  const Result allowed_deletion = invoke({"util", "check-push-conflicts"});
  std::cin.rdbuf(previous);
  std::cin.clear();
  EXPECT_EQ(allowed_deletion.code, 0) << allowed_deletion.error;

  std::istringstream malformed("not pre-push input\n");
  previous = std::cin.rdbuf(malformed.rdbuf());
  const Result rejected_input = invoke({"util", "check-push-conflicts"});
  std::cin.rdbuf(previous);
  std::cin.clear();
  EXPECT_EQ(rejected_input.code, 2);
  EXPECT_NE(rejected_input.error.find("invalid Git pre-push input"),
            std::string::npos);

  ASSERT_EQ(invoke_git({"config", "core.hooksPath", ".custom-hooks"}).code,
            0);
  write(".custom-hooks/pre-push", "#!/bin/sh\n");
  write(".custom-hooks/pre-push.gg-user", "#!/bin/sh\n");
  EXPECT_EQ(invoke({"util", "install-git-hooks"}).code, 2);
  ASSERT_TRUE(std::filesystem::remove(path_ / ".custom-hooks/pre-push"));
  ASSERT_TRUE(
      std::filesystem::remove(path_ / ".custom-hooks/pre-push.gg-user"));
  ASSERT_EQ(invoke({"util", "install-git-hooks"}).code, 0);
  EXPECT_TRUE(std::filesystem::is_regular_file(
      path_ / ".custom-hooks/pre-push"));
}

}  // namespace gg::test
