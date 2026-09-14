// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"
#include "repository.hpp"
#include "gg/gg.h"

#include <algorithm>

namespace gg::test {
namespace {

// Keep every extra checkout under one unique temporary directory so failed
// assertions also clean up real-repository fixtures.
class WorkspacePaths {
 public:
  explicit WorkspacePaths(const std::filesystem::path& repository)
      : root(repository.string() + "-workspaces") {
    std::filesystem::create_directory(root);
  }
  ~WorkspacePaths() { std::filesystem::remove_all(root); }
  std::filesystem::path operator/(const char* name) const { return root / name; }
  std::filesystem::path root;
};

}  // namespace

TEST_F(RepositoryTest, MovesWorkspaceWithItsFilesAndIndependentHistory) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  const auto moved = paths / "moved with spaces";
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"describe", "-m", "side change"}).code, 0);
  detail::Repository target(linked);
  const auto working = *target.workspace();
  const auto operation = *target.operation();
  const auto operation_ref = target.operation_ref_name();
  const auto main_operation = ref(detail::kOperationRef);
  std::ofstream(linked / "tracked.txt") << "unsnapped edits\n";
  std::ofstream(linked / "untracked.txt") << "untracked data\n";

  const auto result = invoke({"workspace", "move", "side", moved.string()});
  ASSERT_EQ(result.code, 0) << result.error;
  EXPECT_FALSE(std::filesystem::exists(linked));
  EXPECT_EQ(read_path(moved / "tracked.txt"), "unsnapped edits\n");
  EXPECT_EQ(read_path(moved / "untracked.txt"), "untracked data\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "side"}).output, moved.string() + "\n");
  detail::Repository relocated(moved);
  EXPECT_EQ(relocated.workspace_name(), "side");
  EXPECT_EQ(relocated.operation_ref_name(), operation_ref);
  EXPECT_EQ(detail::oid_string(*relocated.workspace()), detail::oid_string(working));
  EXPECT_EQ(detail::oid_string(*relocated.operation()), detail::oid_string(operation));
  EXPECT_EQ(detail::oid_string(ref(detail::kOperationRef)), detail::oid_string(main_operation));
  EXPECT_EQ(invoke_at(moved, {"workspace", "root"}).output, moved.string() + "\n");
}

TEST_F(RepositoryTest, RefusesUnsafeMovesAndPreservesWorkspaceMetadata) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  const auto destination = paths / "destination";
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  EXPECT_EQ(invoke({"workspace", "move", "default", destination.string()}).code, 2);
  EXPECT_EQ(invoke_at(linked, {"workspace", "move", "side", destination.string()}).code, 2);
  EXPECT_EQ(invoke({"workspace", "move", "missing", destination.string()}).code, 2);
  std::filesystem::create_directory(destination);
  EXPECT_EQ(invoke({"workspace", "move", "side", destination.string()}).code, 2);
  std::filesystem::remove(destination);
  std::ofstream(paths / "file") << "keep\n";
  EXPECT_EQ(invoke({"workspace", "move", "side", ((paths / "file") / "child").string()}).code, 2);
  EXPECT_EQ(invoke({"workspace", "root", "--name", "side"}).output, linked.string() + "\n");
  EXPECT_EQ(read_path(path_ / ".git/gg/workspace-roots/side"), linked.string() + "\n");
  EXPECT_TRUE(std::filesystem::exists(linked / "tracked.txt"));
  EXPECT_TRUE(has_ref("refs/gg/workspaces/side"));
  EXPECT_EQ(invoke({"--at-op", "@", "workspace", "move", "side", destination.string()}).code, 2);
}

