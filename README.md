# gg

`gg` brings a change-oriented, Jujutsu-inspired workflow to ordinary Git
repositories. Its primary purpose is to make local history easy to build,
rearrange, and revise: the working copy is a mutable change, old changes can be
edited directly, descendants are restacked automatically, and repository
changes can be undone.

`gg` does not require a new repository format or server support. Changes are Git
commits, local branches are exposed as **bookmarks**, and remotes receive no
custom objects, headers, notes, or refs. By default, use `gg` for change and
history editing and use Git for familiar operations such as clone, status,
diff, fetch, and push.

## What changes compared with Git

Git normally asks you to stage files, create a commit, and use a separate
history-editing workflow when an earlier commit needs to change. With `gg`:

- The working copy is a mutable change that `gg` snapshots automatically. A
  separate staging step is optional.
- Each change is identified by its current Git commit ID. When a rewrite
  changes that ID, its previous commit IDs remain usable as aliases.
- `gg edit` can make any change the working copy. Further edits rewrite that
  change and automatically restack its descendants.
- `gg new`, `gg split`, `gg squash`, `gg rebase`, and `gg abandon` operate on
  the change graph directly.
- `gg undo` and `gg redo` provide editor-style history for repository
  operations.

Revisions use `@` for the working-copy change and `@-` for its parent. Commands
also accept current or retained historical commit-ID prefixes, bookmarks, and
Git object IDs.

## Workflows

### Mutable changes

This is the main `gg` workflow. Start a named change, edit files normally, and
start the next change when you are ready. There is no commit step:

```sh
git clone URL project
cd project

gg new -m "Add the parser"
gg bookmark create topic
# edit files
gg log                    # snapshots and displays the current change

gg new -m "Add parser tests"
# edit files
gg bookmark advance topic
git push -u origin topic
```

Descriptions are editable metadata rather than a finalization boundary. Use
`gg describe -m "New description"` at any time. To revise an earlier change,
copy its commit ID from `gg log`, run `gg edit COMMIT_ID`, edit the files, and
run another `gg` command to snapshot the result. Descendant changes and affected
local refs are updated together; the copied ID remains an alias for the
rewritten commit.

### Commit-oriented changes

`gg commit` provides a more familiar boundary while retaining automatic
snapshotting, historical commit-ID aliases, restacking, and undo:

```sh
gg new
gg bookmark create topic

# edit files
gg commit -m "Add the parser"

# edit more files
gg commit -m "Add parser tests"

gg bookmark advance topic --to @-
git push -u origin topic
```

Each `gg commit` describes the current change and creates a new empty working
change. Filesets may be supplied to commit only part of the working change; the
remaining edits stay in the new working change:

```sh
gg commit -m "Add the parser" src/parser.cpp include/parser.hpp
gg commit -m "Commit sources except generated files" \
  "glob('src/*') ~ root('src/generated')"
```

### Using only `gg` commands

All implemented commands are available by default, including the complete
bookmark, file, util, and workspace families:

```sh
gg clone URL project
cd project
gg new -m "Add the parser"
# edit files
gg bookmark create topic
gg push --bookmark topic
```

Every command and command group prints its complete help with `--doc`, without
requiring a repository. Generated manuals and shell completions use the same
command schema.

## Storage model

Each working copy is represented by a commit under
`refs/gg/workspaces/<name>`. The primary checkout starts as `default`; linked
Git worktrees have their own names, working changes, and operation histories
while sharing commits, commit aliases, bookmarks, and tags. Historical commit
IDs are stored together under `refs/gg/commit-aliases` so repositories do not
expose one Git reference for every alias. Git `HEAD` stays at the working
change's parent so existing tooling continues to see normal working-tree
changes.

Use `gg workspace add` when creating another checkout so the Git worktree and
its gg working change are created together:

```sh
gg workspace add ../project-review --name review -r main
gg -R ../project-review new -m "Review fixes"
gg workspace list
```

`gg workspace` is needed in addition to `git worktree` because Git only tracks
the checkout, `HEAD`, and index. gg must also assign a working-change ref and
isolate undo and conflict-recovery state, and prevent a rewrite in one checkout
from silently moving another. A worktree created directly with `git worktree
add` is adopted automatically on its first revision-facing gg command.

