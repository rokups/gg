# gg

`gg` brings Jujutsu-inspired history editing to ordinary Git repositories
while keeping Git's everyday model: `@` is `HEAD`, the current branch is the
branch `HEAD` is attached to, and edits in the working tree are uncommitted
changes to `@`. Old changes can be edited directly, descendants are restacked
automatically, and repository changes can be undone.

`gg` does not require a new repository format or server support. Changes are Git
commits, branches are native local Git branches, and remotes receive no custom
objects, headers, notes, or refs. Git and gg can be used side by side: gg
follows whatever `HEAD`, branches, and remote-tracking refs Git leaves behind.

## What changes compared with Git

- There is no staging step. `gg commit` records working-tree changes (or only
  selected filesets) as a new child of `@` and advances the current branch;
  `gg squash` amends them into `@`, like `git commit --amend`.
- `gg new` creates an empty change on `@` and advances the current branch to it.
  `gg new -d` (`--detach`) creates it without moving any branch; `HEAD` is then
  detached and the change is an unnamed head. Name it with `gg branch create` or
  leave it unnamed; unnamed heads you create stay visible until abandoned.
- `gg edit BRANCH` checks a branch out; `gg edit COMMIT_ID` detaches, and any
  commit can be edited in place. Rewrites keep the branch checked out and
  restack descendants.
- Each change is identified by its current Git commit ID. When a rewrite
  changes that ID, its previous commit IDs remain usable as aliases.
- `gg split`, `gg squash`, `gg rebase`, and `gg abandon` operate on the change
  graph directly, and `gg undo`/`gg redo` provide editor-style history for
  repository operations.

Revisions use `@` for `HEAD`'s commit and `@-` for its parent. Commands also
accept current or retained historical commit-ID prefixes, branches, and Git
object IDs.

## Workflows

### Git-like commits

```sh
git clone URL project
cd project

gg edit main              # check out a branch (HEAD attached)
# edit files
gg status                 # working-tree changes relative to @
gg commit -m "Add the parser"          # new commit; main advances
# edit more files
gg commit -m "Add parser tests" src/parser_test.cpp   # only selected files
gg push                   # pushes the checked-out branch
```

Filesets may be supplied to commit only part of the working tree; unselected
edits stay uncommitted:

```sh
gg commit -m "Commit sources except generated files" \
  "glob('src/*') ~ root('src/generated')"
```

### Changes first, then content

Start a described change, edit, and amend the edits into it:

```sh
gg new -m "Add the parser"        # empty change on main; main advances
# edit files
gg squash                         # amend working-tree edits into @
gg new -d -m "Try another parser" # experiment without moving main
# edit files
gg squash
gg branch create parser-experiment    # optionally name the detached head
```

Descriptions are editable metadata. Use `gg describe -m "New description"` at
any time. To revise an earlier change, run `gg edit COMMIT_ID`, edit the files,
and `gg squash`. Descendant changes and affected local refs are updated
together; the copied ID remains an alias for the rewritten commit.

### Using only `gg` commands

All implemented commands are available by default, including the complete
branch, file, util, and workspace families:

```sh
gg clone URL project
cd project
gg branch create topic -r main
gg edit topic
# edit files
gg commit -m "Add the parser"
gg push
```

Every command and command group prints its complete help with `--doc`, without
requiring a repository. Generated manuals and shell completions use the same
command schema.

## Agent use

The portable [`gg` skill](.agents/skills/gg/SKILL.md) teaches coding agents the
task-per-change workflow and its safety boundaries. To install it, copy the
`.agents/skills/gg` directory into the skill directory used by your agent; for
example, when that location is exposed as `AGENT_SKILLS_DIR`:

```sh
cp -R .agents/skills/gg "$AGENT_SKILLS_DIR/"
```

Help is layered to keep agent context small: use `gg --doc` for the working
model, `gg COMMAND --doc` (or `gg --doc COMMAND`) for one command, and `gg util
markdown-help` for the complete schema-derived reference.

## Storage model

