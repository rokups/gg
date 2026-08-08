// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "commands.hpp"

#include <git2/sys/errors.h>

namespace gg::test {

TEST_F(RepositoryTest, CoversRepositoryStateEdgeCases) {
  detail::Repository repo(path_);
  const git_oid base = ref("HEAD");
  EXPECT_EQ(detail::oid_string(base, 100).size(), GIT_OID_SHA1_HEXSIZE);
  EXPECT_TRUE(detail::first_line(nullptr).empty());
  EXPECT_EQ(detail::first_line("one\ntwo"), "one");
  git_error_clear();
  EXPECT_THROW(detail::check(-1, "synthetic failure"), detail::GitError);

  git_reference* symbolic = nullptr;
  ASSERT_EQ(git_reference_symbolic_create(
                &symbolic, repository_.get(), "refs/heads/unborn-link",
                "refs/heads/does-not-exist", 1, "test"),
            0);
  git_reference_free(symbolic);
  set_ref("refs/remotes/origin/main", base);
  const auto refs = repo.data_refs();
  EXPECT_FALSE(refs.contains("refs/heads/unborn-link"));
  EXPECT_FALSE(repo.rewrite_refs().contains("refs/remotes/origin/main"));

  const std::string id = repo.new_change_id(base);
  set_ref(std::string(detail::kChangePrefix) + id, base);
  EXPECT_EQ(repo.new_change_id(base), id + "z");
  set_ref(std::string(detail::kChangePrefix) + "a1", base);
  const git_oid other = raw_commit("other");
  set_ref(std::string(detail::kChangePrefix) + "a2", other);
  EXPECT_THROW(repo.resolve("a"), detail::UserError);
  set_ref(std::string(detail::kChangePrefix) + "b1", base);
  set_ref(std::string(detail::kChangePrefix) + "b2", base);
  const git_oid matching = repo.resolve("b");
  EXPECT_TRUE(git_oid_equal(&matching, &base) != 0);

  repo.apply_refs({}, {}, "no changes");
  EXPECT_THROW(repo.abort_rewrite(), detail::UserError);
  EXPECT_THROW(repo.prepare_continue(), detail::UserError);
  std::ostringstream status;
  repo.pending_status(status);
  EXPECT_TRUE(status.str().empty());
  repo.finish_rewrite();
}

TEST_F(RepositoryTest, ExercisesRewriteVariants) {
  detail::Repository repo(path_);
  const git_oid base = ref("HEAD");
  const git_oid child = raw_commit("child", {base});

  EXPECT_NO_THROW(repo.rewrite_commit(base, {}));
  EXPECT_NO_THROW(repo.rewrite_commit(child, {}));
  EXPECT_NO_THROW(repo.rewrite_commit(child, {base}));

  git_commit* commit = nullptr;
  ASSERT_EQ(git_commit_lookup(&commit, repository_.get(), &base), 0);
  const git_oid tree_oid = *git_commit_tree_id(commit);
  git_commit_free(commit);
  set_ref(std::string(detail::kChangePrefix) + "tree", tree_oid);
  EXPECT_NO_THROW(repo.descendants({}));

  detail::OperationState state = repo.state();
  state.refs["refs/heads/non-commit"] = tree_oid;
  EXPECT_NO_THROW(repo.create_operation(state, std::nullopt));
}

TEST_F(RepositoryTest, ReportsWorkspaceWithoutAStableChangeId) {
  set_ref(detail::kWorkspaceRef, ref("HEAD"));
  ASSERT_EQ(git_repository_set_head(repository_.get(),
                                    "refs/heads/does-not-exist"),
            0);
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("--------"), std::string::npos);
}

TEST_F(RepositoryTest, ImportsAWorkspaceWhenHeadIsUnborn) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(git_repository_set_head(repository_.get(),
                                    "refs/heads/does-not-exist"),
            0);
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("Root working-copy"), std::string::npos);
}

TEST_F(RepositoryTest, UsesFallbackIdentityWhenGitIdentityIsMissing) {
  git_config* config = nullptr;
  ASSERT_EQ(git_repository_config(&config, repository_.get()), 0);
  ASSERT_EQ(git_config_delete_entry(config, "user.name"), 0);
  ASSERT_EQ(git_config_delete_entry(config, "user.email"), 0);
  git_config_free(config);

  const Result created = invoke({"new", "main"});
  EXPECT_EQ(created.code, 0) << created.error;
}

