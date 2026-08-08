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
  set_ref("refs/gg/remotes/origin/tags/v1", base);
  const auto refs = repo.data_refs();
  EXPECT_FALSE(refs.contains("refs/heads/unborn-link"));
  EXPECT_TRUE(refs.contains("refs/gg/remotes/origin/tags/v1"));
  EXPECT_FALSE(repo.rewrite_refs().contains("refs/remotes/origin/main"));
  EXPECT_FALSE(
      repo.rewrite_refs().contains("refs/gg/remotes/origin/tags/v1"));

  const std::string id = repo.new_change_id();
  EXPECT_EQ(id.size(), 32U);
  for (char digit : id) {
    EXPECT_GE(digit, 'k');
    EXPECT_LE(digit, 'z');
  }
  set_ref(std::string(detail::kChangePrefix) + id, base);
  EXPECT_EQ(repo.short_change_id(id), id.substr(0, 8));
  set_ref(std::string(detail::kChangePrefix) + "a1", base);
  const git_oid other = raw_commit("other");
  set_ref(std::string(detail::kChangePrefix) + "a2", other);
  EXPECT_THROW(repo.resolve("a"), detail::UserError);
  set_ref(std::string(detail::kChangePrefix) + "b1", base);
  set_ref(std::string(detail::kChangePrefix) + "b2", base);
  const git_oid matching = repo.resolve("b");
  EXPECT_TRUE(git_oid_equal(&matching, &base) != 0);
  EXPECT_EQ(repo.short_change_id("a"), "a");

  std::string first(32, 'k');
  std::string second = first;
  first.replace(0, 9, "zzzzzzzzl");
  second.replace(0, 9, "zzzzzzzzm");
  set_ref(std::string(detail::kChangePrefix) + "zzzz", base);
  set_ref(std::string(detail::kChangePrefix) + first, base);
  set_ref(std::string(detail::kChangePrefix) + second, other);
  EXPECT_EQ(repo.short_change_id(first), first.substr(0, 9));
  EXPECT_EQ(repo.short_change_id(second), second.substr(0, 9));

  repo.apply_refs({}, {}, "no changes");
  EXPECT_THROW(repo.abort_rewrite(), detail::UserError);
  EXPECT_THROW(repo.prepare_continue(), detail::UserError);
  std::ostringstream status;
  repo.pending_status(status);
  EXPECT_TRUE(status.str().empty());
  repo.finish_rewrite();
}

TEST_F(RepositoryTest, AssignsGgIdsToReachableGitHistory) {
  detail::Repository repo(path_);
  const git_oid base = ref("HEAD");
  const git_oid child = raw_commit("child", {base});
  set_ref("refs/heads/side", child);
  git_oid blob{};
  ASSERT_EQ(git_blob_create_from_buffer(&blob, repository_.get(), "blob", 4),
            0);
  set_ref("refs/tags/blob", blob);

  const auto updates = repo.missing_change_ids();
  ASSERT_EQ(updates.size(), 2U);
  for (const auto& [reference, oid] : updates) {
    (void)oid;
    ASSERT_TRUE(detail::starts_with(reference, detail::kChangePrefix));
    const std::string id = reference.substr(detail::kChangePrefix.size());
    EXPECT_EQ(id.size(), 32U);
    EXPECT_EQ(id.find_first_not_of("zyxwvutsrqponmlk"), std::string::npos);
  }
  repo.apply_refs(updates, {}, "assign change IDs");
  EXPECT_TRUE(repo.change_id(base).has_value());
  EXPECT_TRUE(repo.change_id(child).has_value());
  EXPECT_TRUE(repo.missing_change_ids().empty());
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
  EXPECT_NO_THROW(repo.create_operation(state, std::nullopt, "test operation"));
  EXPECT_THROW(repo.create_operation(state, std::nullopt, ""), detail::GitError);
}

