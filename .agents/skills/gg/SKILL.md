---
name: gg
description: Use gg for version-control work in gg-managed Git repositories, including inspecting changes, creating task changes, editing history, resolving conflicts, managing branches, and synchronizing remotes.
---

# Use gg

Check that `gg` is available. If it is missing, report that limitation; do not silently substitute Git history commands.

Use one named change per task:

1. Inspect with `gg status` (it shows the checked-out branch or a detached `HEAD`).
2. Start with `gg new -m "<task>"` (advances the checked-out branch) or `gg new -d -m "<task>"` (leaves branches where they are).
3. Edit files and run the relevant tests.
4. Record the edits with `gg squash` (amends `@`) or `gg commit -m "<message>"` (new child of `@`).
5. Verify with `gg diff` (working tree vs `@`), `gg show` (`@`'s own change), and `gg status`.

`@` is Git's `HEAD` commit and `@-` is its parent. Working-tree edits stay uncommitted until `gg commit` or `gg squash`; nothing is snapshotted automatically. `gg edit <branch>` checks a branch out, while `gg edit <commit-id>` detaches. `gg log` loads 256 revisions by default; use `--limit` or explicit `--all` for more. Rewrites retain old commit IDs as aliases, restack descendants, and move affected local refs.

When conflicts appear, inspect them with `gg status`, resolve the materialized files normally, and amend the resolution with `gg squash`. Do not push conflicted history.

Require explicit user intent before creating commits, creating or moving branches, rewriting existing changes, or pushing. Inspect the relevant diff and status before high-impact operations.

Use layered help as needed: `gg --doc` for the working model, `gg <command> --doc` for command semantics, and `gg util markdown-help` for the complete reference.
