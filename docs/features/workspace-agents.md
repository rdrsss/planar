---
title: Workspace AGENTS.md generation
doc_kind: feature
template_version: 1
source_artifacts: [artifact:76, artifact:77, artifact:78]
source_plans: [plan:135]
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
references:
  workspace_product_spec:
    kind: planar
    entity: artifact:76
  workspace_tech_spec:
    kind: planar
    entity: artifact:77
  workspace_roadmap:
    kind: planar
    entity: artifact:78
---

# Workspace AGENTS.md generation

A *workspace* in Planar is the polyrepo root where you hold several
member projects side by side — typically the directory that contains
`web-app/`, `platform/`, `infra/`, and friends as siblings. Planar
generates a single `AGENTS.md` (with a sibling `CLAUDE.md` symlink)
that gives any agent dropped into that root a consistent routing
table across every member project[^workspace_product_spec].

## What it does

- Identifies the workspace from an `associations` row of
  `kind=org`. The workspace's member projects are the
  `project_associations` rows for that org.
- Owns a per-workspace state directory at
  `~/.planar/workspaces/<org_id>/`. The canonical AGENTS.md and the
  routing-table JSON live there; the repo workspace root holds only
  symlinks pointing back at them[^workspace_tech_spec].
- Builds a routing table from static signals — file presence, README
  excerpts, dependency declarations — without invoking an LLM. The
  static pass is deterministic and gates the optional enrichment
  pass.
- Regenerates AGENTS.md by rendering the workspace doc-kind template
  against the routing table plus live database queries. The
  manifest layer from the outward-docs system tracks
  drift[^workspace_roadmap].
- Maintains symlink lifecycle as a first-class concern: atomic
  creation, repair when the link points at a stale path or has been
  flipped to a regular file, and clean removal when the workspace
  is torn down.
- Composes with the strict scope resolver. `planar workspace init`
  scaffolds the org association and writes the first routing
  table; subsequent `planar scope` operations Just Work because the
  org's member projects are already linked.

## The workspace state directory

```text
~/.planar/workspaces/<org_id>/
├── AGENTS.md                     # canonical generated file
├── routing-table.json            # static-signal + enrichment output
├── routing-table-overrides.json  # operator overrides (hand-edited)
└── config.toml                   # per-workspace settings
```

The repo workspace root holds two symlinks:

```text
<workspace-root>/
├── AGENTS.md  -> ~/.planar/workspaces/<id>/AGENTS.md
└── CLAUDE.md  -> ~/.planar/workspaces/<id>/AGENTS.md
```

In degraded mode (filesystems without symlink support — exotic but
real), the regenerator falls back to copies plus a sentinel comment
that marks the file as canonical. `planar workspace doctor` detects
either form and reports back.

## CLI

```sh
# One-time scaffold: register the workspace association,
# materialise the state directory, place the symlinks.
planar workspace init --name platform --slug platform

# Refresh the routing table from current static signals.
planar workspace scan

# Re-render AGENTS.md from the current routing table and DB.
planar workspace regenerate

# Inspect symlink lifecycle health.
planar workspace doctor
```

## Routing table builder

The builder lives in `src/engine/workspace/` and emits a canonical
JSON shape: each member project gets a record with its slug, its
filesystem path inside the workspace, the static signals the builder
detected (e.g. `package.json` ⇒ Node, `Cargo.toml` ⇒ Rust,
`build.zig` ⇒ Zig), and a short capability summary derived from
README headings.
The optional LLM enrichment pass — wired via the `pl-workspace-scan`
vendor skill — fills in human-readable routing hints that the
static scan cannot infer (e.g. "this is the auth surface").

## Operator overrides

Anything an operator wants to assert that the builder cannot
discover lives in `routing-table-overrides.json` next to the
canonical routing table. The regenerator merges overrides on top of
the builder output before rendering AGENTS.md, so hand edits to the
generated file get overwritten while structured overrides survive.

## Hand-edit policy

The generated `AGENTS.md` is *not* a doc you edit. Operator changes
belong in `routing-table-overrides.json`; doc-prompt template changes
belong in `~/.planar/templates/doc-prompts/agents.md`. The
docs-system manifest classifies any drift between the on-disk
AGENTS.md and the regenerator's output as a hand-edit signal, and
`planar workspace doctor` flags it explicitly.

## Related

- [Concepts: workspace and association](../concepts.md)
- [Workflows: scaffolding a new workspace](../workflows.md)
- [Features: scope resolution](scope-resolution.md) — workspace init
  feeds the org association the scope resolver later reads.
