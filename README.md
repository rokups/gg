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
- Each change has a stable change ID, even when editing it produces a new Git
  commit ID.
- `gg edit` can make any change the working copy. Further edits rewrite that
  change and automatically restack its descendants.
- `gg new`, `gg split`, `gg squash`, `gg rebase`, and `gg abandon` operate on
  the change graph directly.
- `gg undo` and `gg redo` provide editor-style history for repository
  operations.

Revisions use `@` for the working-copy change and `@-` for its parent. Commands
also accept stable change-ID prefixes, bookmarks, and Git object IDs.

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
copy its stable ID from `gg log`, run `gg edit CHANGE_ID`, edit the files, and
run another `gg` command to snapshot the result. Descendant changes and affected
local refs are updated together.

### Commit-oriented changes

`gg commit` provides a more familiar boundary while retaining automatic
snapshotting, stable change IDs, restacking, and undo:

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
change. Paths may be supplied to commit only part of the working change; the
remaining edits stay in the new working change:

```sh
gg commit -m "Add the parser" src/parser.cpp include/parser.hpp
```

### Using only `gg` commands

Lean mode is the default and hides only clone, init, status, diff, show, and
sparse commands. Tag, fetch, and push remain available alongside the complete
bookmark, file, util, and workspace families. Set `GG_LEAN=0` to expose the
remaining commands. Workspace commands remain available in lean mode because
they coordinate gg state that Git worktrees do not know about:

```sh
export GG_LEAN=0
gg clone URL project
cd project
gg new -m "Add the parser"
# edit files
gg bookmark create topic
gg push --bookmark topic
```

Help, generated manuals, and shell completions follow the selected mode.

## Storage model

Each working copy is represented by a commit under
`refs/gg/workspaces/<name>`. The primary checkout starts as `default`; linked
Git worktrees have their own names, operation histories, and paused-rewrite
state while sharing commits, change IDs, bookmarks, and tags. Stable change
IDs live under `refs/gg/changes/<id>`. Git `HEAD` stays at the working change's
parent so existing tooling continues to see normal working-tree changes.

Use `gg workspace add` when creating another checkout so the Git worktree and
its gg working change are created together:

```sh
gg workspace add ../project-review --name review -r main
gg -R ../project-review new -m "Review fixes"
gg workspace list
```

`gg workspace` is needed in addition to `git worktree` because Git only tracks
the checkout, `HEAD`, and index. gg must also assign a working-change ref and
stable ID, isolate undo and conflict-recovery state, and prevent a rewrite in
one checkout from silently moving another. A worktree created directly with
`git worktree add` is adopted automatically on its first revision-facing gg
command.

Removing a checkout remains a Git operation: run `git worktree remove PATH`,
then `gg workspace forget NAME` if its gg workspace ref is still listed.

## Command reference

The following is the full implemented command surface. Lean mode exposes the
smaller default surface through `gg --help`.

```text
gg status [PATH...]
gg log [-r REV] [-n LIMIT] [--reversed] [--count] [PATH...]
gg new [-m DESCRIPTION] [--no-edit] [PARENT...]
gg new [-m DESCRIPTION] [--no-edit] (--insert-after REV | --insert-before REV)
gg describe [-m DESCRIPTION | --stdin | --editor] [REV]
gg edit [REV | -r REV]
gg metaedit [REV...] [-m DESCRIPTION] [--author 'NAME <EMAIL>']
gg squash [-r REV | --from REV --into REV]
gg split [-r REV] [-m DESCRIPTION] PATH...
gg abandon [--retain-bookmarks] [--restore-descendants] [REV]
gg rebase -s REV -d REV
gg commit [-m DESCRIPTION] [--editor] [PATH...]
gg restore [--from REV] [--into REV] [PATH...]
gg simplify-parents [-s REV]... [-r REV]...
gg file list [-r REV] [PATH...]
gg file show [-r REV] PATH...
gg file search [-r REV] -p PATTERN [--name-only | --line-number] [PATH...]
gg file chmod [-r REV] (n|normal|x|executable) PATH...
gg diff [-r REV | --from REV] [--to REV] [PATH...]
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
gg push [-b BOOKMARK...] [-t TAG...] [-r REVISION...] [-c REVISION...] [--named NAME=REVISION...] [--all | --tracked | --deleted] [--remote REMOTE] [--dry-run]
gg undo
gg redo
gg operation log
gg operation restore [--what repo|remote-tracking] OPERATION
gg util completion (bash|elvish|fish|nushell|power-shell|zsh)
gg util config-schema
gg util exec -- COMMAND [ARG...]
gg util gc [--expire now]
gg util install-git-hooks
gg util install-man-pages PATH
gg util markdown-help
gg util snapshot
gg util check-push-conflicts
gg workspace add DESTINATION [--name NAME] [-r REVISION] [-m DESCRIPTION]
gg workspace forget [NAME...]
gg workspace list
gg workspace rename NAME
gg workspace root [--name default]
gg next [--edit|--no-edit] [OFFSET]
gg prev [--edit|--no-edit] [OFFSET]
gg config get NAME
gg config list [--user|--repo|--workspace] [NAME]
gg config path (--user|--repo|--workspace)
gg config set (--user|--repo|--workspace) NAME VALUE
gg config unset (--user|--repo|--workspace) NAME
gg config edit (--user|--repo|--workspace)
```

Rewrites restack descendants and move affected local refs together. Conflicts
are recorded as local logical merge terms, so operations still succeed and
conflicted descendants can be rewritten again without nesting marker text.
Editing a conflicted change materializes its sides in the working tree. Resolve
the files normally; the next gg command snapshots the resolution. `gg push`
refuses any selection whose reachable history contains a conflict. Run
`gg util install-git-hooks` to install a managed `pre-push` hook that applies
the same check to native `git push`; an existing hook is preserved and chained.

Change IDs use Jujutsu's 32-character reverse-hex format (`z` through `k`).
Commands show the shortest unique prefix with a minimum length of eight.

`gg undo` and `gg redo` behave like editor history: each restoration is itself
recorded, repeated commands move backward or forward, and a new operation after
an undo clears the redo path. `gg operation log` (also `gg op log`) shows the
newest-first operation graph with IDs, timestamps, and descriptions. `gg
operation restore` restores all state from a logged operation by default, or
only repository or remote-tracking state with repeated `--what` options.

## Project structure

The public runner and CLI dispatch are thin. Repository access, working-copy
snapshots, revision lookup, rewrites, operation history, conflict state, and
command families live in separate translation units under `src/`. Tests are
split by CLI validation, core workflows, rewrites, conflicts, remotes, and
internal state invariants, with one shared fixture in `tests/test_support.hpp`.

## Build and test

Dependencies are pinned and downloaded by CPM.cmake: libgit2 provides repository
plumbing, CLI11 defines the command line, and GoogleTest provides the test
harness.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
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