Removing a checkout remains a Git operation: run `git worktree remove PATH`,
then `gg workspace forget NAME` if its gg workspace ref is still listed.

## Command reference

The following is the full implemented command surface shown by `gg --help`.

```text
gg status [FILESET...]
gg log [-r REVSET] [-n LIMIT] [--reversed] [--count] [FILESET...]
gg new [-m DESCRIPTION] [--no-edit] [PARENT...]
gg new [-m DESCRIPTION] [--no-edit] (--insert-after REV | --insert-before REV)
gg describe [-m DESCRIPTION | --stdin | --editor] [REV]
gg edit [REV | -r REV]
gg metaedit [REV...] [-m DESCRIPTION] [--author 'NAME <EMAIL>']
gg squash [-r REV | --from REV --into REV]
gg split [-r REV] [-m DESCRIPTION] FILESET...
gg abandon [--retain-bookmarks] [--restore-descendants] [REV]
gg rebase -s REV -d REV
gg commit [-m DESCRIPTION] [--editor] [FILESET...]
gg restore [--from REV] [--into REV] [FILESET...]
gg simplify-parents [-s REV]... [-r REV]...
gg file list [-r REV] [FILESET...]
gg file show [-r REV] FILESET...
gg file search [-r REV] -p PATTERN [--name-only | --line-number] [FILESET...]
gg file chmod [-r REV] (n|normal|x|executable) FILESET...
gg diff [-r REVSET | --from REV] [--to REV] [FILESET...]
gg show [--no-patch] [REV...]
gg bookmark advance [NAME...] [-t REV]
gg bookmark create NAME... [-r REV]
gg bookmark set NAME... [-r REV]
gg bookmark move [NAME...] [-f REV]... [-t REV] [-B]
gg bookmark delete NAME...
gg bookmark forget [--include-remotes] NAME...
gg bookmark rename [--overwrite-existing] OLD NEW
gg bookmark list [NAMES...] [--all-remotes | --remote REMOTE...] [-r REVISION...] [--sort KEY...]
gg tag set [--allow-move] NAME... [-r REV]
gg tag delete NAME...
gg tag list [NAME...] [-r REVISION...] [--sort KEY...]
gg init [DESTINATION]
gg clone URL [DESTINATION]
gg fetch [-b BRANCH...] [-t TAG...] [--remote REMOTE... | --all-remotes]
gg pull [GIT_ARGUMENT...]
gg push [-b BOOKMARK...] [-t TAG...] [-r REVSET...] [--all | --tracked | --deleted] [--remote REMOTE] [--dry-run]
gg undo
gg redo
gg operation log
gg operation restore [--what repo|remote-tracking] OPERATION
gg util completion (bash|elvish|fish|nushell|power-shell|zsh)
gg util exec -- COMMAND [ARG...]
gg util gc [--expire now]
gg util install-git-hooks
gg util install-man-pages PATH
gg util markdown-help
gg util snapshot
gg util check-push-conflicts
gg workspace add DESTINATION [--name NAME] [-r REVISION] [-m DESCRIPTION] [--sparse-patterns copy|full|empty]
gg workspace forget [NAME...]
gg workspace list
gg workspace rename NAME
gg workspace root [--name default]
gg next [--edit] [OFFSET]
gg prev [--edit] [OFFSET]
gg config get NAME
gg config list [--user|--repo|--workspace] [NAME]
gg config path (--user|--repo|--workspace)
gg config set (--user|--repo|--workspace) NAME VALUE
gg config unset (--user|--repo|--workspace) NAME
gg config edit (--user|--repo|--workspace)
```

Filesets support literal files/directories, `file:`, `root:`, `cwd:`, and
`glob:` selectors (or their function forms), with `|` union, `&`
intersection, and `~` difference. Revision selection supports graph ranges,
set operators, ancestor/descendant traversal, heads/roots, refs, IDs, metadata
patterns, conflict/empty predicates, and remote-bookmark predicates.

`gg config` is a thin wrapper over native Git configuration. Repository values
live in `.git/config`, workspace values use Git's per-worktree config, and user
values use the normal global Git config. Editor and diff/merge-tool behavior
uses standard keys such as `core.editor`, `difftool.<name>.*`, and
`mergetool.<name>.*`; gg creates no TOML configuration or private defaults.

