---
title: Glossary
doc_kind: glossary
template_version: 1
source_artifacts: [artifact:79, artifact:80, artifact:81, artifact:82]
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
---

# Glossary

This page is aggregated from `kind=glossary_term` artifacts under
`assoc:project:planar`. One sub-section per term, alphabetical.

## Association

A many-to-many tag tying one or more repos (Planar `projects`) to a
logical project or org. An association is the unit of scope: every
plan, task, artifact, decision, scenario, and question is tagged to
exactly one association (or to `global`). The `kind` column
distinguishes the two common shapes — `project` for single-repo
associations (the default created by `planar init`) and `org` for
polyrepo workspaces. Cross-repo features rely on the org-kind
associations to span repo boundaries cleanly.

## Manifest (`.manifest-docs`)

The xxh3-keyed two-level Merkle index across `docs/`, stored at the
repo root as `.manifest-docs`. Each entry records the doc's
`doc_hash` (normalised content hash), a per-source `sources` map
(one xxh64 per source artifact/decision/plan body at synthesis
time), a `sources_hash`, and an `entry_hash`. A root hash rolls the
sorted entry-hash list up. `planar doc manifest verify` is the O(1)
root compare; `planar doc manifest diff` walks the four-signal
classifier (regenerate-candidate, hand-edit, new-authoring,
deletion); `planar doc manifest update` rebuilds and writes
atomically.

## Scope

A named addressing context for plans, tasks, artifacts, decisions,
questions, and scenarios. Scopes nest: `global` contains
`association` scopes, each of which contains the rows tagged to it.
Every Planar write verb resolves to exactly one scope before it
touches the database (see the
[scope-resolution feature page](features/scope-resolution.md)). The
active-scope stack — push/pop via `planar scope push/pop` — is the
read-side input the strict resolver consults when no `--scope`
flag is supplied.

## Workspace

A polyrepo working root — the directory that holds several member
projects (repos) as siblings. In Planar a workspace is an
`associations` row of `kind=org`; its member projects are the
`project_associations` rows for that org. The workspace owns a
per-workspace state directory at `~/.planar/workspaces/<org_id>/`
containing the canonical `AGENTS.md`, the routing-table JSON, and
per-workspace config; the workspace root itself holds symlinks
pointing at those canonical files. See the
[workspace-agents feature page](features/workspace-agents.md).
