// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "test_support.hpp"

#include "commands.hpp"

#include <git2/sys/errors.h>

#include <algorithm>

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
  repo.apply_refs({{"refs/remotes/origin/main", base},
                   {"refs/gg/remotes/origin/tags/v1", base}},
                  {}, "test refs");
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
  repo.apply_refs({{std::string(detail::kChangePrefix) + id, base}}, {},
                  "test change");
  EXPECT_EQ(repo.short_change_id(id), id.substr(0, 8));
  const git_oid other = raw_commit("other");
  repo.apply_refs({{std::string(detail::kChangePrefix) + "a1", base},
                   {std::string(detail::kChangePrefix) + "a2", other}},
                  {}, "test ambiguous changes");
  EXPECT_THROW(repo.resolve("a"), detail::UserError);
  repo.apply_refs({{std::string(detail::kChangePrefix) + "b1", base},
                   {std::string(detail::kChangePrefix) + "b2", base}},
                  {}, "test matching changes");
  const git_oid matching = repo.resolve("b");
  EXPECT_TRUE(git_oid_equal(&matching, &base) != 0);
  EXPECT_EQ(repo.short_change_id("a"), "a");

  std::string first(32, 'k');
  std::string second = first;
  first.replace(0, 9, "zzzzzzzzl");
  second.replace(0, 9, "zzzzzzzzm");
  repo.apply_refs({{std::string(detail::kChangePrefix) + "zzzz", base},
                   {std::string(detail::kChangePrefix) + first, base},
                   {std::string(detail::kChangePrefix) + second, other}},
                  {}, "test long prefixes");
  EXPECT_EQ(repo.short_change_id(first), first.substr(0, 9));
  EXPECT_EQ(repo.short_change_id(second), second.substr(0, 9));
  const detail::ShortId short_change = repo.short_change_id_parts(first);
  EXPECT_EQ(short_change.value, first.substr(0, 9));
  EXPECT_EQ(short_change.prefix_length, 9U);
  const detail::ShortId short_commit = repo.short_commit_id(base);
  const std::string base_text = detail::oid_string(base);
  const std::string other_text = detail::oid_string(other);
  std::size_t common = 0;
  while (base_text[common] == other_text[common]) ++common;
  EXPECT_EQ(short_commit.prefix_length, common + 1);
  EXPECT_EQ(short_commit.value.size(), 8U);

  repo.apply_refs({}, {}, "no changes");
}

TEST_F(RepositoryTest, RendersJujutsuStyleGraphRows) {
  for (std::uint16_t links = 0; links < 128; ++links) {
    EXPECT_FALSE(detail::graph_link_glyph_for_test(links, false).empty());
    EXPECT_FALSE(detail::graph_link_glyph_for_test(links, true).empty());
  }

  const git_oid a = raw_commit("a");
  const git_oid b = raw_commit("b");
  const git_oid c = raw_commit("c");
  const git_oid d = raw_commit("d");
  const git_oid p = raw_commit("p");
  const git_oid q = raw_commit("q");
  const git_oid r = raw_commit("r");
  const git_oid s = raw_commit("s");
  const git_oid x = raw_commit("x");
  const git_oid y = raw_commit("y");
  detail::GraphRenderer graph;
  std::ostringstream output;
  graph.add(output, a, std::vector<git_oid>{p}, "○", "");
  graph.add(output, b, std::vector<git_oid>{q}, "○", "b\n");
  graph.add(output, c, std::vector<git_oid>{r}, "○", "c\n");
  graph.add(output, d, std::vector<git_oid>{s}, "○", "d\n");
  graph.add(output, p, std::vector<git_oid>{s}, "○", "p\nlink\npad\n");
  graph.add(output, q, std::vector<git_oid>{s}, "○", "q\n");
  graph.add(output, x, std::vector<git_oid>{s, r}, "○", "x\n");
  graph.add(output, y, {}, "", "");
  EXPECT_NE(output.str().find("──"), std::string::npos);
  EXPECT_NE(output.str().find("├"), std::string::npos);
  EXPECT_NE(output.str().find("╯"), std::string::npos);
}

TEST_F(RepositoryTest, AssignsGgIdsToReachableHistory) {
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
  EXPECT_NO_THROW(repo.import_git_history());

  const std::string invalid(32, 'a');
  set_ref(std::string(detail::kChangePrefix) + invalid, base);
  EXPECT_TRUE(repo.invalid_change_id_refs().contains(
      std::string(detail::kChangePrefix) + invalid));
}

