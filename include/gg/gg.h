/* Copyright (c) 2026-2026 the gg project.
 * This work is licensed under the terms of the GNU General Public License version 2.
 * For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file. */

#ifndef INCLUDE_GG_GG_H
#define INCLUDE_GG_GG_H

#if defined(__has_include)
#  if __has_include(<git2-experimental.h>)
#    include <git2-experimental.h>
#  else
#    include <git2.h>
#  endif
#else
#  include <git2.h>
#endif

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(GG_SHARED)
#  if defined(GG_BUILDING_LIBRARY)
#    define GG_EXTERN __declspec(dllexport)
#  else
#    define GG_EXTERN __declspec(dllimport)
#  endif
#elif defined(__GNUC__)
#  define GG_EXTERN __attribute__((visibility("default")))
#else
#  define GG_EXTERN
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define GG_OPTIONS_VERSION 1

/* gg_repository borrows its git_repository for its entire lifetime. Every
 * function returns a libgit2 GIT_* code; failure details are available from
 * git_error_last(). Call the matching dispose function for owned outputs. */
typedef struct gg_repository gg_repository;

typedef struct gg_string_array {
  const char **strings;
  size_t count;
} gg_string_array;

typedef struct gg_owned_string_array {
  char **strings;
  size_t count;
} gg_owned_string_array;

typedef struct gg_oid_array {
  git_oid *ids;
  size_t count;
} gg_oid_array;

typedef struct gg_reference {
  char *name;
  git_oid target;
} gg_reference;

typedef struct gg_reference_array {
  gg_reference *items;
  size_t count;
} gg_reference_array;

typedef enum gg_named_ref_kind {
  GG_NAMED_REF_LOCAL_BOOKMARK,
  GG_NAMED_REF_REMOTE_BOOKMARK,
  GG_NAMED_REF_LOCAL_TAG,
  GG_NAMED_REF_REMOTE_TAG
} gg_named_ref_kind;

typedef struct gg_named_ref {
  char *name;
  char *remote;
  git_oid target;
  gg_named_ref_kind kind;
  int tracked;
  int conflicted;
} gg_named_ref;

typedef struct gg_named_ref_array {
  gg_named_ref *items;
  size_t count;
} gg_named_ref_array;

typedef struct gg_conflict_term {
  git_oid oid;
  git_filemode_t mode;
  int present;
} gg_conflict_term;

typedef struct gg_conflict {
  char *path;
  gg_conflict_term *removes;
  size_t remove_count;
  gg_conflict_term *adds;
  size_t add_count;
} gg_conflict;

typedef struct gg_conflict_array {
  gg_conflict *items;
  size_t count;
} gg_conflict_array;

typedef struct gg_reference_change {
  char *name;
  git_oid before;
  git_oid after;
  int had_before;
  int has_after;
} gg_reference_change;

typedef struct gg_rewrite {
  git_oid before;
  git_oid after;
} gg_rewrite;

typedef struct gg_mutation_result {
  unsigned int version;
  int changed;
  git_oid working_copy;
  git_oid operation;
  int has_working_copy;
  int has_operation;
  gg_rewrite *rewrites;
  size_t rewrite_count;
  gg_reference_change *references;
  size_t reference_count;
} gg_mutation_result;

typedef int (*gg_cancel_cb)(void *payload);
typedef void (*gg_progress_cb)(const char *phase,
                               size_t completed,
                               size_t total,
                               void *payload);

typedef struct gg_operation_options {
  unsigned int version;
  gg_cancel_cb cancel_cb;
  gg_progress_cb progress_cb;
  void *payload;
} gg_operation_options;

#define GG_OPERATION_OPTIONS_INIT { GG_OPTIONS_VERSION, NULL, NULL, NULL }

typedef struct gg_new_options {
  unsigned int version;
  const char *message;
  gg_string_array parents;
  gg_string_array insert_after;
  gg_string_array insert_before;
  int no_edit;
} gg_new_options;

#define GG_NEW_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, 0 }

typedef struct gg_commit_options {
  unsigned int version;
  gg_string_array filesets;
  const char *message;
  int message_provided;
} gg_commit_options;

#define GG_COMMIT_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, NULL, 0 }

typedef struct gg_describe_options {
  unsigned int version;
  gg_string_array revisions;
  const char *message;
  int message_provided;
} gg_describe_options;

#define GG_DESCRIBE_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, NULL, 1 }

