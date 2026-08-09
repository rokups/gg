// Copyright (c) 2026-2026 the gg project.
// This work is licensed under the terms of the GNU General Public License version 2.
// For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file.

#include "gg/gg.h"

#include "test_support.hpp"

#include <cstring>

namespace gg::test {

TEST_F(RepositoryTest, ExposesStructuredCWorkflowApi) {
  gg_operation_options operation = GG_OPERATION_OPTIONS_INIT;
  gg_new_options new_options = GG_NEW_OPTIONS_INIT;
  gg_commit_options commit_options = GG_COMMIT_OPTIONS_INIT;
  gg_describe_options describe_options = GG_DESCRIBE_OPTIONS_INIT;
  gg_metaedit_options metaedit_options = GG_METAEDIT_OPTIONS_INIT;
  gg_rebase_options rebase_options = GG_REBASE_OPTIONS_INIT;
  gg_reorder_options reorder_options = GG_REORDER_OPTIONS_INIT;
  gg_split_options split_options = GG_SPLIT_OPTIONS_INIT;
  gg_squash_options squash_options = GG_SQUASH_OPTIONS_INIT;
  gg_abandon_options abandon_options = GG_ABANDON_OPTIONS_INIT;
  gg_restore_options restore_options = GG_RESTORE_OPTIONS_INIT;
  gg_move_files_options move_files_options = GG_MOVE_FILES_OPTIONS_INIT;
  gg_simplify_parents_options simplify_options =
      GG_SIMPLIFY_PARENTS_OPTIONS_INIT;
  gg_bookmark_options bookmark_options = GG_BOOKMARK_OPTIONS_INIT;
  gg_tag_options tag_options = GG_TAG_OPTIONS_INIT;
  gg_move_options move_options = GG_MOVE_OPTIONS_INIT;
  gg_workspace_add_options workspace_options = GG_WORKSPACE_ADD_OPTIONS_INIT;
  gg_fetch_options fetch_options = GG_FETCH_OPTIONS_INIT;
  gg_push_options push_options = GG_PUSH_OPTIONS_INIT;
  gg_revision_query_options revision_options =
      GG_REVISION_QUERY_OPTIONS_INIT;
  gg_status_options status_options = GG_STATUS_OPTIONS_INIT;
  EXPECT_EQ(gg_operation_options_init(&operation, 1), GIT_OK);
  EXPECT_EQ(gg_new_options_init(&new_options, 1), GIT_OK);
  EXPECT_EQ(gg_commit_options_init(&commit_options, 1), GIT_OK);
  EXPECT_EQ(gg_describe_options_init(&describe_options, 1), GIT_OK);
  EXPECT_EQ(gg_metaedit_options_init(&metaedit_options, 1), GIT_OK);
  EXPECT_EQ(gg_rebase_options_init(&rebase_options, 1), GIT_OK);
  EXPECT_EQ(gg_reorder_options_init(&reorder_options, 1), GIT_OK);
  EXPECT_EQ(gg_split_options_init(&split_options, 1), GIT_OK);
  EXPECT_EQ(gg_squash_options_init(&squash_options, 1), GIT_OK);
  EXPECT_EQ(gg_abandon_options_init(&abandon_options, 1), GIT_OK);
  EXPECT_EQ(gg_restore_options_init(&restore_options, 1), GIT_OK);
  EXPECT_EQ(gg_move_files_options_init(&move_files_options, 1), GIT_OK);
  EXPECT_EQ(gg_simplify_parents_options_init(&simplify_options, 1), GIT_OK);
  EXPECT_EQ(gg_bookmark_options_init(&bookmark_options, 1), GIT_OK);
  EXPECT_EQ(gg_tag_options_init(&tag_options, 1), GIT_OK);
  EXPECT_EQ(gg_move_options_init(&move_options, 1), GIT_OK);
  EXPECT_EQ(gg_workspace_add_options_init(&workspace_options, 1), GIT_OK);
  EXPECT_EQ(gg_fetch_options_init(&fetch_options, 1), GIT_OK);
  EXPECT_EQ(gg_push_options_init(&push_options, 1), GIT_OK);
  EXPECT_EQ(gg_revision_query_options_init(&revision_options, 1), GIT_OK);
  EXPECT_EQ(gg_status_options_init(&status_options, 1), GIT_OK);

  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_operation_capabilities capabilities{};
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  EXPECT_TRUE(capabilities.can_undo);
  EXPECT_FALSE(capabilities.can_redo);

  revision_options.revisions = "HEAD";
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &revision_options),
            GIT_OK);
  ASSERT_EQ(revisions.count, 1U);
  EXPECT_NE(revisions.items[0].change_id, nullptr);
  EXPECT_STREQ(revisions.items[0].description, "base");
  EXPECT_STREQ(revisions.items[0].author->name, "GG Test");
  gg_revision_array_dispose(&revisions);

  gg_status status{};
  ASSERT_EQ(gg_repository_status(&status, repository, &status_options), GIT_OK);
  EXPECT_FALSE(status.has_working_copy);
  gg_status_dispose(&status);

  new_options.message = "work";
  gg_mutation_result mutation{};
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &new_options,
                                     nullptr),
            GIT_OK);
  ASSERT_TRUE(mutation.changed);
  ASSERT_TRUE(mutation.has_working_copy);
  const git_oid working = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  git_oid queried_working{};
  ASSERT_EQ(gg_repository_working_copy(&queried_working, repository), GIT_OK);
  EXPECT_TRUE(git_oid_equal(&working, &queried_working));

  ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  EXPECT_TRUE(capabilities.can_undo);
  EXPECT_TRUE(capabilities.can_redo);
  ASSERT_EQ(gg_repository_redo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_operation_capabilities(&capabilities, repository),
            GIT_OK);
  EXPECT_TRUE(capabilities.can_undo);
  EXPECT_FALSE(capabilities.can_redo);

  char* change_id = nullptr;
  ASSERT_EQ(gg_repository_change_id(&change_id, repository, &working), GIT_OK);
  EXPECT_EQ(std::strlen(change_id), 32U);
  gg_string_dispose(change_id);

  gg_workspace_array workspaces{};
  ASSERT_EQ(gg_repository_workspaces(&workspaces, repository), GIT_OK);
  ASSERT_EQ(workspaces.count, 1U);
  EXPECT_STREQ(workspaces.items[0].name, "default");
  EXPECT_FALSE(workspaces.items[0].stale);
  gg_workspace_array_dispose(&workspaces);

  write("tracked.txt", "changed\n");
  int changed = 0;
  ASSERT_EQ(gg_repository_snapshot_working_copy(&changed, repository, nullptr),
            GIT_OK);
  EXPECT_TRUE(changed);
  ASSERT_EQ(gg_repository_status(&status, repository, &status_options), GIT_OK);
  ASSERT_TRUE(status.has_working_copy);
  ASSERT_EQ(status.entry_count, 1U);
  EXPECT_EQ(status.entries[0].status, GIT_DELTA_MODIFIED);
  EXPECT_STREQ(status.entries[0].new_path, "tracked.txt");
  gg_status_dispose(&status);

  const char* tracked_path[] = {"tracked.txt"};
  const gg_string_array tracked_paths{tracked_path, 1};
  ASSERT_EQ(gg_repository_untrack_paths(&mutation, repository, tracked_paths,
                                        nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);
  ASSERT_EQ(gg_repository_track_paths(&mutation, repository, tracked_paths, 0,
                                      nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);

  const char* topic[] = {"topic"};
  bookmark_options.action = GG_BOOKMARK_CREATE;
  bookmark_options.names = {topic, 1};
  bookmark_options.revision = "@";
  ASSERT_EQ(gg_repository_bookmark(&mutation, repository, &bookmark_options,
                                   nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);
  tag_options.action = GG_TAG_SET;
  tag_options.names = {topic, 1};
  tag_options.revision = "@";
  ASSERT_EQ(gg_repository_tag(&mutation, repository, &tag_options, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  gg_reference_array references{};
  ASSERT_EQ(gg_repository_references(&references, repository), GIT_OK);
  EXPECT_GT(references.count, 3U);
  gg_reference_array_dispose(&references);
  gg_named_ref_array named_refs{};
  ASSERT_EQ(gg_repository_named_refs(&named_refs, repository), GIT_OK);
  ASSERT_EQ(named_refs.count, 3U);
  bool found_bookmark = false;
  bool found_tag = false;
  for (size_t index = 0; index < named_refs.count; ++index) {
    const gg_named_ref& item = named_refs.items[index];
    if (std::strcmp(item.name, "topic") == 0 &&
        item.kind == GG_NAMED_REF_LOCAL_BOOKMARK) {
      found_bookmark = true;
    }
    if (std::strcmp(item.name, "topic") == 0 &&
        item.kind == GG_NAMED_REF_LOCAL_TAG) {
      found_tag = true;
    }
    EXPECT_STREQ(item.remote, "");
    EXPECT_FALSE(item.conflicted);
  }
  EXPECT_TRUE(found_bookmark);
  EXPECT_TRUE(found_tag);
  gg_named_ref_array_dispose(&named_refs);
  gg_conflict_array conflicts{};
  ASSERT_EQ(gg_repository_conflicts(&conflicts, repository, &working), GIT_OK);
  EXPECT_EQ(conflicts.count, 0U);
  gg_conflict_array_dispose(&conflicts);
  gg_operation_array operations{};
  ASSERT_EQ(gg_repository_operations(&operations, repository, 100), GIT_OK);
  EXPECT_GT(operations.count, 0U);
  gg_operation_array_dispose(&operations);
  gg_owned_string_array sparse{};
  ASSERT_EQ(gg_repository_sparse_patterns(&sparse, repository), GIT_OK);
  ASSERT_EQ(sparse.count, 1U);
  EXPECT_STREQ(sparse.strings[0], ".");
  gg_owned_string_array_dispose(&sparse);

  git_remote* remote = nullptr;
  ASSERT_EQ(git_remote_create(&remote, repository_.get(), "origin",
                              path_.string().c_str()),
            GIT_OK);
  git_remote_free(remote);
  push_options.bookmarks = {topic, 1};
  gg_transport_plan push_plan{};
  ASSERT_EQ(gg_repository_plan_push(&push_plan, repository, &push_options),
            GIT_OK);
  ASSERT_EQ(push_plan.refspec_count, 1U);
  EXPECT_TRUE(push_plan.atomic);
  ASSERT_EQ(gg_repository_complete_push(&mutation, repository, &push_plan,
                                        nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  gg_transport_plan_dispose(&push_plan);

  const git_oid head = ref("HEAD");
  const gg_advertised_ref advertised{
      "origin", "main", head, GG_REMOTE_BRANCH};
  fetch_options.advertised_refs = &advertised;
  fetch_options.advertised_ref_count = 1;
  gg_transport_plan fetch_plan{};
  ASSERT_EQ(gg_repository_plan_fetch(&fetch_plan, repository, &fetch_options),
            GIT_OK);
  ASSERT_EQ(fetch_plan.refspec_count, 1U);
  set_ref("refs/remotes/origin/main", head);
  ASSERT_EQ(gg_repository_complete_fetch(&mutation, repository, &fetch_plan,
                                         nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  gg_transport_plan_dispose(&fetch_plan);

  gg_repository_free(repository);
}

TEST_F(RepositoryTest, RefreshesCachedReferencesAfterAdoptingGitChanges) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_named_ref_array refs{};
  ASSERT_EQ(gg_repository_named_refs(&refs, repository), GIT_OK);
  gg_named_ref_array_dispose(&refs);

  const git_oid head = ref("HEAD");
  git_reference* external = nullptr;
  ASSERT_EQ(git_reference_create(&external, repository_.get(),
                                 "refs/heads/external", &head, 0, nullptr),
            GIT_OK);
  git_reference_free(external);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  ASSERT_EQ(gg_repository_named_refs(&refs, repository), GIT_OK);
  bool found_external = false;
  for (size_t index = 0; index < refs.count; ++index) {
    if (std::strcmp(refs.items[index].name, "external") == 0 &&
        refs.items[index].kind == GG_NAMED_REF_LOCAL_BOOKMARK) {
      found_external = true;
    }
  }
  EXPECT_TRUE(found_external);
  gg_named_ref_array_dispose(&refs);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, MovesFilesBetweenChangesAtomically) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "source";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  write("tracked.txt", "moved\n");
  int changed = 0;
  ASSERT_EQ(gg_repository_snapshot_working_copy(&changed, repository, nullptr),
            GIT_OK);
  ASSERT_TRUE(changed);
  git_oid source{};
  ASSERT_EQ(gg_repository_working_copy(&source, repository), GIT_OK);

  create.message = "destination";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid destination = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);

  const char* path[] = {"tracked.txt"};
  const std::string source_text = git_oid_tostr_s(&source);
  const std::string destination_text = git_oid_tostr_s(&destination);
  gg_move_files_options move = GG_MOVE_FILES_OPTIONS_INIT;
  move.source = source_text.c_str();
  move.destination = destination_text.c_str();
  move.filesets = {path, 1};
  ASSERT_EQ(gg_repository_move_files(&mutation, repository, &move, nullptr),
            GIT_OK)
      << git_error_last()->message;
  ASSERT_TRUE(mutation.changed);

  git_oid rewritten_source{};
  git_oid rewritten_destination{};
  bool found_source = false;
  bool found_destination = false;
  for (size_t index = 0; index < mutation.rewrite_count; ++index) {
    if (git_oid_equal(&mutation.rewrites[index].before, &source) != 0) {
      rewritten_source = mutation.rewrites[index].after;
      found_source = true;
    }
    if (git_oid_equal(&mutation.rewrites[index].before, &destination) != 0) {
      rewritten_destination = mutation.rewrites[index].after;
      found_destination = true;
    }
  }
  ASSERT_TRUE(found_source);
  ASSERT_TRUE(found_destination);

  const auto delta_count = [&](const git_oid& oid) {
    git_commit* commit = nullptr;
    EXPECT_EQ(git_commit_lookup(&commit, repository_.get(), &oid), GIT_OK);
    git_commit* parent = nullptr;
    EXPECT_EQ(git_commit_parent(&parent, commit, 0), GIT_OK);
    git_tree* tree = nullptr;
    git_tree* parent_tree = nullptr;
    EXPECT_EQ(git_commit_tree(&tree, commit), GIT_OK);
    EXPECT_EQ(git_commit_tree(&parent_tree, parent), GIT_OK);
    git_diff* diff = nullptr;
    EXPECT_EQ(git_diff_tree_to_tree(&diff, repository_.get(), parent_tree, tree,
                                    nullptr),
              GIT_OK);
    const size_t result = git_diff_num_deltas(diff);
    git_diff_free(diff);
    git_tree_free(parent_tree);
    git_tree_free(tree);
    git_commit_free(parent);
    git_commit_free(commit);
    return result;
  };
  EXPECT_EQ(delta_count(rewritten_source), 0U);
  EXPECT_EQ(delta_count(rewritten_destination), 1U);
  gg_mutation_result_dispose(&mutation);
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ReordersAStackAsOneCOperation) {
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "first";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid first = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "second";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);
  create.message = "third";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid third = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);

  gg_operation_array before{};
  ASSERT_EQ(gg_repository_operations(&before, repository, 100), GIT_OK);
  const size_t before_count = before.count;
  gg_operation_array_dispose(&before);

  gg_reorder_options reorder = GG_REORDER_OPTIONS_INIT;
  const std::string third_text = git_oid_tostr_s(&third);
  const std::string first_text = git_oid_tostr_s(&first);
  reorder.source = third_text.c_str();
  reorder.target = first_text.c_str();
  reorder.placement = GG_REORDER_BEFORE;
  ASSERT_EQ(gg_repository_reorder(&mutation, repository, &reorder, nullptr),
            GIT_OK)
      << git_error_last()->message;
  EXPECT_TRUE(mutation.changed);
  EXPECT_TRUE(mutation.has_operation);
  EXPECT_GE(mutation.rewrite_count, 3U);
  gg_mutation_result_dispose(&mutation);

  gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
  query.revisions = "all()";
  query.reversed = 1;
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &query), GIT_OK);
  ASSERT_GE(revisions.count, 3U);
  EXPECT_STREQ(revisions.items[revisions.count - 3].description, "third");
  EXPECT_STREQ(revisions.items[revisions.count - 2].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 1].description, "second");
  gg_revision_array_dispose(&revisions);

  gg_operation_array after{};
  ASSERT_EQ(gg_repository_operations(&after, repository, 100), GIT_OK);
  EXPECT_EQ(after.count, before_count + 1);
  gg_operation_array_dispose(&after);
  ASSERT_EQ(gg_repository_undo(&mutation, repository, nullptr), GIT_OK);
  gg_mutation_result_dispose(&mutation);

  git_oid working{};
  ASSERT_EQ(gg_repository_working_copy(&working, repository), GIT_OK);
  EXPECT_TRUE(git_oid_equal(&working, &third));
  gg_repository_free(repository);
}

