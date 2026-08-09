/* Copyright (c) 2026-2026 the gg project.
 * This work is licensed under the terms of the GNU General Public License version 2.
 * For a copy, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0.html> or the accompanying LICENSE file. */

#include "gg/gg.h"

#include <assert.h>
#include <string.h>

static int cancel(void *payload) {
  int *calls = payload;
  ++*calls;
  return 1;
}

int main(void) {
  assert(git_libgit2_init() > 0);

  gg_operation_options operation = GG_OPERATION_OPTIONS_INIT;
  gg_new_options new_options = GG_NEW_OPTIONS_INIT;
  assert(operation.version == GG_OPTIONS_VERSION);
  assert(new_options.version == GG_OPTIONS_VERSION);
  assert(gg_operation_options_init(&operation, GG_OPTIONS_VERSION) == GIT_OK);
  assert(gg_new_options_init(&new_options, GG_OPTIONS_VERSION) == GIT_OK);
  assert(gg_new_options_init(&new_options, 0) == GIT_EINVALID);

  git_repository *raw = NULL;
  assert(git_repository_open_ext(&raw, ".", GIT_REPOSITORY_OPEN_CROSS_FS,
                                 NULL) == GIT_OK);
  gg_repository *repository = NULL;
  assert(gg_repository_attach(&repository, raw) == GIT_OK);
  assert(gg_repository_raw(repository) == raw);

  git_oid head;
  assert(gg_repository_resolve(&head, repository, "HEAD") == GIT_OK);
  assert(gg_repository_resolve(&head, repository, "missing-revision") ==
         GIT_ENOTFOUND);
  assert(strstr(git_error_last()->message, "not found") != NULL);
  gg_oid_array revisions = {0};
  assert(gg_repository_resolve_set(&revisions, repository, "HEAD") == GIT_OK);
  assert(revisions.count == 1);
  assert(git_oid_equal(&head, &revisions.ids[0]));
  gg_oid_array_dispose(&revisions);

  gg_reference_array references = {0};
  assert(gg_repository_references(&references, repository) == GIT_OK);
  assert(references.count != 0);
  gg_reference_array_dispose(&references);

  int cancel_calls = 0;
  operation.cancel_cb = cancel;
  operation.payload = &cancel_calls;
  assert(gg_repository_adopt_git_history(repository, &operation) == GIT_EUSER);
  assert(cancel_calls == 1);
  assert(strstr(git_error_last()->message, "cancelled") != NULL);

  assert(gg_repository_resolve(NULL, repository, "HEAD") == GIT_EINVALID);
  assert(strstr(git_error_last()->message, "must not be null") != NULL);
  gg_repository_free(repository);
  assert(git_repository_path(raw) != NULL);
  git_repository_free(raw);

  gg_owned_string_array_dispose(NULL);
  gg_mutation_result_dispose(NULL);
  gg_string_dispose(NULL);
  git_libgit2_shutdown();
  return 0;
}