typedef struct gg_metaedit_options {
  unsigned int version;
  gg_string_array revisions;
  const char *message;
  const char *author;
  const char *author_timestamp;
  int message_provided;
  int author_provided;
  int author_timestamp_provided;
  int update_change_id;
  int update_author;
  int update_author_timestamp;
  int force_rewrite;
} gg_metaedit_options;

#define GG_METAEDIT_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }

typedef struct gg_rebase_options {
  unsigned int version;
  const char *source;
  const char *destination;
} gg_rebase_options;

#define GG_REBASE_OPTIONS_INIT { GG_OPTIONS_VERSION, NULL, NULL }

typedef enum gg_reorder_placement {
  GG_REORDER_BEFORE,
  GG_REORDER_AFTER
} gg_reorder_placement;

typedef struct gg_reorder_options {
  unsigned int version;
  const char *source;
  const char *target;
  gg_reorder_placement placement;
} gg_reorder_options;

#define GG_REORDER_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, NULL, GG_REORDER_BEFORE }

typedef struct gg_split_options {
  unsigned int version;
  const char *revision;
  const char *message;
  gg_string_array filesets;
} gg_split_options;

#define GG_SPLIT_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, NULL, { NULL, 0 } }

typedef struct gg_squash_options {
  unsigned int version;
  const char *revision;
  const char *source;
  const char *destination;
  const char *message;
} gg_squash_options;

#define GG_SQUASH_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, NULL, NULL, NULL }

typedef struct gg_abandon_options {
  unsigned int version;
  gg_string_array revisions;
  int retain_bookmarks;
  int restore_descendants;
} gg_abandon_options;

#define GG_ABANDON_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, 0, 0 }

typedef struct gg_restore_options {
  unsigned int version;
  gg_string_array filesets;
  const char *from;
  const char *into;
  const char *changes_in;
  int restore_descendants;
} gg_restore_options;

#define GG_RESTORE_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, NULL, NULL, NULL, 0 }

typedef struct gg_move_files_options {
  unsigned int version;
  const char *source;
  const char *destination;
  gg_string_array filesets;
} gg_move_files_options;

#define GG_MOVE_FILES_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, NULL, { NULL, 0 } }

typedef struct gg_simplify_parents_options {
  unsigned int version;
  gg_string_array sources;
  gg_string_array revisions;
} gg_simplify_parents_options;

#define GG_SIMPLIFY_PARENTS_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, { NULL, 0 } }

typedef enum gg_bookmark_action {
  GG_BOOKMARK_ADVANCE,
  GG_BOOKMARK_CREATE,
  GG_BOOKMARK_SET,
  GG_BOOKMARK_MOVE,
  GG_BOOKMARK_DELETE,
  GG_BOOKMARK_FORGET,
  GG_BOOKMARK_RENAME,
  GG_BOOKMARK_TRACK,
  GG_BOOKMARK_UNTRACK
} gg_bookmark_action;

typedef struct gg_bookmark_options {
  unsigned int version;
  gg_bookmark_action action;
  gg_string_array names;
  gg_string_array from;
  gg_string_array remotes;
  const char *revision;
  int allow_backwards;
  int include_remotes;
  int overwrite_existing;
} gg_bookmark_options;

#define GG_BOOKMARK_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, GG_BOOKMARK_CREATE, { NULL, 0 }, { NULL, 0 }, \
    { NULL, 0 }, NULL, 0, 0, 0 }

typedef enum gg_tag_action {
  GG_TAG_SET,
  GG_TAG_DELETE,
  GG_TAG_TRACK,
  GG_TAG_UNTRACK
} gg_tag_action;

typedef struct gg_tag_options {
  unsigned int version;
  gg_tag_action action;
  gg_string_array names;
  gg_string_array remotes;
  const char *revision;
  int allow_move;
} gg_tag_options;

#define GG_TAG_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, GG_TAG_SET, { NULL, 0 }, { NULL, 0 }, NULL, 0 }

typedef enum gg_move_direction {
  GG_MOVE_NEXT,
  GG_MOVE_PREVIOUS
} gg_move_direction;

typedef struct gg_move_options {
  unsigned int version;
  gg_move_direction direction;
  uint64_t offset;
  int edit;
  int conflict;
} gg_move_options;

#define GG_MOVE_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, GG_MOVE_NEXT, 1, 0, 0 }

typedef struct gg_workspace_add_options {
  unsigned int version;
  const char *name;
  const char *destination;
  const char *revision;
  const char *message;
  const char *sparse_patterns;
} gg_workspace_add_options;

