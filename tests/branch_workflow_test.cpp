// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "gg/gg.h"
#include "repository.hpp"

#include <cstring>

namespace gg::test {
namespace {

std::string id(const git_oid& oid) { return detail::oid_string(oid); }

}  // namespace

TEST_F(RepositoryTest, SquashAmendsSelectedWorkingTreePaths) {
  ASSERT_EQ(invoke({"new", "-m", "work"}).code, 0);
  write("selected.txt", "selected\n");
  write("tracked.txt", "left alone\n");
  const Result amended = invoke({"squash", "selected.txt"});
  ASSERT_EQ(amended.code, 0) << amended.error;
  EXPECT_NE(amended.output.find("Amended as "), std::string::npos);
  EXPECT_EQ(invoke({"file", "show", "-r", "@", "selected.txt"}).output,
            "selected\n");
  EXPECT_EQ(invoke({"file", "show", "-r", "@", "tracked.txt"}).output,
            "base\n");
  EXPECT_EQ(invoke_git({"status", "--porcelain"}).output, " M tracked.txt\n");
  // The branch followed the amended commit and stays checked out.
  EXPECT_EQ(id(ref("refs/heads/main")), id(ref(detail::kWorkspaceRef)));
  EXPECT_EQ(detail::Repository(path_).head_state().value, "refs/heads/main");

  ASSERT_EQ(invoke({"squash", "-m", "renamed"}).code, 0);
  EXPECT_NE(invoke({"show", "--no-patch"}).output.find("Description: renamed"),
            std::string::npos);
  EXPECT_EQ(invoke({"squash"}).output, "Nothing changed.\n");
  EXPECT_EQ(invoke({"squash", "selected.txt", "-r", "@"}).code, 2);
}

TEST_F(RepositoryTest, DiffAndStatusDescribeTheWorkingTreeWithoutStaging) {
  write("tracked.txt", "edited\n");
  write("added.txt", "added\n");
  const Result diff = invoke({"diff"});
  ASSERT_EQ(diff.code, 0) << diff.error;
  EXPECT_NE(diff.output.find("+edited"), std::string::npos);
  EXPECT_NE(diff.output.find("added.txt"), std::string::npos);
  EXPECT_EQ(invoke({"diff", "-r", "@"}).output.find("+edited"),
            std::string::npos);
  // Neither command stages anything for Git.
  EXPECT_EQ(invoke_git({"status", "--porcelain"}).output,
            " M tracked.txt\n?? added.txt\n");
  const Result status = invoke({"status"});
  EXPECT_NE(status.output.find("M tracked.txt"), std::string::npos);
  EXPECT_NE(status.output.find("A added.txt"), std::string::npos);
  EXPECT_EQ(invoke_git({"status", "--porcelain"}).output,
            " M tracked.txt\n?? added.txt\n");
}

TEST_F(RepositoryTest, CheckoutAttachesToBranchesAndProtectsOtherWorkspaces) {
  const auto linked =
      path_.parent_path() / (path_.filename().string() + "-branch-workspace");
  std::filesystem::remove_all(linked);
  ASSERT_EQ(invoke({"branch", "create", "feature", "-r", "main"}).code, 0);
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side",
                    "-r", "feature"})
                .code,
            0);
  EXPECT_EQ(invoke_git_at(linked, {"symbolic-ref", "HEAD"}).output,
            "refs/heads/feature\n");
  const Result refused = invoke({"edit", "feature"});
  EXPECT_EQ(refused.code, 2);
  EXPECT_NE(refused.error.find("checked out in workspace side"),
            std::string::npos);
  EXPECT_EQ(invoke({"new", "feature"}).code, 2);
  const git_oid elsewhere = raw_commit("elsewhere", {ref("HEAD")});
  EXPECT_EQ(invoke({"branch", "set", "feature", "-r", id(elsewhere)}).code, 2);

  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  gg_named_ref_array refs{};
  ASSERT_EQ(gg_repository_named_refs(&refs, api), GIT_OK);
  bool saw_main = false;
  bool saw_feature = false;
  for (std::size_t index = 0; index < refs.count; ++index) {
    const gg_named_ref& item = refs.items[index];
    if (item.kind != GG_NAMED_REF_LOCAL_BRANCH) continue;
    if (std::strcmp(item.name, "main") == 0) {
      saw_main = true;
      EXPECT_TRUE(item.current);
      EXPECT_EQ(item.workspace, nullptr);
    } else if (std::strcmp(item.name, "feature") == 0) {
      saw_feature = true;
      EXPECT_FALSE(item.current);
      ASSERT_NE(item.workspace, nullptr);
      EXPECT_STREQ(item.workspace, "side");
    }
  }
  EXPECT_TRUE(saw_main);
  EXPECT_TRUE(saw_feature);
  gg_named_ref_array_dispose(&refs);
  gg_workspace_array workspaces{};
  ASSERT_EQ(gg_repository_workspaces(&workspaces, api), GIT_OK);
  bool saw_side = false;
  for (std::size_t index = 0; index < workspaces.count; ++index) {
    if (std::strcmp(workspaces.items[index].name, "side") == 0) {
      saw_side = true;
      ASSERT_NE(workspaces.items[index].branch, nullptr);
      EXPECT_STREQ(workspaces.items[index].branch, "feature");
    }
  }
  EXPECT_TRUE(saw_side);
  gg_workspace_array_dispose(&workspaces);
  gg_repository_free(api);

  ASSERT_EQ(invoke({"workspace", "remove", "side"}).code, 0);
  const Result attached = invoke({"edit", "feature"});
  ASSERT_EQ(attached.code, 0) << attached.error;
  EXPECT_NE(attached.output.find("(on branch feature)"), std::string::npos);
  EXPECT_EQ(invoke_git({"symbolic-ref", "HEAD"}).output, "refs/heads/feature\n");
  ASSERT_EQ(invoke({"edit", id(ref("refs/heads/main"))}).code, 0);
  EXPECT_NE(invoke_git({"symbolic-ref", "HEAD"}).code, 0);
}