`gg pull` is a direct wrapper over `git pull`; every trailing argument is
forwarded unchanged and Git's exit status is returned.

Push only sends existing bookmarks and tags, in one atomic `git push`. A
revision selector must resolve to an existing local ref; an unbookmarked change
is rejected instead of receiving a generated remote name.

Rewrites restack descendants and move affected local refs together. Conflicts
are recorded as local logical merge terms, so operations still succeed and
conflicted descendants can be rewritten again without nesting marker text.
Editing a conflicted change materializes its sides in the working tree. Resolve
the files normally; the next gg command snapshots the resolution. `gg push`
refuses any selection whose reachable history contains a conflict. Run
`gg util install-git-hooks` to install a managed `pre-push` hook that applies
the same check to native `git push`; an existing hook is preserved and chained.

Commit IDs and all retained aliases share one prefix namespace. Commands show
the shortest unique prefix with a minimum length of eight. Explicitly resolving
an alias refreshes its last-used time; unused aliases are collected after one
week by repository mutations and `gg util gc`. Alias collection is recorded in
the operation log, so it can be undone.

`gg undo` and `gg redo` behave like editor history: each restoration is itself
recorded, repeated commands move backward or forward, and a new operation after
an undo clears the redo path. `gg operation log` (also `gg op log`) shows the
newest-first operation graph with IDs, timestamps, and descriptions. `gg
operation restore` restores all state from a logged operation by default, or
only repository or remote-tracking state with repeated `--what` options.

## Library API

`libgg` exposes the gg workflow as a versioned C API in `<gg/gg.h>`. It borrows
an existing `git_repository*`, uses `git_oid` for object IDs, returns libgit2
`GIT_*` status codes, and reports details through `git_error_last()`. The caller
owns libgit2 initialization and the underlying repository handle.

```c
gg_repository *gg = NULL;
gg_new_options options = GG_NEW_OPTIONS_INIT;
gg_mutation_result result = {0};

options.message = "Add the parser";
if (gg_repository_attach(&gg, repository) == GIT_OK &&
    gg_repository_adopt_git_history(gg, NULL) == GIT_OK &&
    gg_repository_new_change(&result, gg, &options, NULL) == GIT_OK) {
  /* result.working_copy and result.references are structured GUI data. */
}
gg_mutation_result_dispose(&result);
gg_repository_free(gg);
```

Synchronization is explicit: call `gg_repository_adopt_git_history()` after
native Git history changes and `gg_repository_snapshot_working_copy()` after
filesystem or index changes. Queries do not modify repository state. Fetch and
push use plan/complete pairs so a GUI can perform transport itself and record gg
tracking state only after success. Long calls accept synchronous progress and
cancellation callbacks through `gg_operation_options`.

Install the project and consume it from CMake with
`find_package(gg CONFIG REQUIRED)` and `target_link_libraries(app PRIVATE
gg::gg)`.

## Project structure

The `gg_lib` target builds `libgg` and the public C API. The `gg_cli` target
builds the `gg` executable; CLI11, argument parsing, and text rendering stay in
this application layer. The C API accepts final messages and transport results,
so command-line editors, external diff tools, and arbitrary subprocesses are
not part of its interface.
Repository access, snapshots, revision lookup, rewrites, operation history,
and conflict state remain shared workflow implementation under `src/`.

## Build and test

Dependencies are pinned and downloaded by CPM.cmake: libgit2 provides repository
plumbing, CLI11 defines the command line, and GoogleTest provides the test
harness.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

To cross-build a standalone Windows executable with MinGW-w64:

```sh
cmake --preset windows-x64-static
cmake --build --preset windows-x64-static
```

The resulting `build/windows-x64-static/gg.exe` statically links libgit2 and
the GCC runtimes. HTTPS uses Windows Schannel, so no OpenSSL DLLs are needed.

The coverage build gates project source lines and reachable, non-exception
branches at 100%. Compiler-generated exception and unreachable cleanup edges
are excluded; the small number of source-line exclusions are marked inline
with `GG_COV_EXCL_BRANCH` so they remain reviewable.

```sh
cmake -S . -B build/coverage -G Ninja -DGG_COVERAGE=ON
cmake --build build/coverage --target coverage
```

## License

`gg` is licensed under the GNU General Public License version 2 only. See
[`LICENSE`](LICENSE).