#define GG_WORKSPACE_ADD_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, NULL, NULL, NULL, "copy" }

typedef enum gg_restore_operation_flags {
  GG_RESTORE_REPOSITORY = 1u << 0,
  GG_RESTORE_REMOTE_TRACKING = 1u << 1,
  GG_RESTORE_ALL = GG_RESTORE_REPOSITORY | GG_RESTORE_REMOTE_TRACKING
} gg_restore_operation_flags;

typedef enum gg_remote_ref_kind {
  GG_REMOTE_BRANCH,
  GG_REMOTE_TAG
} gg_remote_ref_kind;

typedef struct gg_advertised_ref {
  const char *remote;
  const char *name;
  git_oid target;
  gg_remote_ref_kind kind;
} gg_advertised_ref;

typedef struct gg_fetch_options {
  unsigned int version;
  const gg_advertised_ref *advertised_refs;
  size_t advertised_ref_count;
  gg_string_array remotes;
  gg_string_array branches;
  gg_string_array tags;
  int tracked;
  int all_remotes;
} gg_fetch_options;

#define GG_FETCH_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, NULL, 0, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, 0, 0 }

typedef struct gg_push_options {
  unsigned int version;
  gg_string_array bookmarks;
  gg_string_array tags;
  gg_string_array revisions;
  gg_string_array push_options;
  const char *remote;
  int all;
  int tracked;
  int deleted;
  int allow_empty_description;
} gg_push_options;

#define GG_PUSH_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, { NULL, 0 }, { NULL, 0 }, \
    { NULL, 0 }, NULL, 0, 0, 0, 0 }

typedef struct gg_refspec {
  char *remote;
  char *source;
  char *destination;
  git_oid target;
  int has_target;
} gg_refspec;

typedef struct gg_transport_plan {
  unsigned int version;
  int atomic;
  gg_refspec *refspecs;
  size_t refspec_count;
  gg_owned_string_array reference_deletes;
  gg_owned_string_array push_options;
} gg_transport_plan;

typedef struct gg_revision {
  git_oid oid;
  gg_oid_array parents;
  char *change_id;
  char *description;
  git_signature *author;
  git_signature *committer;
  int has_conflicts;
} gg_revision;

typedef struct gg_revision_array {
  gg_revision *items;
  size_t count;
} gg_revision_array;

typedef struct gg_revision_query_options {
  unsigned int version;
  const char *revisions;
  const char *at_operation;
  size_t limit;
  int reversed;
} gg_revision_query_options;

#define GG_REVISION_QUERY_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, "all()", NULL, (size_t)-1, 0 }

typedef struct gg_status_entry {
  git_delta_t status;
  char *old_path;
  char *new_path;
  git_filemode_t old_mode;
  git_filemode_t new_mode;
  int conflicted;
} gg_status_entry;

typedef struct gg_status {
  int has_working_copy;
  git_oid working_copy;
  gg_oid_array parents;
  gg_status_entry *entries;
  size_t entry_count;
} gg_status;

typedef struct gg_status_options {
  unsigned int version;
  gg_string_array filesets;
  const char *at_operation;
} gg_status_options;

#define GG_STATUS_OPTIONS_INIT \
  { GG_OPTIONS_VERSION, { NULL, 0 }, NULL }

typedef struct gg_operation {
  git_oid oid;
  git_oid previous;
  int has_previous;
  char *description;
  git_time_t time;
  int offset;
} gg_operation;

typedef struct gg_operation_array {
  gg_operation *items;
  size_t count;
} gg_operation_array;

typedef struct gg_workspace {
  char *name;
  char *root;
  git_oid working_copy;
  int stale;
} gg_workspace;

typedef struct gg_workspace_array {
  gg_workspace *items;
  size_t count;
} gg_workspace_array;

GG_EXTERN int gg_operation_options_init(gg_operation_options *options,
                                        unsigned int version);
GG_EXTERN int gg_new_options_init(gg_new_options *options,
                                  unsigned int version);
GG_EXTERN int gg_commit_options_init(gg_commit_options *options,
                                     unsigned int version);
GG_EXTERN int gg_describe_options_init(gg_describe_options *options,
                                       unsigned int version);
GG_EXTERN int gg_metaedit_options_init(gg_metaedit_options *options,
                                       unsigned int version);
GG_EXTERN int gg_rebase_options_init(gg_rebase_options *options,
                                     unsigned int version);
