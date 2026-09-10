---
description: 'The cross-scope write cue and the Codex coordinate-role enforcement caveat — render-time content the pre-migration skillrender pipeline injected onto every source whose frontmatter carried `cross_scope_writes: true` (and, for Codex, `capability: coordinate`). Restored here as a linked companion doc after the M1+M2 migration dropped both frontmatter keys.'
kind: doc
slug: cross-scope-writes
---

# Cross-scope Writes

## Cross-scope write cue

Before invoking a mutation, compare its target with the cwd-derived scope from
`planar scope show --json`. If the target is outside that scope, emit this
standalone narrative line immediately before the command:

```text
[cross-scope write: <normalized-target-label>]
```

Normalize the cue independently from the command's target syntax. Use an
explicit `--scope` only when that verb supports it; otherwise preserve the
verb's supported plan, entity, workspace, positional, `--to`, or `--from`
target. Never add `--scope` to a verb whose schema lacks it.

For verbs that support `--scope`, use these exact label/argument mappings:

- Repo/project row with project slug `planar`: cue
  `[cross-scope write: project:planar]`; pass `--scope repo:planar`.
- Ordinary association with slug `org:acme`: cue
  `[cross-scope write: association:org:acme]`; pass `--scope assoc:org:acme`.
- Legacy project association with `kind=association`, slug `project:planar`,
  and `kind_label=project`: cue `[cross-scope write: project:planar]`; pass
  `--scope assoc:project:planar`. Never emit `association:project:planar`.
- Global target: cue `[cross-scope write: global]`; pass `--scope global`.

For commands without `--scope`, use these command-specific target rules:

- Workbench plan target: resolve the stored owner of `plan:<plan-id>`. For a
  plan owned by project `planar`, emit `[cross-scope write: project:planar]`
  and preserve the positional target, for example
  `planar workbench sync plan:<plan-id>`. Do not add `--scope`.
- Existing entity target: resolve the entity's stored owner, emit its normalized
  scope cue, and preserve the supported `<kind:id>`, plan, event, or other
  positional target. Do not add `--scope`.
- Single workspace target: translate skill input `--workspace org:work` to the
  CLI's positional target, emit `[cross-scope write: association:org:work]`,
  and run, for example, `planar workspace routing build org:work` or
  `planar workspace regenerate org:work`. Do not add `--scope`.
- All-workspaces doctor: enumerate registered org workspaces first. Immediately
  before the single `planar workspace doctor --json` command, emit one normalized
  cue for each workspace outside the cwd-derived scope, for example
  `[cross-scope write: association:org:work]`, sorted by normalized label.
  Emit no cue for same-scope workspaces; doctor takes no target or `--scope`
  argument.
- `planar promote`/`demote`: derive the cue from the destination. For
  `--to org:acme`, emit `[cross-scope write: association:org:acme]`; preserve
  the supported `--to` or `--from`/global-demotion form and do not add `--scope`.

The cue is visibility, not authorization: it does not replace confirmation,
relax scope guards, or bypass the cross-scope guard — no such CLI flag exists.
Same-scope writes MUST NOT emit any cross-scope cue.

## Codex enforcement caveat

When running under Codex, and the linking agent's role is `coordinate`: this
role runs CLI commands and spawns subagents but must NOT edit source files.
Codex's `sandbox_mode` is a coarse filesystem-write switch and cannot express
that boundary precisely; `workspace-write` is set so legitimate
`planar`/`planar-agent` DB writes succeed. Do not edit repository source
files from this agent — that boundary is doctrinal here, not structurally
enforced.