TEST_F(RepositoryTest, ReordersARootChangeUsingAfterPlacement) {
  const git_oid base = ref("HEAD");
  gg_repository* repository = nullptr;
  ASSERT_EQ(gg_repository_attach(&repository, repository_.get()), GIT_OK);
  ASSERT_EQ(gg_repository_adopt_git_history(repository, nullptr), GIT_OK);

  gg_mutation_result mutation{};
  gg_new_options create = GG_NEW_OPTIONS_INIT;
  create.message = "first";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  const git_oid first = mutation.working_copy;
  gg_mutation_result_dispose(&mutation);
  create.message = "second";
  ASSERT_EQ(gg_repository_new_change(&mutation, repository, &create, nullptr),
            GIT_OK);
  gg_mutation_result_dispose(&mutation);

  const std::string base_text = git_oid_tostr_s(&base);
  const std::string first_text = git_oid_tostr_s(&first);
  gg_reorder_options reorder = GG_REORDER_OPTIONS_INIT;
  reorder.source = base_text.c_str();
  reorder.target = first_text.c_str();
  reorder.placement = GG_REORDER_AFTER;
  ASSERT_EQ(gg_repository_reorder(&mutation, repository, &reorder, nullptr),
            GIT_OK);
  EXPECT_TRUE(mutation.changed);
  gg_mutation_result_dispose(&mutation);

  gg_revision_query_options query = GG_REVISION_QUERY_OPTIONS_INIT;
  query.revisions = "all()";
  query.reversed = 1;
  gg_revision_array revisions{};
  ASSERT_EQ(gg_repository_revisions(&revisions, repository, &query), GIT_OK);
  ASSERT_GE(revisions.count, 3U);
  EXPECT_STREQ(revisions.items[revisions.count - 3].description, "first");
  EXPECT_STREQ(revisions.items[revisions.count - 2].description, "base");
  EXPECT_STREQ(revisions.items[revisions.count - 1].description, "second");
  gg_revision_array_dispose(&revisions);
  gg_repository_free(repository);
}

}  // namespace gg::test