GG_EXTERN int gg_reorder_options_init(gg_reorder_options *options,
                                      unsigned int version);
GG_EXTERN int gg_split_options_init(gg_split_options *options,
                                    unsigned int version);
GG_EXTERN int gg_squash_options_init(gg_squash_options *options,
                                     unsigned int version);
GG_EXTERN int gg_abandon_options_init(gg_abandon_options *options,
                                      unsigned int version);
GG_EXTERN int gg_restore_options_init(gg_restore_options *options,
                                      unsigned int version);
GG_EXTERN int gg_move_files_options_init(gg_move_files_options *options,
                                         unsigned int version);
GG_EXTERN int gg_simplify_parents_options_init(
    gg_simplify_parents_options *options, unsigned int version);
GG_EXTERN int gg_bookmark_options_init(gg_bookmark_options *options,
                                       unsigned int version);
GG_EXTERN int gg_tag_options_init(gg_tag_options *options,
                                  unsigned int version);
GG_EXTERN int gg_move_options_init(gg_move_options *options,
                                   unsigned int version);
GG_EXTERN int gg_workspace_add_options_init(gg_workspace_add_options *options,
                                            unsigned int version);
GG_EXTERN int gg_fetch_options_init(gg_fetch_options *options,
                                    unsigned int version);
GG_EXTERN int gg_push_options_init(gg_push_options *options,
                                   unsigned int version);
GG_EXTERN int gg_revision_query_options_init(
    gg_revision_query_options *options, unsigned int version);
GG_EXTERN int gg_status_options_init(gg_status_options *options,
                                     unsigned int version);

GG_EXTERN int gg_repository_attach(gg_repository **out,
                                   git_repository *repository);
GG_EXTERN void gg_repository_free(gg_repository *repository);
GG_EXTERN git_repository *gg_repository_raw(gg_repository *repository);

GG_EXTERN int gg_repository_adopt_git_history(
    gg_repository *repository, const gg_operation_options *options);
GG_EXTERN int gg_repository_snapshot_working_copy(
    int *changed, gg_repository *repository,
    const gg_operation_options *options);

GG_EXTERN int gg_repository_resolve(git_oid *out,
                                    gg_repository *repository,
                                    const char *revision);
GG_EXTERN int gg_repository_resolve_set(gg_oid_array *out,
                                        gg_repository *repository,
                                        const char *revisions);
GG_EXTERN int gg_repository_working_copy(git_oid *out,
                                         gg_repository *repository);
GG_EXTERN int gg_repository_change_id(char **out,
                                      gg_repository *repository,
                                      const git_oid *revision);
GG_EXTERN int gg_repository_references(gg_reference_array *out,
                                       gg_repository *repository);
GG_EXTERN int gg_repository_named_refs(gg_named_ref_array *out,
                                       gg_repository *repository);
GG_EXTERN int gg_repository_conflict_paths(gg_owned_string_array *out,
                                           gg_repository *repository,
                                           const git_oid *revision);
GG_EXTERN int gg_repository_conflicts(gg_conflict_array *out,
                                      gg_repository *repository,
                                      const git_oid *revision);
GG_EXTERN int gg_repository_revisions(
    gg_revision_array *out, gg_repository *repository,
    const gg_revision_query_options *options);
GG_EXTERN int gg_repository_status(gg_status *out,
                                    gg_repository *repository,
                                    const gg_status_options *options);
GG_EXTERN int gg_repository_operations(gg_operation_array *out,
                                       gg_repository *repository,
                                       size_t limit);
GG_EXTERN int gg_repository_workspaces(gg_workspace_array *out,
                                       gg_repository *repository);
GG_EXTERN int gg_repository_sparse_patterns(gg_owned_string_array *out,
                                            gg_repository *repository);

