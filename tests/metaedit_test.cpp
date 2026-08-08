// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "repository.hpp"

namespace gg::test {
namespace {

detail::CommitPtr lookup(detail::Repository& repo, std::string_view revision) {
  return repo.commit(repo.resolve(revision));
}

}  // namespace

TEST_F(RepositoryTest, EditsMetadataAndRestacksDescendants) {
  ASSERT_EQ(invoke({"new", "-m", "old", "main"}).code, 0);
  detail::Repository repo(path_);
  const git_oid old = repo.resolve("@");
  const std::optional<std::string> old_id = repo.change_id(old);
  ASSERT_TRUE(old_id.has_value());
  ASSERT_EQ(invoke({"bookmark", "create", "topic"}).code, 0);
  ASSERT_EQ(invoke({"new", "-m", "child"}).code, 0);

  const Result edited =
      invoke({"metaedit", "@-", "-m", "changed", "--author",
              "Alice Example <alice@example.test>", "--author-timestamp",
              "2000-01-23T01:23:45-08:00", "--update-change-id"});
  ASSERT_EQ(edited.code, 0) << edited.error;
  EXPECT_NE(edited.output.find("Modified 1 revision(s)."), std::string::npos);
  EXPECT_NE(edited.output.find("Rebased 1 descendant revision(s)."),
            std::string::npos);

  const git_oid topic = repo.resolve("topic");
  const git_oid workspace = repo.resolve("@");
  const git_oid parent = commit_parent(workspace);
  EXPECT_NE(git_oid_equal(&parent, &topic), 0);
  detail::CommitPtr commit = repo.commit(topic);
  EXPECT_EQ(std::string(git_commit_message(commit.get())), "changed");
  const git_signature* author = git_commit_author(commit.get());
  EXPECT_EQ(std::string(author->name), "Alice Example");
  EXPECT_EQ(std::string(author->email), "alice@example.test");
  EXPECT_EQ(author->when.offset, -8 * 60);
  const std::optional<std::string> new_id = repo.change_id(topic);
  ASSERT_TRUE(new_id.has_value());
  EXPECT_NE(*new_id, *old_id);
  EXPECT_FALSE(has_ref(std::string(detail::kChangePrefix) + *old_id));

  ASSERT_EQ(invoke({"metaedit", "topic", "--update-author",
                    "--update-author-timestamp"})
                .code,
            0);
  commit = lookup(repo, "topic");
  author = git_commit_author(commit.get());
  EXPECT_EQ(std::string(author->name), "GG Test");
  EXPECT_EQ(std::string(author->email), "gg@example.test");
  EXPECT_NE(author->when.time, 0);

  const git_oid before_force = repo.resolve("topic");
  ASSERT_EQ(invoke({"metaedit", "-r", "topic", "--force-rewrite"}).code,
            0);
  const git_oid after_force = repo.resolve("topic");
  EXPECT_EQ(git_oid_equal(&before_force, &after_force), 0);
  EXPECT_NE(invoke({"metaedit", "topic"}).output.find("Nothing changed."),
            std::string::npos);

  ASSERT_EQ(invoke({"metaedit", "topic", "@", "-m", "bulk"}).code, 0);
  EXPECT_EQ(std::string(git_commit_message(lookup(repo, "topic").get())),
            "bulk");
  EXPECT_EQ(std::string(git_commit_message(lookup(repo, "@").get())),
            "bulk");
  ASSERT_EQ(invoke({"metaedit", "topic", "-m", ""}).code, 0);
  EXPECT_EQ(std::string(git_commit_message(lookup(repo, "topic").get())), "");
  EXPECT_NE(invoke({"metaedit", "topic", "-m", ""})
                .output.find("Nothing changed."),
            std::string::npos);

  const git_oid unchanged_workspace = repo.resolve("@");
  const git_oid side = raw_commit("side", {ref("refs/heads/main")});
  set_ref("refs/heads/side", side);
  ASSERT_EQ(invoke({"metaedit", "side", "-m", "updated side"}).code, 0);
  const git_oid after_side = repo.resolve("@");
  EXPECT_NE(git_oid_equal(&unchanged_workspace, &after_side), 0);
}

TEST_F(RepositoryTest, EditsRawGitMetadataWithoutAWorkspace) {
  const Result edited = invoke({"metaedit", "main", "-m", "updated",
                                "--update-change-id"});
  ASSERT_EQ(edited.code, 0) << edited.error;
  detail::Repository repo(path_);
  EXPECT_EQ(std::string(git_commit_message(lookup(repo, "main").get())),
            "updated");
  EXPECT_TRUE(repo.change_id(repo.resolve("main")).has_value());
  EXPECT_EQ(invoke({"workspace", "list"}).output, "No workspaces.\n");
}

TEST_F(RepositoryTest, ValidatesMetadataValues) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  EXPECT_EQ(invoke({"metaedit", "--author", ""}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author", "broken"}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author", " <a@example.test>"}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author", "Name <a@example.test"}).code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author", "Name <>"}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp", "short"}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp", ""}).code, 2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23 01:23:45Z"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45+24:00"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45+0100"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45Zextra"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45Q"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45+01-00"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45+0a:00"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45+01:60"})
                .code,
            2);
  ASSERT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45Z"})
                .code,
            0);
  ASSERT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T03:53:45+02:30"})
                .code,
            0);
  EXPECT_NE(invoke({"metaedit", "--author", "GG Test <gg@example.test>"})
                .output.find("Nothing changed."),
            std::string::npos);
  ASSERT_EQ(invoke({"metaedit", "--author",
                    "GG Test <other@example.test>"})
                .code,
            0);
  ASSERT_EQ(invoke({"metaedit", "--update-author"}).code, 0);
  EXPECT_EQ(invoke({"metaedit", "--author", "A <a@example.test>",
                    "--update-author"})
                .code,
            2);
  EXPECT_EQ(invoke({"metaedit", "--author-timestamp",
                    "2000-01-23T01:23:45Z", "--update-author-timestamp"})
                .code,
            2);
}

}  // namespace gg::test
