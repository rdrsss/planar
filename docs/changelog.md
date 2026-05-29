---
title: Changelog
doc_kind: changelog
template_version: 1
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
---

# Changelog

This page is aggregated from `kind=changelog_entry` artifacts under
`assoc:project:planar`, newest first. Each entry corresponds to a
shipped milestone or plan.

## 2026-05-18

### Plan 96 M4–M6: promote, regenerate, and query verbs

The synthesis pipeline landed across three milestones. M4 added
`planar doc promote` (synthesise a new doc from named source
entities; the LLM call lives in the `pl-doc-promote` vendor skill).
M5 added `planar doc regenerate` (re-synthesise an existing doc
when sources have drifted; hand-edit policy with `--force` /
`--merge`). M6 added the coverage queries: `planar doc backlinks`
(every doc citing an entity), `planar doc orphans` (artifacts of a
kind with zero backlinks), and `planar doc coverage` (done plans
with no published doc).

### Plan 135: workspace AGENTS.md generation

Plan 135 shipped the workspace AGENTS.md generation pipeline. A
workspace is an `associations` row of `kind=org`. Each workspace
owns a state directory at `~/.planar/workspaces/<id>/` holding the
canonical `AGENTS.md`, the routing-table JSON, and per-workspace
config. The repo workspace root holds symlinks back to those
canonical files (with a degraded copy-mode fallback).
`planar workspace init` scaffolds the org association and writes
the first routing table; `planar workspace scan` refreshes static
signals; `planar workspace regenerate` re-renders AGENTS.md;
`planar workspace doctor` inspects symlink lifecycle health.

### Plan 88: strict scope resolution shipped

The strict scope resolver landed on 2026-05-18 (plan 88, M1–M7).
Every write verb now resolves to exactly one scope via a four-step
algorithm: `--scope` explicit override → cwd derivation →
stack-top fallback → strict tiebreak (refuse when cwd and stack
disagree). The `--no-scope-check` escape hatch is available for
legacy scripts but is excluded from routine workflow examples.
New: `planar health` surfaces cwd-vs-stack disagreement;
cwd-not-on-top mismatches print a non-blocking warning
(suppressible with `--quiet`).

### Plan 96 M1–M2: doc kinds and manifest layer landed

The outward-facing docs system's foundation shipped in two
milestones. M1 (migration 0008) extended `artifacts.kind` with
`research`, `getting_started`, `changelog_entry`, and
`glossary_term`. M2 added the `.manifest-docs` Merkle index
(xxh3-keyed, two-level), the citation linter (`planar doc lint`)
covering GFM footnote citations plus a structured `references:`
front-matter block, the URL HEAD validator with on-disk cache and
30-day re-check policy, and the per-entry planar-entity resolver.
Pre-commit hook ships under `scripts/git-hooks/`.