GG_EXTERN int gg_repository_new_change(
    gg_mutation_result *out, gg_repository *repository,
    const gg_new_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_commit(
    gg_mutation_result *out, gg_repository *repository,
    const gg_commit_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_describe(
    gg_mutation_result *out, gg_repository *repository,
    const gg_describe_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_metaedit(
    gg_mutation_result *out, gg_repository *repository,
    const gg_metaedit_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_edit(gg_mutation_result *out,
                                 gg_repository *repository,
                                 const char *revision,
                                 const gg_operation_options *operation);
GG_EXTERN int gg_repository_move(gg_mutation_result *out,
                                 gg_repository *repository,
                                 const gg_move_options *options,
                                 const gg_operation_options *operation);
GG_EXTERN int gg_repository_rebase(
    gg_mutation_result *out, gg_repository *repository,
    const gg_rebase_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_reorder(
    gg_mutation_result *out, gg_repository *repository,
    const gg_reorder_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_split(
    gg_mutation_result *out, gg_repository *repository,
    const gg_split_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_squash(
    gg_mutation_result *out, gg_repository *repository,
    const gg_squash_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_abandon(
    gg_mutation_result *out, gg_repository *repository,
    const gg_abandon_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_restore(
    gg_mutation_result *out, gg_repository *repository,
    const gg_restore_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_move_files(
    gg_mutation_result *out, gg_repository *repository,
    const gg_move_files_options *options,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_simplify_parents(
    gg_mutation_result *out, gg_repository *repository,
    const gg_simplify_parents_options *options,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_bookmark(
    gg_mutation_result *out, gg_repository *repository,
    const gg_bookmark_options *options, const gg_operation_options *operation);
GG_EXTERN int gg_repository_tag(gg_mutation_result *out,
                                gg_repository *repository,
                                const gg_tag_options *options,
                                const gg_operation_options *operation);
GG_EXTERN int gg_repository_undo(gg_mutation_result *out,
                                 gg_repository *repository,
                                 const gg_operation_options *operation);
GG_EXTERN int gg_repository_redo(gg_mutation_result *out,
                                 gg_repository *repository,
                                 const gg_operation_options *operation);
GG_EXTERN int gg_repository_restore_operation(
    gg_mutation_result *out, gg_repository *repository,
    const char *operation, unsigned int flags,
    const gg_operation_options *operation_options);
GG_EXTERN int gg_repository_workspace_add(
    gg_mutation_result *out, gg_repository *repository,
    const gg_workspace_add_options *options,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_workspace_forget(
    gg_mutation_result *out, gg_repository *repository,
    gg_string_array names, const gg_operation_options *operation);
GG_EXTERN int gg_repository_workspace_rename(
    gg_mutation_result *out, gg_repository *repository,
    const char *name, const gg_operation_options *operation);
GG_EXTERN int gg_repository_sparse_reset(
    gg_mutation_result *out, gg_repository *repository,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_track_paths(
    gg_mutation_result *out, gg_repository *repository,
    gg_string_array filesets, int include_ignored,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_untrack_paths(
    gg_mutation_result *out, gg_repository *repository,
    gg_string_array filesets, const gg_operation_options *operation);
GG_EXTERN int gg_repository_chmod(gg_mutation_result *out,
                                  gg_repository *repository,
                                  gg_string_array filesets,
                                  int executable,
                                  const gg_operation_options *operation);
GG_EXTERN int gg_repository_plan_fetch(gg_transport_plan *out,
                                       gg_repository *repository,
                                       const gg_fetch_options *options);
GG_EXTERN int gg_repository_complete_fetch(
    gg_mutation_result *out, gg_repository *repository,
    const gg_transport_plan *plan,
    const gg_operation_options *operation);
GG_EXTERN int gg_repository_plan_push(gg_transport_plan *out,
                                      gg_repository *repository,
                                      const gg_push_options *options);
GG_EXTERN int gg_repository_complete_push(
    gg_mutation_result *out, gg_repository *repository,
    const gg_transport_plan *plan,
    const gg_operation_options *operation);

GG_EXTERN void gg_oid_array_dispose(gg_oid_array *array);
GG_EXTERN void gg_owned_string_array_dispose(gg_owned_string_array *array);
GG_EXTERN void gg_reference_array_dispose(gg_reference_array *array);
GG_EXTERN void gg_named_ref_array_dispose(gg_named_ref_array *array);
GG_EXTERN void gg_conflict_array_dispose(gg_conflict_array *array);
GG_EXTERN void gg_mutation_result_dispose(gg_mutation_result *result);
GG_EXTERN void gg_transport_plan_dispose(gg_transport_plan *plan);
GG_EXTERN void gg_revision_array_dispose(gg_revision_array *array);
GG_EXTERN void gg_status_dispose(gg_status *status);
GG_EXTERN void gg_operation_array_dispose(gg_operation_array *array);
GG_EXTERN void gg_workspace_array_dispose(gg_workspace_array *array);
GG_EXTERN void gg_string_dispose(char *value);

#ifdef __cplusplus
}
#endif

#endif
