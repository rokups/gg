---
name: gg
description: Use gg for version-control work in gg-managed Git repositories, including inspecting changes, creating task changes, editing history, resolving conflicts, managing bookmarks, and synchronizing remotes.
---

# Use gg

Check that `gg` is available. If it is missing, report that limitation; do not silently substitute Git history commands.

Use one named change per task:

1. Inspect with `gg status`.
2. Start with `gg new -m "<task>"`.
3. Edit files and run the relevant tests.
4. Verify with `gg diff` and `gg status`.
5. Leave the named change as the working-copy change at `@`.

Expect gg to snapshot tracked working-copy files automatically when revision-facing commands run; no staging or final commit step is required. Treat `@` as the working-copy change and `@-` as its parent. Rewrites retain old commit IDs as aliases, restack descendants, and move affected local refs.

When conflicts appear, inspect them with `gg status`, resolve the materialized files normally, and run another gg command to snapshot the resolution. Do not push conflicted history.

Require explicit user intent before creating commits, creating or moving bookmarks, rewriting existing changes, or pushing. Inspect the relevant diff and status before high-impact operations.

Use layered help as needed: `gg --doc` for the working model, `gg <command> --doc` for command semantics, and `gg util markdown-help` for the complete reference.
