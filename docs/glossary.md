---
title: Glossary
doc_kind: glossary
template_version: 1
regenerated_at: 2026-05-18T00:00:00Z
regenerated_by: hand
---

# Glossary

This page is aggregated from `kind=glossary_term` artifacts under
`assoc:project:planar`. One sub-section per term, alphabetical.

## Association

A many-to-many tag tying one or more repos (Planar `projects`) to a
logical project or org. An association is one of the three scope
kinds: every plan, task, artifact, decision, scenario, and question is
tagged to exactly one repo, one association, or `global`. The `kind`
column distinguishes the two common shapes — `project` for a cross-repo
product or initiative and `org` for polyrepo workspaces. `planar init`
registers a project but does not create an association; use
`planar assoc create` and `planar assoc add`. Cross-repo features rely on the org-kind
associations to span repo boundaries cleanly.

## Scope

A named addressing context for plans, tasks, artifacts, decisions,
questions, and scenarios. There are three kinds: `repo:<slug>`,
`assoc:<slug>`, and `global`.
Every Planar write verb resolves to exactly one scope before it
touches the database (see the
[scope-resolution feature page](features/scope-resolution.md)). The
current working directory is the input both resolvers consult when no
`--scope` flag is supplied; the active-scope stack was removed
(migration 00009) and there is no ambient scope state.

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