TEST_F(RepositoryTest, ReportsWorkspaceWithoutAStableChangeId) {
  set_ref(detail::kWorkspaceRef, ref("HEAD"));
  ASSERT_EQ(git_repository_set_head(repository_.get(),
                                    "refs/heads/does-not-exist"),
            0);
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_NE(status.output.find("--------"), std::string::npos);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("--------"),
            std::string::npos);
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
      "gg-operation-v2\nwrong -\ndescription test\nhead S refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(bad_previous), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit("gg-operation-v2\n")),
               detail::GitError);
  const git_oid missing_description_line =
      raw_commit("gg-operation-v2\nprevious -\n");
  EXPECT_THROW(repo.parse_operation(missing_description_line), detail::GitError);

  const git_oid bad_head = raw_commit(
      "gg-operation-v2\nprevious -\ndescription test\nhead X refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(bad_head), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit(
                   "gg-operation-v2\nprevious -\ndescription test\n")),
               detail::GitError);

  const git_oid bad_ref = raw_commit(
      "gg-operation-v2\nprevious -\ndescription test\nhead S refs/heads/main\nwrong 0000000000000000000000000000000000000000 ref\n");
  EXPECT_THROW(repo.parse_operation(bad_ref), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit(
                   "gg-operation-v2\nprevious -\ndescription test\n"
                   "head S refs/heads/main\nref")),
               detail::GitError);
  EXPECT_THROW(repo.operation_previous(bad_header), detail::GitError);
  EXPECT_THROW(repo.operation_previous(raw_commit(
                   "gg-operation-v2\nwrong -\n")),
               detail::GitError);

  const git_oid empty_description = raw_commit(
      "gg-operation-v2\nprevious -\ndescription \nhead S refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(empty_description), detail::GitError);
  const git_oid missing_description = raw_commit(
      "gg-operation-v2\nprevious -\nhead S refs/heads/main\n");
  EXPECT_THROW(repo.parse_operation(missing_description), detail::GitError);
  const git_oid duplicate_description = raw_commit(
      "gg-operation-v2\nprevious -\ndescription first\n"
      "head S refs/heads/main\ndescription second\n");
  EXPECT_THROW(repo.parse_operation(duplicate_description), detail::GitError);
  const git_oid invalid_target = raw_commit(
      "gg-operation-v2\nprevious -\n"
      "description undo: restore to operation invalid\n"
      "head S refs/heads/main\n");
  EXPECT_THROW(repo.operation_target(invalid_target,
                                     "undo: restore to operation "),
               detail::GitError);
  EXPECT_THROW(repo.operation_description(bad_header), detail::GitError);
  EXPECT_THROW(repo.operation_description(raw_commit("")), detail::GitError);
  EXPECT_THROW(repo.operation_description(raw_commit("gg-operation-v2\n")),
               detail::GitError);
  EXPECT_THROW(repo.operation_description(raw_commit(
                   "gg-operation-v2\nwrong -\ndescription test\n")),
               detail::GitError);
  EXPECT_THROW(repo.operation_description(missing_description_line),
               detail::GitError);
  EXPECT_THROW(repo.operation_description(empty_description), detail::GitError);
  EXPECT_THROW(repo.operation_description(missing_description), detail::GitError);
  EXPECT_THROW(repo.parse_operation(raw_commit(
                   "gg-operation-v1\nprevious -\ndescription legacy\n"
                   "head S refs/heads/main\n")),
               detail::GitError);

  detail::OperationState state = repo.state();
  const git_oid invalid_undo = repo.create_operation(
      state, std::nullopt,
      "undo: restore to operation " + detail::oid_string(ref("HEAD")));
  set_ref(detail::kOperationRef, invalid_undo);
  EXPECT_EQ(invoke({"redo"}).code, 1);
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
  const git_oid operation =
      repo.create_operation(state, std::nullopt, "test operation");
  EXPECT_NO_THROW(repo.restore_operation(operation));
  EXPECT_FALSE(repo.head_oid().has_value());
}

TEST_F(RepositoryTest, ResolvesOperationExpressions) {
  detail::Repository repo(path_);
  EXPECT_THROW(repo.resolve_operation(""), detail::UserError);
  EXPECT_THROW(repo.resolve_operation("@"), detail::UserError);

  const detail::OperationState state = repo.state();
  git_oid current =
      repo.create_operation(state, std::nullopt, "initial operation");
  set_ref(detail::kOperationRef, current);
  EXPECT_THROW(repo.resolve_operation("@-"), detail::UserError);

  std::map<char, git_oid> prefixes;
  std::optional<char> ambiguous;
  for (int index = 0; index < 17; ++index) {
    current = repo.create_operation(
        state, current, "test operation " + std::to_string(index));
    const char prefix = git_oid_tostr_s(&current)[0];
    if (!prefixes.emplace(prefix, current).second) {
      ambiguous = prefix;
    }
  }
  set_ref(detail::kOperationRef, current);
  const git_oid resolved = repo.resolve_operation("@");
  EXPECT_NE(git_oid_equal(&resolved, &current), 0);
  EXPECT_NO_THROW(repo.resolve_operation("@-"));
  EXPECT_THROW(repo.resolve_operation("@-x"), detail::UserError);
  ASSERT_TRUE(ambiguous.has_value());
  EXPECT_THROW(repo.resolve_operation(std::string(1, *ambiguous)),
               detail::UserError);
}

}  // namespace gg::test