TEST_F(RepositoryTest, NativeLocksProtectLiveAndMissingWorkspacesFromPruning) {
  WorkspacePaths paths(path_);
  const auto locked = paths / "locked";
  const auto stale = paths / "stale";
  ASSERT_EQ(invoke({"workspace", "add", locked.string(), "--name", "locked"}).code, 0);
  ASSERT_EQ(invoke({"workspace", "add", stale.string(), "--name", "stale"}).code, 0);
  ASSERT_EQ(invoke_at(locked, {"describe", "-m", "locked history"}).code, 0);
  ASSERT_EQ(invoke_at(stale, {"describe", "-m", "stale history"}).code, 0);
  detail::Repository locked_repo(locked), stale_repo(stale);
  const auto locked_operation = locked_repo.operation_ref_name();
  const auto stale_operation = stale_repo.operation_ref_name();
  ASSERT_EQ(invoke({"workspace", "lock", "locked", "--reason", "external drive"}).code, 0);
  EXPECT_NE(invoke_git({"worktree", "list", "--porcelain"}).output.find("locked external drive"), std::string::npos);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("(locked: external drive)"), std::string::npos);
  EXPECT_EQ(invoke({"workspace", "remove", "locked"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "move", "locked", (paths / "moved").string()}).code, 2);
  EXPECT_EQ(invoke({"workspace", "lock", "locked"}).code, 2);
  EXPECT_EQ(invoke({"workspace", "lock", "default"}).code, 2);
  std::filesystem::remove_all(locked);
  std::filesystem::remove_all(stale);
  ASSERT_EQ(invoke({"workspace", "prune", "--dry-run"}).code, 0);
  EXPECT_TRUE(has_ref("refs/gg/workspaces/stale"));
  EXPECT_TRUE(has_ref(stale_operation));
  ASSERT_EQ(invoke({"workspace", "prune", "--expire", "never"}).code, 0);
  EXPECT_TRUE(has_ref("refs/gg/workspaces/stale"));
  ASSERT_EQ(invoke({"workspace", "prune"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/stale"));
  EXPECT_FALSE(has_ref(stale_operation));
  EXPECT_TRUE(has_ref("refs/gg/workspaces/locked"));
  EXPECT_TRUE(has_ref(locked_operation));
  ASSERT_EQ(invoke({"workspace", "unlock", "locked"}).code, 0);
  ASSERT_EQ(invoke({"workspace", "prune"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/locked"));
  EXPECT_FALSE(has_ref(locked_operation));
  ASSERT_EQ(invoke({"workspace", "prune"}).code, 0);
}

TEST_F(RepositoryTest, RollsBackWorkspaceCreationWhenMetadataCannotBeWritten) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  const auto blocker = path_ / ".git/gg/workspace-roots/blocked";
  std::filesystem::create_directories(blocker.parent_path());
  std::ofstream(blocker) << "preserve\n";
  const auto added = invoke({"workspace", "add", linked.string(), "--name", "blocked/side"});
  EXPECT_NE(added.code, 0);
  EXPECT_FALSE(std::filesystem::exists(linked));
  EXPECT_FALSE(has_ref("refs/gg/workspaces/blocked/side"));
  EXPECT_FALSE(std::filesystem::exists(path_ / ".git/gg/workspace-roots/linked"));
  EXPECT_EQ(read_path(blocker), "preserve\n");
  EXPECT_EQ(invoke_git({"worktree", "list", "--porcelain"}).output.find(linked.string()), std::string::npos);
  EXPECT_NE(invoke({"workspace", "rename", "blocked/renamed"}).code, 0);
  detail::Repository reopened(path_);
  EXPECT_EQ(reopened.workspace_name(), "default");
  EXPECT_TRUE(has_ref("refs/gg/workspaces/default"));
  EXPECT_FALSE(has_ref("refs/gg/workspaces/blocked/renamed"));
}

TEST_F(RepositoryTest, RefLocksRejectWorkspaceRemovalAndPruneBeforeNativeDeletion) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  const auto lock = path_ / ".git/refs/gg/workspaces/side.lock";
  std::ofstream(lock) << "locked\n";
  EXPECT_NE(invoke({"workspace", "remove", "side"}).code, 0);
  EXPECT_TRUE(std::filesystem::exists(linked / "tracked.txt"));
  EXPECT_TRUE(has_ref("refs/gg/workspaces/side"));
  std::filesystem::remove_all(linked);
  EXPECT_NE(invoke({"workspace", "prune"}).code, 0);
  EXPECT_NE(invoke_git({"worktree", "list", "--porcelain"}).output.find(linked.string()), std::string::npos);
  EXPECT_TRUE(has_ref("refs/gg/workspaces/side"));
  std::filesystem::remove(lock);
  EXPECT_EQ(invoke({"workspace", "prune"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/side"));
}

TEST_F(RepositoryTest, RemovingWorkspaceKeepsTheFinalSnapshotRecoverable) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  std::ofstream(linked / "tracked.txt") << "last unsnapped edit\n";
  ASSERT_EQ(invoke({"workspace", "remove", "side"}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/operations/worktrees/linked"));
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(invoke_git({"show", "refs/gg/workspaces/side:tracked.txt"}).output, "last unsnapped edit\n");
  EXPECT_FALSE(std::filesystem::exists(linked));
}

TEST_F(RepositoryTest, RepairsAnExternallyMovedLockedWorkspace) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  const auto moved = paths / "moved";
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"describe", "-m", "history"}).code, 0);
  const auto working = ref("refs/gg/workspaces/side");
  ASSERT_EQ(invoke({"workspace", "lock", "side", "--reason", "moving"}).code, 0);
  std::filesystem::rename(linked, moved);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("(stale)"), std::string::npos);
  EXPECT_EQ(invoke({"workspace", "repair", moved.string(), (paths / "missing").string()}).code, 2);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("(stale)"), std::string::npos);
  const auto repaired = invoke({"workspace", "repair", moved.string()});
  ASSERT_EQ(repaired.code, 0) << repaired.error;
  EXPECT_EQ(invoke({"workspace", "root", "--name", "side"}).output, moved.string() + "\n");
  EXPECT_EQ(read_path(path_ / ".git/gg/workspace-roots/side"), moved.string() + "\n");
  EXPECT_EQ(invoke_at(moved, {"workspace", "root"}).output, moved.string() + "\n");
  EXPECT_EQ(detail::oid_string(ref("refs/gg/workspaces/side")), detail::oid_string(working));
  EXPECT_NE(invoke({"workspace", "list"}).output.find("(locked: moving)"), std::string::npos);
}

TEST_F(RepositoryTest, ManagesNativeUnadoptedWorkspacesWithoutCreatingWorkingRefs) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "native";
  const auto moved = paths / "moved";
  ASSERT_EQ(invoke_git({"worktree", "add", "--detach", linked.string()}).code, 0);
  ASSERT_EQ(invoke({"workspace", "move", "native", moved.string()}).code, 0);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/native"));
  EXPECT_TRUE(std::filesystem::exists(path_ / ".git/gg/workspace-roots/native"));
  ASSERT_EQ(invoke({"workspace", "lock", "native"}).code, 0);
  ASSERT_EQ(invoke({"workspace", "unlock", "native"}).code, 0);
  std::filesystem::remove_all(moved);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  std::unique_ptr<gg_repository, decltype(&gg_repository_free)> owned(repository, gg_repository_free);
  gg_mutation_result result{};
  ASSERT_EQ(gg_repository_workspace_prune(&result, repository, nullptr, 0, nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  EXPECT_FALSE(std::filesystem::exists(path_ / ".git/gg/workspace-roots/native"));
  EXPECT_FALSE(has_ref("refs/gg/workspaces/native"));
}

TEST_F(RepositoryTest, RepairsRememberedRootsWithoutAdoptingNativeWorkspaces) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "native";
  const auto moved = paths / "moved";
  const auto manual = paths / "manual";
  const auto remembered = path_ / ".git/gg/workspace-roots/native";
  ASSERT_EQ(invoke_git({"worktree", "add", "--detach", linked.string()}).code, 0);
  ASSERT_EQ(invoke({"workspace", "move", "native", moved.string()}).code, 0);
  ASSERT_EQ(read_path(remembered), moved.string() + "\n");
  std::filesystem::rename(moved, manual);
  ASSERT_EQ(invoke({"workspace", "repair", manual.string()}).code, 0);
  EXPECT_EQ(read_path(remembered), manual.string() + "\n");
  EXPECT_EQ(invoke({"workspace", "root", "--name", "native"}).output, manual.string() + "\n");
  detail::Repository repository(path_);
  const auto records = repository.workspaces();
  const auto native = std::ranges::find(records, "native", &detail::WorkspaceRecord::name);
  ASSERT_NE(native, records.end());
  EXPECT_FALSE(native->managed);
  EXPECT_FALSE(native->stale);
  EXPECT_FALSE(has_ref("refs/gg/workspaces/native"));
  EXPECT_FALSE(has_ref("refs/gg/operations/worktrees/native"));
  EXPECT_FALSE(std::filesystem::exists(path_ / ".git/worktrees/native/gg/workspace"));
}

TEST_F(RepositoryTest, SupportsWorkspacesAttachedToABareCommonRepository) {
  WorkspacePaths paths(path_);
  const auto bare = paths / "bare.git";
  const auto linked = paths / "linked";
  const auto peer = paths / "peer";
  const auto moved = paths / "moved";
  ASSERT_EQ(invoke_git({"clone", "--bare", path_.string(), bare.string()}).code, 0);
  ASSERT_EQ(invoke_git_at(bare, {"worktree", "add", "--detach", linked.string(), "main"}).code, 0);
  const auto listed = invoke_at(linked, {"workspace", "list"});
  ASSERT_EQ(listed.code, 0) << listed.error;
  EXPECT_EQ(listed.output.find("(primary)"), std::string::npos);
  ASSERT_EQ(invoke_at(linked, {"new", "-m", "adopted"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "add", peer.string(), "--name", "peer"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "rename", "renamed", "--workspace", "peer"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "move", "renamed", moved.string()}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "lock", "renamed"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "unlock", "renamed"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "remove", "renamed"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "repair"}).code, 0);
  ASSERT_EQ(invoke_at(linked, {"workspace", "prune"}).code, 0);
}