TEST_F(RepositoryTest, ReplacesCommitHashChangeIdsWhenReadingARepository) {
  const git_oid base = ref("HEAD");
  const git_oid child = raw_commit("child", {base});
  set_ref("refs/heads/side", child);
  const std::string legacy =
      std::string(detail::kChangePrefix) + detail::oid_string(base);
  set_ref(legacy, base);

  const Result log = invoke({"log", "-r", "ancestors(side)", "--no-graph"});
  ASSERT_EQ(log.code, 0) << log.error;
  detail::Repository repo(path_);
  EXPECT_FALSE(repo.ref_target(legacy).has_value());
  EXPECT_TRUE(repo.change_id(base).has_value());
  EXPECT_TRUE(repo.change_id(child).has_value());
  for (const auto& [id, oid] : repo.changes()) {
    (void)oid;
    EXPECT_EQ(id.size(), 32U);
    EXPECT_EQ(id.find_first_not_of("zyxwvutsrqponmlk"), std::string::npos);
  }
  set_ref(legacy, base);
  ASSERT_EQ(invoke({"log", "-r", "side", "--no-graph"}).code, 0);
  EXPECT_FALSE(detail::Repository(path_).ref_target(legacy).has_value());
}

TEST_F(RepositoryTest, AppliesLargeAtomicReferenceUpdates) {
  detail::Repository repo(path_);
  const git_oid base = ref("HEAD");
  std::map<std::string, git_oid> updates;
  for (int index = 0; index < 1100; ++index) {
    updates.emplace("refs/heads/large-" + std::to_string(index), base);
  }
  EXPECT_NO_THROW(repo.apply_refs(updates, {}, "large update"));
  EXPECT_TRUE(repo.ref_target("refs/heads/large-1099").has_value());
}