TEST_F(RepositoryTest, CApiReportsHeadAndCreatesDetachedChanges) {
  gg_repository* api = nullptr;
  ASSERT_EQ(gg_repository_attach(&api, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(api, nullptr), GIT_OK);
  const git_oid base = ref("refs/heads/main");
  gg_head head{};
  head.version = GG_OPTIONS_VERSION;
  ASSERT_EQ(gg_repository_head(&head, api), GIT_OK);
  EXPECT_TRUE(head.attached);
  EXPECT_STREQ(head.branch, "main");
  EXPECT_TRUE(head.has_target);
  EXPECT_NE(git_oid_equal(&head.target, &base), 0);
  gg_head_dispose(&head);
  EXPECT_EQ(gg_repository_head(nullptr, api), GIT_EINVALID);

  gg_new_options options = GG_NEW_OPTIONS_INIT;
  options.message = "continued";
  gg_mutation_result result{};
  ASSERT_EQ(gg_repository_new_change(&result, api, &options, nullptr), GIT_OK);
  const git_oid continued = result.working_copy;
  gg_mutation_result_dispose(&result);
  EXPECT_EQ(id(ref("refs/heads/main")), id(continued));

  options.message = "detached";
  options.detach = 1;
  ASSERT_EQ(gg_repository_new_change(&result, api, &options, nullptr), GIT_OK);
  const git_oid detached = result.working_copy;
  gg_mutation_result_dispose(&result);
  EXPECT_EQ(id(ref("refs/heads/main")), id(continued));
  EXPECT_TRUE(has_ref(detail::user_head_ref(detached)));
  ASSERT_EQ(gg_repository_head(&head, api), GIT_OK);
  EXPECT_FALSE(head.attached);
  EXPECT_EQ(head.branch, nullptr);
  EXPECT_NE(git_oid_equal(&head.target, &detached), 0);
  gg_head_dispose(&head);

  // A branch name as the parent checks that branch out and continues it.
  const char* main_parent[] = {"main"};
  options.message = "on main";
  options.detach = 0;
  options.parents = {main_parent, 1};
  ASSERT_EQ(gg_repository_new_change(&result, api, &options, nullptr), GIT_OK);
  const git_oid on_main = result.working_copy;
  gg_mutation_result_dispose(&result);
  EXPECT_EQ(id(ref("refs/heads/main")), id(on_main));
  ASSERT_EQ(gg_repository_head(&head, api), GIT_OK);
  EXPECT_STREQ(head.branch, "main");
  gg_head_dispose(&head);
  gg_repository_free(api);
}

TEST_F(RepositoryTest, FetchFastForwardsTheCheckedOutBranchOnlyWhenClean) {
  const auto remote_path =
      path_.parent_path() / (path_.filename().string() + "-ff-remote");
  std::filesystem::remove_all(remote_path);
  git_repository* bare = nullptr;
  ASSERT_EQ(git_repository_init(&bare, remote_path.string().c_str(), 1), 0);
  git_repository_free(bare);
  ASSERT_EQ(invoke_git({"remote", "add", "origin", remote_path.string()}).code, 0);
  ASSERT_EQ(invoke({"push"}).code, 0);
  const git_oid base = ref("refs/heads/main");

  const auto publish = [&](const std::string& message, const git_oid& parent) {
    const git_oid next = raw_commit(message, {parent});
    EXPECT_EQ(invoke_git({"push", "-q", "origin", id(next) + ":refs/heads/main"})
                  .code,
              0);
    // Pretend the push happened elsewhere: only fetch may learn about it.
    EXPECT_EQ(invoke_git({"update-ref", "refs/remotes/origin/main", id(parent)})
                  .code,
              0);
    return next;
  };
  const git_oid upstream = publish("upstream", base);
  const Result fetched = invoke({"fetch"});
  ASSERT_EQ(fetched.code, 0) << fetched.error;
  EXPECT_NE(fetched.output.find("Fast-forwarded main"), std::string::npos);
  EXPECT_EQ(id(ref("refs/heads/main")), id(upstream));
  EXPECT_EQ(id(ref(detail::kWorkspaceRef)), id(upstream));
  EXPECT_EQ(invoke_git({"symbolic-ref", "HEAD"}).output, "refs/heads/main\n");
  expect_workspace_coherent();

  const git_oid later = publish("later", upstream);
  write("tracked.txt", "local edit\n");
  const Result dirty = invoke({"fetch"});
  ASSERT_EQ(dirty.code, 0) << dirty.error;
  EXPECT_NE(dirty.output.find("main is behind its remote"), std::string::npos);
  EXPECT_EQ(id(ref("refs/heads/main")), id(upstream));
  EXPECT_EQ(id(ref("refs/remotes/origin/main")), id(later));
  EXPECT_EQ(read_path(path_ / "tracked.txt"), "local edit\n");
  std::filesystem::remove_all(remote_path);
}

TEST_F(RepositoryTest, MigratesWorkspacesFromTheSnapshotModel) {
  ASSERT_EQ(invoke({"status"}).code, 0);
  const git_oid base = ref("refs/heads/main");
  const auto legacy = [&](const git_oid& change) {
    ASSERT_EQ(invoke_git({"checkout", "-q", "--detach", id(base)}).code, 0);
    detail::Repository repo(path_);
    repo.record({{std::string(detail::kWorkspaceRef), change}}, {},
                {false, id(base)}, "legacy snapshot state");
  };

  // An untouched placeholder change is dropped and main is checked out.
  legacy(raw_commit("", {base}));
  ASSERT_EQ(invoke({"status"}).code, 0);
  EXPECT_EQ(id(ref(detail::kWorkspaceRef)), id(base));
  EXPECT_EQ(invoke_git({"symbolic-ref", "HEAD"}).output, "refs/heads/main\n");
  EXPECT_EQ(detail::Repository(path_).operation_description(
                *detail::Repository(path_).operation()),
            "gg migrate working copy");

  // Real work stays @ and remains visible without a branch.
  const git_oid work = raw_commit("work", {base});
  legacy(work);
  ASSERT_EQ(invoke({"log"}).code, 0);
  EXPECT_EQ(id(ref(detail::kWorkspaceRef)), id(work));
  EXPECT_EQ(id(ref("HEAD")), id(work));
  EXPECT_TRUE(has_ref(detail::user_head_ref(work)));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(id(ref("HEAD")), id(base));
}

TEST_F(RepositoryTest, MarksAliasOnlyHeadsAsUserHeadsOnce) {
  const git_oid orphan = raw_commit("alias only", {ref("HEAD")});
  const git_oid kept = raw_commit("reachable", {ref("HEAD")});
  set_ref(std::string(detail::kAliasPrefix) + id(orphan), orphan);
  set_ref(std::string(detail::kAliasPrefix) + id(kept), kept);
  set_ref("refs/heads/kept", kept);
  const Result log = invoke({"log"});
  ASSERT_EQ(log.code, 0) << log.error;
  EXPECT_NE(log.output.find("alias only"), std::string::npos);
  EXPECT_TRUE(has_ref(detail::user_head_ref(orphan)));
  EXPECT_FALSE(has_ref(detail::user_head_ref(kept)));
  EXPECT_EQ(invoke_git({"config", "gg.unnamedheads"}).output, "markers\n");

  // Aliases no longer keep heads visible by themselves.
  const git_oid later = raw_commit("later alias", {ref("HEAD")});
  set_ref(std::string(detail::kAliasPrefix) + id(later), later);
  EXPECT_EQ(invoke({"log"}).output.find("later alias"), std::string::npos);
  EXPECT_FALSE(has_ref(detail::user_head_ref(later)));
}

TEST_F(RepositoryTest, GarbageCollectionPrunesMarkersThatNoLongerMarkHeads) {
  ASSERT_EQ(invoke({"new", "-d", "-m", "first"}).code, 0);
  const git_oid first = ref(detail::kWorkspaceRef);
  set_ref(detail::user_head_ref(first) + "-stale", first);
  ASSERT_EQ(invoke({"new", "-m", "second"}).code, 0);
  const git_oid second = ref(detail::kWorkspaceRef);
  EXPECT_TRUE(has_ref(detail::user_head_ref(first) + "-stale"));
  ASSERT_EQ(invoke({"util", "gc"}).code, 0);
  EXPECT_FALSE(has_ref(detail::user_head_ref(first) + "-stale"));
  EXPECT_TRUE(has_ref(detail::user_head_ref(second)));
}

TEST_F(RepositoryTest, DeletingTheCheckedOutBranchKeepsItsWorkVisible) {
  ASSERT_EQ(invoke({"branch", "create", "topic", "-r", "main"}).code, 0);
  ASSERT_EQ(invoke({"edit", "topic"}).code, 0);
  write("tracked.txt", "topic work\n");
  ASSERT_EQ(invoke({"commit", "-m", "topic work"}).code, 0);
  const git_oid work = ref("refs/heads/topic");
  ASSERT_EQ(invoke({"branch", "delete", "topic"}).code, 0);
  EXPECT_NE(invoke_git({"symbolic-ref", "HEAD"}).code, 0);
  EXPECT_TRUE(has_ref(detail::user_head_ref(work)));
  ASSERT_EQ(invoke({"edit", "main"}).code, 0);
  EXPECT_NE(invoke({"log"}).output.find("topic work"), std::string::npos);
}

TEST_F(RepositoryTest, RestoreDiscardsWorkingTreeEditsRecoverably) {
  write("tracked.txt", "discarded\n");
  const Result restored = invoke({"restore"});
  ASSERT_EQ(restored.code, 0) << restored.error;
  EXPECT_EQ(file(), "base\n");
  const std::string saved = token_after(restored.output, "saved them as ");
  EXPECT_EQ(invoke({"file", "show", "-r", saved.substr(0, 40), "tracked.txt"})
                .output,
            "discarded\n");
  EXPECT_EQ(invoke_git({"status", "--porcelain"}).output, "");
}

}  // namespace gg::test
