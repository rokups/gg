# gg

`gg` is a small, JJ-shaped interface over ordinary Git repositories. Git
commits remain the storage format, local Git branches are exposed as
**bookmarks**, and remotes receive no custom objects, headers, notes, or refs.

The working copy is represented by an ordinary commit under
`refs/gg/workspaces/default`. Stable change IDs live under
`refs/gg/changes/<id>`. Git `HEAD` stays at the working change's parent so
existing Git-aware editors continue to see normal working-tree changes.

## MVP commands

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
gg tag list [NAME...]
gg init [DESTINATION]
gg clone URL [DESTINATION]
gg fetch [--remote REMOTE]
gg push --bookmark NAME [--remote REMOTE]
gg continue
gg abort
gg undo
gg redo
gg operation log
gg operation restore [--what repo|remote-tracking] OPERATION
gg util completion (bash|elvish|fish|nushell|power-shell|zsh)
gg util config-schema
gg util exec -- COMMAND [ARG...]
gg util gc [--expire now]
gg util install-man-pages PATH
gg util markdown-help
gg util snapshot
gg workspace list
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

Revisions use JJ names: `@`, `@-`, stable change-ID prefixes, bookmarks, or
ordinary Git object IDs. Rewrites restack descendants and move affected local
refs together. If libgit2 reports a merge conflict, the rewrite pauses before
moving its refs and writes conflict markers into the working tree. Resolve the
files and run `gg continue`, or run `gg abort` to restore the pre-rewrite
operation. First-class conflicted commits are intentionally outside this MVP.

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
split by CLI validation, ordinary workflows, rewrites, conflicts, remotes, and
internal state invariants, with one shared fixture in `tests/test_support.hpp`.

## Build and test

Dependencies are pinned and downloaded by CPM.cmake: libgit2 provides Git
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