TEST_F(RepositoryTest, RepairsLinksAfterMovingThePrimaryCheckout) {
  WorkspacePaths paths(path_);
  const auto primary = paths / "primary";
  const auto moved = paths / "moved-primary";
  const auto linked = paths / "linked";
  ASSERT_EQ(invoke_git({"clone", path_.string(), primary.string()}).code, 0);
  ASSERT_EQ(invoke_git_at(primary, {"worktree", "add", "--detach", linked.string()}).code, 0);
  std::filesystem::rename(primary, moved);
  const auto repaired = invoke_at(moved, {"workspace", "repair"});
  ASSERT_EQ(repaired.code, 0) << repaired.error;
  EXPECT_EQ(invoke_at(linked, {"workspace", "root"}).output, linked.string() + "\n");
  EXPECT_NE(invoke_at(moved, {"workspace", "list"}).output.find("(primary)"), std::string::npos);
}

TEST(WorkspaceCliTest, ReplaysAllLifecycleCommandsAndNamedRenames) {
  const std::vector<std::vector<std::string>> cases{
      {"workspace", "rename", "--workspace", "old", "new"},
      {"workspace", "remove", "side"},
      {"workspace", "move", "side", "../new path"},
      {"workspace", "lock", "--reason", "external drive", "side"},
      {"workspace", "unlock", "side"},
      {"workspace", "prune", "--expire", "yesterday", "--dry-run"},
      {"workspace", "repair", "../one", "../two"}};
  for (const auto& arguments : cases) {
    std::vector<std::string_view> views(arguments.begin(), arguments.end());
    std::ostringstream output, error;
    const auto parsed = detail::parse_cli(views, output, error);
    ASSERT_TRUE(std::holds_alternative<detail::Invocation>(parsed.invocation)) << error.str();
    const auto& invocation = std::get<detail::Invocation>(parsed.invocation);
    EXPECT_EQ(detail::replay_arguments(invocation.command), arguments);
  }
  for (const auto& name : {"move", "lock", "unlock", "prune", "repair"}) {
    const auto help = run({"workspace", name, "--doc"});
    ASSERT_EQ(help.code, 0);
    EXPECT_NE(help.output.find("DETAILS"), std::string::npos);
    EXPECT_NE(run({"util", "markdown-help"}).output.find("gg workspace " + std::string(name)), std::string::npos);
  }
}