TEST_F(RepositoryTest, RejectsMalformedOperationSnapshots) {
  detail::Repository repo(path_);
  const git_oid bad_header = raw_commit("bad");
  EXPECT_THROW(repo.parse_operation(bad_header), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit("")), detail::GitError);

  const git_oid bad_previous = raw_commit(
      "gg-operation-v1\nwrong -\nhead S refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(bad_previous), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit("gg-operation-v1\n")),
               detail::GitError);

  const git_oid bad_head = raw_commit(
      "gg-operation-v1\nprevious -\nhead X refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(bad_head), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit(
                   "gg-operation-v1\nprevious -\n")),
               detail::GitError);

  const git_oid bad_ref = raw_commit(
      "gg-operation-v1\nprevious -\nhead S refs/heads/main\nwrong 0000000000000000000000000000000000000000 ref\n");
  EXPECT_THROW(repo.parse_operation(bad_ref), detail::GitError);
  EXPECT_THROW(repo.operation_previous(bad_header), detail::GitError);
  EXPECT_THROW(repo.operation_previous(raw_commit(
                   "gg-operation-v1\nwrong -\n")),
               detail::GitError);
}

TEST_F(RepositoryTest, RejectsMalformedPendingRewrites) {
  detail::Repository repo(path_);
  set_ref(detail::kRewriteRef, raw_commit("bad"));
  EXPECT_THROW(repo.pending(), detail::GitError);

  set_ref(detail::kRewriteRef, raw_commit("gg-rewrite-v1\nunknown value\n"));
  EXPECT_THROW(repo.pending(), detail::GitError);

  const git_oid operation = ref("HEAD");
  git_commit* commit = nullptr;
  ASSERT_EQ(git_commit_lookup(&commit, repository_.get(), &operation), 0);
  const git_oid marker_tree = *git_commit_tree_id(commit);
  git_commit_free(commit);
  detail::PendingRewrite pending{};
  pending.operation = operation;
  pending.ancestor = marker_tree;
  pending.ours = marker_tree;
  pending.theirs = marker_tree;
  pending.marker_tree = marker_tree;
  repo.write_pending(pending);
  EXPECT_THROW(repo.prepare_continue(), detail::UserError);
  std::ostringstream status;
  repo.pending_status(status);
  EXPECT_NE(status.str().find("paused"), std::string::npos);
  repo.finish_rewrite();
  EXPECT_FALSE(repo.pending().has_value());

  pending.arguments = {"status"};
  repo.write_pending(pending);
  EXPECT_THROW(repo.prepare_continue(), detail::UserError);
  repo.finish_rewrite();
}

TEST_F(RepositoryTest, SelectsSupportedCredentialKinds) {
  git_credential* credential = nullptr;
  EXPECT_EQ(detail::credentials(&credential, "https://example.test", nullptr, 0,
                                nullptr),
            GIT_PASSTHROUGH);
  EXPECT_NE(detail::credentials(&credential, "https://example.test", nullptr,
                                GIT_CREDENTIAL_DEFAULT, nullptr),
            GIT_PASSTHROUGH);
  git_credential_free(credential);
  credential = nullptr;
  EXPECT_EQ(detail::credentials(&credential, "ssh://example.test", nullptr,
                                GIT_CREDENTIAL_SSH_KEY, nullptr),
            GIT_PASSTHROUGH);
  EXPECT_NE(detail::credentials(&credential, "ssh://example.test", "git",
                                GIT_CREDENTIAL_SSH_KEY, nullptr),
            GIT_PASSTHROUGH);
  git_credential_free(credential);
}

TEST_F(RepositoryTest, UndoReachesTheInitialOperation) {
  ASSERT_EQ(invoke({"new", "main"}).code, 0);
  ASSERT_EQ(invoke({"undo"}).code, 0);
  EXPECT_EQ(invoke({"undo"}).code, 2);
  EXPECT_FALSE(has_ref(detail::kWorkspaceRef));
}

TEST_F(RepositoryTest, RestoresAnUnbornOperationWithoutAWorkspace) {
  detail::Repository repo(path_);
  detail::OperationState state;
  state.head = {true, "refs/heads/does-not-exist"};
  const git_oid operation = repo.create_operation(state, std::nullopt);
  EXPECT_NO_THROW(repo.restore_operation(operation));
  EXPECT_FALSE(repo.head_oid().has_value());
}

}  // namespace gg::test