Each working copy records `@` (always Git's `HEAD` commit) under
`refs/gg/workspaces/<name>`. The primary checkout starts as `default`; linked
Git worktrees have their own names, working changes, and operation histories
while sharing commits, commit aliases, branches, and tags. Historical commit
IDs are stored together under `refs/gg/commit-aliases` so repositories do not
expose one Git reference for every alias. Unnamed heads created with
`gg new -d`, detached commits, or duplication are marked under
`refs/gg/visible-heads/`; heads that only aliases or Git internals reference are
not shown. Git `HEAD` is `@`, attached to a branch or detached exactly as Git
would have it, so existing tooling continues to see normal working-tree
changes.

Use `gg workspace add` when creating another checkout so the Git worktree and
its gg working change are created together. Worktrees created with native Git
are also listed as unmanaged navigation targets and are adopted automatically
when opened by gg:

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

`gg workspace rename NEW --workspace OLD` renames any managed workspace.
`gg workspace remove NAME` refuses uncommitted changes, then safely removes a
linked worktree, or prunes stale worktree administration and gg
metadata. It refuses files that cannot be preserved, locked worktrees, the
primary checkout, and the checkout running the command. Operation history keeps
the final working change for recovery, but deletion of the directory itself is
not undoable.

`gg workspace move NAME DESTINATION` moves a linked checkout using Git while
preserving its name, working change, and independent history. Run it from
another checkout and choose a destination that does not exist. Git's restrictions
on moving locked worktrees, primary checkouts, and worktrees containing submodules
also apply. Local files move with the checkout.

`gg workspace lock NAME [--reason TEXT]` and `gg workspace unlock NAME` use native
Git locks. A lock protects even a missing checkout, such as one on an unmounted
drive, from removal and pruning. `gg workspace list` includes lock reasons.

After moving a linked checkout outside gg, run `gg workspace repair NEW_PATH`
from a checkout Git can still open. After moving the primary checkout, run
`gg workspace repair` there to reconnect its linked worktrees. These commands
also support linked checkouts attached to a bare common repository.

`gg workspace prune [--dry-run] [--expire DATE]` runs native Git pruning, then
cleans gg refs and per-worktree state for worktrees no longer registered. The
expiration defaults to `now`; native locks are respected. Repair moved checkouts
before pruning. Move, lock, unlock, and repair are physical Git lifecycle changes
that `gg undo` does not reverse. Undo can restore pruned gg refs, but cannot
recreate Git worktree administration.

The C API exposes the same lifecycle through `gg_repository_workspace_*`.
`gg_repository_workspace_lock_info` returns an owned lock reason; release it
with `gg_workspace_lock_info_dispose`. The existing workspace array ABI is
unchanged.

## Command reference

The following is the full implemented command surface shown by `gg --help`.

```text
gg status [FILESET...]
gg log [-r REVSET] [-n LIMIT | --all] [--reversed] [--count] [FILESET...]
gg new [-m DESCRIPTION] [-d | --no-edit] [PARENT...]
gg new [-m DESCRIPTION] [--no-edit] (--insert-after REV | --insert-before REV)
gg describe [-m DESCRIPTION | --stdin | --editor] [REV]
gg edit [REV | -r REV]
gg metaedit [REV...] [-m DESCRIPTION] [--author 'NAME <EMAIL>']
gg squash [-m DESCRIPTION] [-i] [FILESET...]
gg squash [-r REV | --from REV --into REV]
gg split [-r REV] [-m DESCRIPTION] FILESET...
gg abandon [--retain-branches] [--restore-descendants] [REV]
gg rebase -s REV -d REV
gg commit [-m DESCRIPTION] [--editor] [-i] [FILESET...]
gg restore [--from REV] [--into REV | -c REV] [FILESET...]
gg simplify-parents [-s REV]... [-r REV]...
gg file list [-r REV] [FILESET...]
gg file show [-r REV] FILESET...
gg file search [-r REV] -p PATTERN [--name-only | --line-number] [FILESET...]
gg file chmod [-r REV] (n|normal|x|executable) FILESET...
gg diff [-r REVSET | --from REV] [--to REV] [FILESET...]
gg show [--no-patch] [REV...]
gg branch create NAME... [-r REV]
gg branch set NAME... [-r REV]
gg branch move [NAME...] [-f REV]... [-t REV] [-B]
gg branch delete NAME...
gg branch forget [--include-remotes] NAME...
gg branch rename [--overwrite-existing] OLD NEW
gg branch list [NAMES...] [--all-remotes | --remote REMOTE...] [-r REVISION...] [--sort KEY...]
gg tag set [--allow-move] NAME... [-r REV]
gg tag delete NAME...
gg tag list [NAME...] [-r REVISION...] [--sort KEY...]
gg init [DESTINATION]
gg clone URL [DESTINATION]
gg fetch [-b BRANCH...] [-t TAG...] [--remote REMOTE... | --all-remotes]
gg pull [GIT_ARGUMENT...]
gg push [-b BRANCH...] [-t TAG...] [-r REVSET...] [--all | --tracked | --deleted] [--remote REMOTE] [--dry-run]
gg undo
gg redo
gg operation log
gg operation restore [--what repo|remote-tracking] OPERATION
gg util completion (bash|elvish|fish|nushell|power-shell|zsh)
gg util exec -- COMMAND [ARG...]
gg util gc [--expire now]
gg util optimize
gg util install-git-hooks
gg util install-man-pages PATH
gg util markdown-help
gg util check-push-conflicts
gg workspace add DESTINATION [--name NAME] [-r REVISION] [-m DESCRIPTION] [--sparse-patterns copy|full|empty]
gg workspace forget [NAME...]
gg workspace list
gg workspace rename NEW [--workspace OLD]
gg workspace remove NAME
gg workspace move NAME DESTINATION
gg workspace lock NAME [--reason TEXT]
gg workspace unlock NAME
gg workspace prune [--dry-run] [--expire DATE]
gg workspace repair [PATH...]
gg workspace root [--name default]
gg next [OFFSET]
gg prev [OFFSET]
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
patterns, conflict/empty predicates, and remote-branch predicates.

`gg config` is a thin wrapper over native Git configuration. Repository values
live in `.git/config`, workspace values use Git's per-worktree config, and user
values use the normal global Git config. Editor and diff/merge-tool behavior
uses standard keys such as `core.editor`, `difftool.<name>.*`, and
`mergetool.<name>.*`; gg creates no TOML configuration or private defaults.

`gg pull` is a direct wrapper over `git pull`; every trailing argument is
forwarded unchanged and Git's exit status is returned. After `gg pull`, or on
the next `gg` command after a native Git fetch or pull, fetched tracked
branches fast-forward local branches that are their ancestors. Branches checked
out in a workspace are never moved behind its back; `gg fetch` fast-forwards the
current branch together with the working tree when it has no uncommitted
changes. Diverged local branches are left untouched.

With no selectors, `gg push` pushes the checked-out branch in one atomic
`git push`; a detached `HEAD` requires `--branch`. Explicit selection modes retain their selected-ref behavior; a
revision selector must resolve to an existing local branch or tag.

Rewrites restack descendants and move affected local refs together. Conflicts
are recorded as local logical merge terms, so operations still succeed and
conflicted descendants can be rewritten again without nesting marker text.
Editing a conflicted change materializes its sides in the working tree. Resolve
the files normally; standard or gg conflict markers keep the file conflicted,
and graph rewrites preserve that state until the markers are removed. Amend the
resolution into the change with `gg squash`. `gg push`
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
native Git history changes; `@` then follows Git's `HEAD`. Queries do not modify
repository state. Fetch and push use plan/complete pairs so a GUI can perform
transport itself and record gg tracking state only after success. Long calls
accept synchronous progress and cancellation callbacks through
`gg_operation_options`.

`gg_repository_head()` reports whether `HEAD` is attached and to which branch;
`gg_repository_named_refs()` marks the current branch and branches checked out
in other workspaces. `gg_repository_new_change()` continues the current branch
(or a branch named as the only parent) unless `gg_new_options.detach` is set.

`gg_repository_worktree_status()` compares disk contents with `@`.
`gg_repository_commit()` records eligible disk changes (optionally selected
filesets) in one new child and advances the current branch;
`gg_repository_amend()` amends them into `@` and restacks its descendants. The
amend revision, when supplied, must still identify `@`. These operations use the
filesystem as the source of file contents, including when the Git index holds
staged data, and apply the rules for ignored files, sparse checkouts, large new
files, and conflict metadata.

`gg_repository_edit()` checks out a revision with Git semantics (a branch name
attaches `HEAD`) and refuses a switch that would overwrite uncommitted changes.
For selected file or hunk transfers, construct a Git tree and pass its OID to
`gg_repository_amend_tree()`, which rewrites `@` and descendants while leaving
the filesystem and index untouched. A native Git commit that moves `HEAD`
outside gg is rejected by these calls until `gg_repository_adopt_git_history()`
adopts it.

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
The C API's `gg_repository_lookup_revisions()` hydrates an explicitly bounded
OID batch in input order—including parents, aliases, conflict state, and
emptiness—without walking repository history.

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
cmake --preset mingw-x64-static
cmake --build --preset mingw-x64-static
```

The resulting `build/mingw-x64-static/gg.exe` statically links libgit2 and
the GCC runtimes. HTTPS uses Windows Schannel, so no OpenSSL DLLs are needed.

For a native Visual Studio 2022 x64 release build:

```sh
cmake --preset msvc-x64
cmake --build --preset msvc-x64-debug
cmake --build --preset msvc-x64-release
```

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
