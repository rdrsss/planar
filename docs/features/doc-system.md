---
title: Outward-facing documentation system
doc_kind: feature
template_version: 2
regenerated_at: 2026-05-29T00:00:00Z
regenerated_by: hand
---

# Outward-facing documentation system

Planar delegates published-documentation state to the standalone Tabularium
tool. Tabularium links docs under `docs/` to source-area xxh64 / merkle hashes
over the working tree in its machine-local SQLite database, so documentation
drift is detectable without committing bookkeeping into this repository. An
orchestrator-launched documenter can consume the diff at the end of work cycles
and emit a worklist for the operator to gate.

## What it does

- Maintains a machine-local manifest — an xxh64-keyed, merkle-rooted index
  whose entries link each published doc to one or more **repo-path
  sources** (directories or files in the working tree). The root hash
  is O(1) to verify against the live tree.
- Walks the repo with a stable exclusion policy: `.git/`, dotfile
  ancestors, and `.gitignore`-matched paths are skipped. Everything
  else under the tree contributes to the merkle root.
- Computes a three-signal classifier (`regenerate-candidate`,
  `hand-edit`, `new-authoring` / `deletion`) by comparing the stored
  per-entry hashes against the recomputed tree.
- Tracks a `nodoc` set: paths the documenter inspected and decided
  are not worth documenting. The set is re-evaluated whenever a
  nodoc path's hash changes.
- Is owned by the separately installed `tabularium` executable, with a small
  capability surface: build / verify / diff / cover / nodoc / lint / schema /
  import / export. Its writes stay under `$TABULARIUM_HOME` (default
  `~/.tabularium`); the documented repository is read-only except when the
  caller explicitly authors prose.

## Manifest shape

```json
{
  "version": 2,
  "algo": "xxh64",
  "root": "<16-hex merkle root>",
  "entries": {
    "docs/architecture.md": {
      "doc_hash": "<xxh64 of doc body>",
      "sources": {
        "src/db/": "<merkle hash of subtree>",
        "migrations/": "<merkle hash of subtree>"
      },
      "sources_hash": "<xxh64 of sorted sources map>",
      "entry_hash": "<xxh64 of doc_hash + sources_hash>"
    }
  },
  "nodoc": {
    "vendor/sqlite/": "<merkle hash captured when documenter decided 'no doc needed'>"
  }
}
```

Source scope (what the root hash covers):

- **Include:** `src/`, `migrations/`, `templates/`, `vendor/`,
  `build.zig`, `build.zig.zon`, any non-dotfile top-level file, and
  `docs/*` (for the `doc_hash` halves of entries).
- **Exclude:** `.git/`, all dotfile ancestors, anything matched by a
  `.gitignore` rule.

## The three-signal classifier

`tabularium diff` walks the merkle, detects changed subtrees, and emits
one of three signals per changed path:

| Signal | Meaning |
|--------|---------|
| `regenerate-candidate` | a source the doc covers drifted; doc body may need to be refreshed |
| `hand-edit` | doc body changed without its sources moving |
| `new-authoring` / `deletion` | a path appeared without a covering entry, or an entry's path is gone |

The classifier is pure. The documenter agent acts on the worklist; the
operator gates every action. There is no "auto-regenerate" path — the
binary itself only reads and reports.

## CLI surface

```sh
# Build / refresh the manifest (recompute hashes, write atomically).
tabularium build

# O(1) root compare against the live tree.
tabularium verify

# Three-signal drift breakdown.
tabularium diff [--json]

# Add or remove a (doc, source) coverage edge.
tabularium cover <doc-path> <repo-path> [--remove]

# Mark a path as intentionally undocumented (or remove from nodoc).
tabularium nodoc <repo-path> [--remove]

# Minimal docs prose linter (URL footnotes etc.; DB-free).
tabularium lint [--path <dir>]
```

## Orchestrator integration

At the end of a work cycle the orchestrator launches the documenter
agent with the prior manifest, the current repo merkle, and the set of
changed subtrees. The documenter resolves changed subtrees to affected
docs via the manifest, walks uncovered changes through the three-outcome
decision (extend an existing doc's sources, create a new doc, or add to
`nodoc`), and emits a worklist the operator gates before any doc verb
runs.

## Where docs live vs where authoring lives

Internal planning artifacts (tech specs, roadmaps, product specs,
ADRs, design notes) live as Planar artifacts under the active scope
and surface via the workbench filesystem. Published outward-facing
docs live under `docs/` in the repo. The doc system is the boundary:
the documenter agent proposes; the operator authors; `tabularium`
tracks the manifest contract.

## Related

- [Concepts: artifacts and the workbench](../concepts.md)
- [Workflows: synthesising and refreshing docs](../workflows.md)
- [Architecture: the four-binary boundary](../architecture.md)
- [Features: scope resolution](scope-resolution.md)