TEST_F(RepositoryTest, CApiExposesNativeWorkspaceLifecycleAndOwnedLockInfo) {
  WorkspacePaths paths(path_);
  const auto linked = paths / "linked";
  const auto moved = paths / "moved";
  const auto repaired = paths / "repaired";
  ASSERT_EQ(invoke({"workspace", "add", linked.string(), "--name", "side"}).code, 0);
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  std::unique_ptr<gg_repository, decltype(&gg_repository_free)> owned(repository, gg_repository_free);
  gg_mutation_result result{};
  gg_workspace_lock_info info{};
  ASSERT_EQ(gg_repository_workspace_lock_info(&info, repository, "side"), GIT_OK);
  EXPECT_FALSE(info.locked);
  gg_workspace_lock_info_dispose(&info);
  gg_workspace_lock_info_dispose(&info);
  gg_workspace_lock_info_dispose(nullptr);
  ASSERT_EQ(gg_repository_workspace_lock(&result, repository, "side", "keep this", nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  ASSERT_EQ(gg_repository_workspace_lock_info(&info, repository, "side"), GIT_OK);
  EXPECT_TRUE(info.locked);
  EXPECT_STREQ(info.reason, "keep this");
  gg_workspace_lock_info_dispose(&info);
  ASSERT_EQ(gg_repository_workspace_unlock(&result, repository, "side", nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  ASSERT_EQ(gg_repository_workspace_move(&result, repository, "side", moved.c_str(), nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  std::filesystem::rename(moved, repaired);
  const auto repair_path = repaired.string();
  const char* repair_paths[]{repair_path.c_str()};
  ASSERT_EQ(gg_repository_workspace_repair(&result, repository, {repair_paths, 1}, nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  std::filesystem::remove_all(repaired);
  ASSERT_EQ(gg_repository_workspace_prune(&result, repository, nullptr, 1, nullptr), GIT_OK);
  EXPECT_FALSE(result.changed);
  gg_mutation_result_dispose(&result);
  ASSERT_EQ(gg_repository_workspace_prune(&result, repository, nullptr, 0, nullptr), GIT_OK);
  EXPECT_TRUE(result.changed);
  gg_mutation_result_dispose(&result);
  EXPECT_LT(gg_repository_workspace_lock_info(nullptr, repository, "side"), 0);
  EXPECT_LT(gg_repository_workspace_lock_info(&info, repository, "missing"), 0);
  EXPECT_LT(gg_repository_workspace_move(&result, repository, nullptr, "", nullptr), 0);
  gg_mutation_result_dispose(&result);
}

}  // namespace gg::test
