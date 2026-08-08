// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

namespace gg::test {

TEST_F(RepositoryTest, ManagesBookmarksAndRejectsInvalidRequests) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  EXPECT_NE(invoke({"bookmark", "list"}).output.find("topic:"),
            std::string::npos);
  EXPECT_EQ(invoke({"bookmark", "create", "topic"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "create", "bad name"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "set", "topic", "-r", "main"}).code, 0);
  EXPECT_EQ(invoke({"bookmark", "delete", "missing"}).code, 2);
  EXPECT_EQ(invoke({"bookmark", "delete", "topic"}).code, 0);
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

}  // namespace gg::test