TEST_F(RepositoryTest, ResolvesRevisionSetExpressions) {
  detail::Repository repo(path_);
  const git_oid base = ref("HEAD");
  const git_oid left = raw_commit("left", {base});
  const git_oid right = raw_commit("right", {base});
  const git_oid merge = raw_commit("merge", {left, right});
  const git_oid tip = raw_commit("tip", {merge});
  const git_oid other = raw_commit("other", {base});
  set_ref("refs/heads/base", base);
  set_ref("refs/heads/left", left);
  set_ref("refs/heads/right", right);
  set_ref("refs/heads/merge", merge);
  set_ref("refs/heads/tip", tip);
  set_ref("refs/heads/other", other);
  set_ref("refs/tags/release", right);

  const auto contains = [](const std::vector<git_oid>& revisions,
                           const git_oid& oid) {
    return std::ranges::any_of(revisions, [&](const git_oid& candidate) {
      return git_oid_equal(&candidate, &oid) != 0;
    });
  };
  const auto expect = [&](std::string_view expression,
                          std::initializer_list<git_oid> expected) {
    const std::vector<git_oid> actual = repo.resolve_set(expression);
    EXPECT_EQ(actual.size(), expected.size()) << expression;
    for (const git_oid& oid : expected) {
      EXPECT_TRUE(contains(actual, oid)) << expression;
    }
  };

  expect("left | (right | left)", {left, right});
  expect("left~right", {left});
  expect("ancestors(left) & ancestors(right)", {base});
  expect("ancestors(tip) ~ ancestors(left)", {tip, merge, right});
  expect("left..tip", {tip, merge, right});
  expect("left::tip", {left, merge, tip});
  expect("parents(merge)", {left, right});
  expect("children(base)", {left, right, other});
  expect("descendants(left)", {left, merge, tip});
  expect("roots(all())", {base});
  expect("root()", {base});
  expect("heads(all())", {tip, other});
  expect("heads()", {tip, other});
  expect("bookmarks('glob:l*')", {left});
  expect("(bookmarks('glob:l*'))", {left});
  expect("bookmarks(glob:l*)", {left});
  expect("bookmarks(\"exact:right\")", {right});
  expect("bookmarks(x)", {});
  expect("tags()", {right});
  expect("ancestors(tip, 1)", {tip, merge});
  expect("first_ancestors(merge, 1)", {merge, left});
  expect("first_ancestors(tip)", {tip, merge, left, base});
  expect("present(missing)", {});
  expect("visible_heads()", {tip, other});
  expect("merges()", {merge});
  expect("description(exact:merge)", {merge});
  expect("author(substring:GG Test)", {base, left, right, merge, tip, other});
  expect("committer(exact:GG Test <gg@example.test>)",
         {base, left, right, merge, tip, other});
  expect("empty()", {left, right, tip, other});
  expect("commit_id('" + detail::oid_string(tip, 8) + "')", {tip});
  const std::string tip_change(32, 'k');
  repo.apply_refs({{std::string(detail::kChangePrefix) + tip_change, tip}}, {},
                  "test revset IDs");
  expect("change_id('kkkkkkkk')", {tip});
  set_ref("refs/remotes/origin/HEAD", tip);
  set_ref("refs/remotes/origin/tracked", left);
  set_ref("refs/remotes/origin/untracked", right);
  set_ref("refs/remotes/orphan", other);
  set_ref("refs/gg/tracking/bookmarks/origin/tracked", left);
  expect("remote_bookmarks()", {left, right});
  expect("remote_bookmarks(exact:tracked)", {left});
  expect("tracked_remote_bookmarks()", {left});
  expect("untracked_remote_bookmarks()", {right});
  git_oid conflict_blob{};
  ASSERT_EQ(git_blob_create_from_buffer(&conflict_blob, repository_.get(),
                                        "conflict", 8),
            0);
  detail::ConflictValue conflict;
  conflict.adds.push_back(
      {true, conflict_blob, GIT_FILEMODE_BLOB});
  detail::CommitPtr base_commit = repo.commit(base);
  repo.record_conflicts(*git_commit_tree_id(base_commit.get()),
                        {{"conflicted.txt", conflict}});
  expect("conflicts()", {base, left, right, merge, tip, other});
  expect("none()", {});
  const git_oid resolved_parent = repo.resolve("parents(left)");
  EXPECT_TRUE(git_oid_equal(&resolved_parent, &base) != 0);
  const git_oid git_parent = repo.resolve("tip~1");
  EXPECT_TRUE(git_oid_equal(&git_parent, &merge) != 0);
  const git_oid git_default_parent = repo.resolve("tip~");
  EXPECT_TRUE(git_oid_equal(&git_default_parent, &merge) != 0);

  EXPECT_THROW(repo.resolve("left | right"), detail::UserError);
  EXPECT_THROW(repo.resolve("none()"), detail::UserError);
  EXPECT_THROW(repo.resolve_set(""), detail::UserError);
  EXPECT_THROW(repo.resolve_set("all(base)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("none(base)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("root(base)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("ancestors()"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("ancestors(tip, 1, 2)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("ancestors(tip, nope)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("present()"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("visible_heads(tip)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("merges(tip)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("description()"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("conflicts(tip)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("empty(tip)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("commit_id()"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("unknown(base)"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("left ~ /"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("(left"), detail::UserError);
  EXPECT_THROW(repo.resolve_set("(')"), detail::UserError);
}

TEST_F(RepositoryTest, EvaluatesFilesetExpressions) {
  EXPECT_TRUE(detail::fileset_matches("nested", "nested/file.txt"));
  EXPECT_TRUE(detail::fileset_matches("glob:*.txt", "nested/file.txt"));
  EXPECT_TRUE(
      detail::fileset_matches("glob('nested/*') & ~none()", "nested/a"));
  EXPECT_TRUE(detail::fileset_matches(
      "file('one.txt') | file('two.txt')", "two.txt"));
  EXPECT_FALSE(detail::fileset_matches(
      "all() ~ (glob('*.bin') | file('skip.txt'))", "skip.txt"));
  EXPECT_TRUE(detail::fileset_matches("(file('one.txt')) | none()",
                                      "one.txt"));
  EXPECT_TRUE(detail::fileset_matches("root(\"a/b\")", "a/b/c"));
  EXPECT_TRUE(detail::fileset_matches("'quoted.txt'", "quoted.txt"));
  EXPECT_THROW(detail::fileset_matches("../outside", "outside"),
               detail::UserError);
  EXPECT_THROW(detail::fileset_matches("glob('/absolute')", "absolute"),
               detail::UserError);
  EXPECT_THROW(detail::fileset_matches("glob:/absolute", "absolute"),
               detail::UserError);
  EXPECT_THROW(detail::fileset_matches("file()", "file"),
               detail::UserError);
  EXPECT_THROW(detail::fileset_matches("broken)", "broken"),
               detail::UserError);
  EXPECT_THROW(detail::fileset_matches("(broken", "broken"),
               detail::UserError);
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
  const git_oid first_operation =
      repo.create_operation(state, std::nullopt, "test operation");
  state.refs["refs/heads/changed"] = child;
  const git_oid second_operation =
      repo.create_operation(state, first_operation, "second operation");
  detail::CommitPtr operation = repo.commit(second_operation);
  EXPECT_TRUE(std::string_view(git_commit_message(operation.get()))
                  .starts_with("gg-operation-v3\n"));
  EXPECT_EQ(git_commit_parentcount(operation.get()), 2U);
  const auto previous = repo.operation_previous(operation.get());
  ASSERT_TRUE(previous.has_value());
  EXPECT_NE(git_oid_equal(&*previous, &first_operation), 0);
  EXPECT_EQ(repo.operation_description(operation.get()), "second operation");
  const detail::OperationState restored = repo.parse_operation(operation.get());
  ASSERT_EQ(restored.refs.size(), state.refs.size());
  for (const auto& [name, oid] : state.refs) {
    ASSERT_TRUE(restored.refs.contains(name));
    EXPECT_NE(git_oid_equal(&restored.refs.at(name), &oid), 0);
  }
  EXPECT_THROW(repo.create_operation(state, std::nullopt, ""), detail::GitError);
}

TEST_F(RepositoryTest, AssignsAStableIdWhenReadingAWorkspace) {
  set_ref(detail::kWorkspaceRef, ref("HEAD"));
  ASSERT_EQ(git_repository_set_head(repository_.get(),
                                    "refs/heads/does-not-exist"),
            0);
  const Result status = invoke({"status"});
  ASSERT_EQ(status.code, 0) << status.error;
  EXPECT_EQ(status.output.find("--------"), std::string::npos);
  detail::Repository repo(path_);
  EXPECT_TRUE(repo.change_id(ref(detail::kWorkspaceRef)).has_value());
  EXPECT_EQ(invoke({"workspace", "list"}).output.find("--------"),
            std::string::npos);

  std::set<std::string> ids;
  for (const auto& [id, oid] : repo.changes()) {
    (void)oid;
    ids.insert(std::string(detail::kChangePrefix) + id);
  }
  repo.apply_refs({}, ids, "remove IDs for fallback rendering");
  EXPECT_NE(invoke({"status"}).output.find("--------"), std::string::npos);
  EXPECT_NE(invoke({"workspace", "list"}).output.find("--------"),
            std::string::npos);
  const Result log = invoke({"log"});
  EXPECT_EQ(log.code, 0) << log.error;
  EXPECT_NE(log.output.find("@"), std::string::npos);
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
  EXPECT_THROW(repo.parse_operation(raw_commit("gg-operation-v2-bad")),
               detail::GitError);

  detail::CommitPtr head = repo.commit(ref("HEAD"));
  const git_oid state_tree = *git_commit_tree_id(head.get());
  git_treebuilder* builder = nullptr;
  ASSERT_EQ(git_treebuilder_new(&builder, repository_.get(), nullptr), 0);
  ASSERT_EQ(git_treebuilder_insert(nullptr, builder, "state", &state_tree,
                                   GIT_FILEMODE_TREE),
            0);
  git_oid invalid_tree{};
  ASSERT_EQ(git_treebuilder_write(&invalid_tree, builder), 0);
  git_treebuilder_free(builder);
  const git_oid invalid_v3 = repo.create_commit(
      invalid_tree, {},
      "gg-operation-v3\nprevious -\ndescription invalid state\n");
  EXPECT_THROW(repo.parse_operation(invalid_v3), detail::GitError);

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

TEST_F(RepositoryTest, RejectsLegacyPendingRewrites) {
  set_ref(detail::kRewriteRef, raw_commit("bad"));
  const Result result = invoke({"status"});
  EXPECT_EQ(result.code, 2);
  EXPECT_NE(result.error.find("legacy paused rewrite"), std::string::npos);
}

TEST_F(RepositoryTest, RejectsMalformedConflictMetadata) {
  const git_oid head = ref("HEAD");
  git_commit* raw_head = nullptr;
  ASSERT_EQ(git_commit_lookup(&raw_head, repository_.get(), &head), 0);
  const git_oid tree = *git_commit_tree_id(raw_head);
  git_commit_free(raw_head);
  const std::string reference =
      std::string(detail::kConflictPrefix) + detail::oid_string(tree);

  set_ref(reference, raw_commit("bad"));
  EXPECT_THROW(detail::Repository(path_).tree_conflicts(tree),
               detail::GitError);
  set_ref(reference, raw_commit("gg-conflicts-v1\n\"path\"\nX - 0\nE\n"));
  EXPECT_THROW(detail::Repository(path_).tree_conflicts(tree),
               detail::GitError);
  set_ref(reference, raw_commit("gg-conflicts-v1\n\"path\"\nR - 0\nE\n"));
  EXPECT_THROW(detail::Repository(path_).tree_conflicts(tree),
               detail::GitError);
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
