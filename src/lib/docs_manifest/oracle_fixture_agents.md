---
title: "Workspace AGENTS Guide — acme"
doc_kind: agents
template_version: 1
source_artifacts: []
source_decisions: []
source_plans: []
source_versions: {}
regenerated_at: "2026-08-31T09:16:23Z"
regenerated_by: planar-workspace-regenerate
references: {}
---

# Workspace AGENTS Guide — acme

**Workspace:** org:acme (id 1)
**Generated:** 2026-08-31T09:16:23Z
**Source:** /private/tmp/claude-501/-Users-mn-projects-github-rdrsss-planar/c4a11482-9bda-4166-a653-114e547d11c3/scratchpad/oracle/home/.planar/workspaces/1

This file is auto-generated. Do not edit directly; run
`planar workspace regenerate` to refresh.

## Projects in this workspace


| Slug | Path | Purpose | Capabilities |
|---|---|---|---|
| repo1-2 | /private/tmp/claude-501/-Users-mn-projects-github-rdrsss-planar/c4a11482-9bda-4166-a653-114e547d11c3/scratchpad/oracle/ws/repo1 |  |  |



## Cross-repo dependencies


_No cross-repo dependencies detected._


## Cross-repo plans in flight


_No active plans scoped to this workspace._


## Open cross-repo questions


_No open questions scoped to this workspace._


## Quick-reference CLI

- `planar tree --scope org:acme` — see all in-flight work
- `planar task list --scope org:acme` — cross-repo tasks
- `planar plan show <id>` — drill into a specific plan
- `planar workspace regenerate` — refresh this file after changes

## How to dispatch work

Cross-repo coordination tasks live in `org:acme`.
Repo-specific work belongs in the per-project scope. Pass
`--scope <slug>` explicitly when running write verbs from outside
the target repo; the strict resolver refuses to guess.
