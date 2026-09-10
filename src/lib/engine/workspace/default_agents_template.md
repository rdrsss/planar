---
title: "Workspace AGENTS Guide — {{.WorkspaceName}}"
doc_kind: agents
template_version: 1
source_artifacts: []
source_decisions: []
source_plans: []
source_versions: {}
regenerated_at: "{{.GeneratedAt}}"
regenerated_by: planar-workspace-regenerate
references: {}
---

# Workspace AGENTS Guide — {{.WorkspaceName}}

**Workspace:** org:{{.WorkspaceSlug}} (id {{.WorkspaceID}})
**Generated:** {{.GeneratedAt}}
**Source:** {{.SourcePath}}

This file is auto-generated. Do not edit directly; run
`planar workspace regenerate` to refresh.

## Projects in this workspace

{{if .Projects -}}
| Slug | Path | Purpose | Capabilities |
|---|---|---|---|
{{range .Projects}}| {{.Slug}} | {{.RootPath}} | {{.Summary}} | {{commaJoin .Capabilities}} |
{{end}}
{{- else}}
_No projects registered. Run `planar assoc add ...` to add members._
{{end}}

## Cross-repo dependencies

{{if .DependencyEdges -}}
{{range .DependencyEdges}}- {{.From}} → {{.To}} ({{.Reason}})
{{end}}
{{- else}}
_No cross-repo dependencies detected._
{{end}}

## Cross-repo plans in flight

{{if .ActivePlans -}}
{{range .ActivePlans}}- plan {{.ID}}: {{.Title}}
{{end}}
{{- else}}
_No active plans scoped to this workspace._
{{end}}

## Open cross-repo questions

{{if .OpenQuestions -}}
{{range .OpenQuestions}}- q{{.ID}}: {{.Title}}
{{end}}
{{- else}}
_No open questions scoped to this workspace._
{{end}}

## Quick-reference CLI

- `planar tree --scope org:{{.WorkspaceSlug}}` — see all in-flight work
- `planar task list --scope org:{{.WorkspaceSlug}}` — cross-repo tasks
- `planar plan show <id>` — drill into a specific plan
- `planar workspace regenerate` — refresh this file after changes

## How to dispatch work

Cross-repo coordination tasks live in `org:{{.WorkspaceSlug}}`.
Repo-specific work belongs in the per-project scope. Pass
`--scope <slug>` explicitly when running write verbs from outside
the target repo; the strict resolver refuses to guess.
