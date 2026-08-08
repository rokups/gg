// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, ManagesBookmarksAndRejectsInvalidRequests) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "create", "forward", "-r", "main"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "set", "forward", "-r", "@"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "set", "forward", "-r", "@"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "create", "one", "two", "--to", "main"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/one"));
  EXPECT_TRUE(has_ref("refs/heads/two"));
  EXPECT_NE(invoke({"bookmark", "list"}).output.find("topic:"),
            std::string::npos);
  EXPECT_EQ(invoke({"bookmark", "create", "topic"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "create", "bad name"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "create", "valid", "bad name"}).code, 2);
  EXPECT_FALSE(has_ref("refs/heads/valid"));
  EXPECT_EQ(invoke({"bookmark", "set", "topic", "three", "-r", "main"})
                .code,
            2);
  EXPECT_EQ(invoke({"bookmark", "set", "topic", "three", "-B", "-r", "main"})
                .code,
            0);
  EXPECT_TRUE(has_ref("refs/heads/three"));
  EXPECT_EQ(invoke({"bookmark", "delete", "missing"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "delete", "topic", "one", "two", "three",
                    "forward"})
                .code,
            0);
}

TEST_F(RepositoryTest, RenamesAndForgetsBookmarks) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "old", "occupied", "plain"}).code,
            0);
  ASSERT_EQ(invoke({"bookmark", "rename", "plain", "renamed"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "rename", "old", "old"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "rename", "missing", "new"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "rename", "old", "bad name"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "rename", "old", "occupied"}).code, 2);
  ASSERT_EQ(invoke({"bookmark", "rename", "old", "occupied",
                    "--overwrite-existing"})
                .code,
            0);
  EXPECT_FALSE(has_ref("refs/heads/old"));
  EXPECT_TRUE(has_ref("refs/heads/occupied"));

  const git_oid target = ref("refs/heads/occupied");
  set_ref("refs/remotes/origin/occupied", target);
  set_ref("refs/remotes/origin/other", target);
  ASSERT_EQ(invoke({"bookmark", "forget", "occupied"}).code, 0);
  EXPECT_FALSE(has_ref("refs/heads/occupied"));
  EXPECT_TRUE(has_ref("refs/remotes/origin/occupied"));

  set_ref("refs/heads/occupied", target);
  ASSERT_EQ(invoke({"bookmark", "forget", "occupied", "--include-remotes"})
                .code,
            0);
  EXPECT_FALSE(has_ref("refs/heads/occupied"));
  EXPECT_FALSE(has_ref("refs/remotes/origin/occupied"));
  EXPECT_TRUE(has_ref("refs/remotes/origin/other"));
  EXPECT_EQ(invoke({"bookmark", "forget", "missing"}).code, 2);
}

TEST_F(RepositoryTest, PushesFetchesAndClonesOrdinaryGitBookmarks) {
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

  ASSERT_EQ(invoke({"new", "-m", "publish", "main"}).code, 0);
  write("published.txt", "published\n");
  ASSERT_EQ(invoke({"status"}).code, 0);
  ASSERT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  ASSERT_EQ(invoke({"push", "--bookmark", "topic"}).code, 0);

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
  EXPECT_EQ(invoke({"fetch"}).code, 0);
  EXPECT_EQ(invoke({"fetch", "--remote", "origin"}).code, 0);
  EXPECT_EQ(invoke({"push", "--bookmark", "topic", "--remote", "origin"})
                .code,
            0);

  const Result cloned =
      run({"clone", remote_path.string(), clone_path.string()});
  EXPECT_EQ(cloned.code, 0) << cloned.error;
  EXPECT_TRUE(std::filesystem::exists(clone_path / ".git"));
  std::filesystem::remove_all(clone_path);
  std::filesystem::remove_all(remote_path);
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

TEST_F(RepositoryTest, InitializesNewAndExistingGitRepositories) {
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

  EXPECT_EQ(run({"init", "--object-hash", "sha256", target.string()}).code, 2);
  std::filesystem::remove_all(target);

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
