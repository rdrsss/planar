---
description: Route operator intent to available Planar workflows or show command help.
origin: docs/cli-reference.md#domain-help
shared_notes:
    - Active scope and database state come from the CLI; the skill must not read or write workspace context outside it.
slug: pl-help
vendor:
    claude:
        argument_hint: '[<intent-or-subcommand>]'
        invocation_examples: |
            /pl-help
            /pl-help resume interrupted work
            /pl-help inspect active agents
            /pl-help record a technical decision
            /pl-help repair my local skills
            /pl-help task
            /pl-help sync resolve
---

# Planar Help ({{.VendorTitle}})

Routes an operator's intent to an available Planar workflow, or shows the
command reference for a named CLI verb.

## What It Does

For an intent, selects one workflow, gives a one-line reason, and includes an
executable example. For a CLI subcommand, shows that command's full usage
including flags, output shape, and exit codes. It always names the underlying
CLI verbs so the intent-oriented entry point does not obscure the supported
interface.

## CLI Commands

Wraps [`help`](../../docs/cli-reference.md#domain-help):

```
planar help [<subcommand> [<sub-subcommand>]]
planar <subcommand> --help
```

## When To Invoke

When exploring what Planar can do, looking up the flags for a specific command, or orienting a new agent session to the available CLI surface.

## Intent Routing

Match the operator's requested outcome, not only a keyword. Return the first
applicable route with its rationale and example. A route marked **available**
names a canonical skill authored in this checkout. A route marked **CLI
fallback** is the executable path while its planned intent skill is not yet
authored; do not claim that the planned skill can be invoked.

| Intent | Route and rationale | Executable example |
|--------|---------------------|--------------------|
| Resume or recover interrupted work | **Available:** `pl-resume` validates and restores a specific resumable task; use `pl-doctor` when health is degraded or the task is not resume-ready. Both expose the underlying resume, audit, and reconciliation verbs. | `/pl-resume validate 42`, then `/pl-resume 42`; degraded state: `/pl-doctor` |
| Observe active work | **Available:** `pl-observe` assembles a read-only, plan-filtered activity view from `planar dashboard`, `planar-watch`, and handoff reads. Use `pl-status` instead for the current scope's attention queue and claim-aware next work. | `/pl-observe --plan 808`; scope orientation: `/pl-status`; direct claim inspection: `planar-watch claims --plan 808 --json` |
| Manage durable knowledge | **Available:** `pl-knowledge` resolves typed targets, composes scope-safe decision, artifact, annotation, and relationship operations, and verifies their durable post-state. The underlying entity verbs remain available for direct inspection and precise CRUD. | `/pl-knowledge capture "Adopt SQLite WAL" --plan 42 --artifact 17`; direct interfaces: `planar decision --help`, `planar artifact --help`, `planar annotate --help`, `planar links --help` |
| Manage operator-local skills or agents | **Available:** `pl-local` covers import, list, link, unlink, migrate, and repair across the operator-local lifecycle. Retain `pl-local-import` only for legacy or import-only compatibility. | `/pl-local list`; repair links: `/pl-local repair`; legacy import: `/pl-local-import ~/my-skills/` |
| Inspect feedback or triage findings | **Available:** `pl-introspect` previews redacted friction findings; `pl-feedback-triage` previews and applies structured local triage; `pl-report-issue` separately previews an external report. | `/pl-introspect --days 7`; then `/pl-feedback-triage --plan <id>` |
| Maintain published documentation | **Raised to tabularium:** the `pl-documenter` / `pl-doc-maintain` skills moved to tabularium (which owns the doc-system tool they drive); planar routes documentation maintenance to the `tabularium` CLI — see the Docs Domain section below. | `tabularium diff`, `tabularium verify`, `tabularium lint` |

If the request is a verb lookup rather than an outcome, bypass intent routing:

```
planar help task
planar help sync resolve
planar task --help
```

## Output Contract

### Context

Report the resolved scope when the selected workflow is scope-sensitive, plus
the requested intent or verb and whether the route is available or a CLI
fallback.

### Intent

State the interpreted operator outcome in one sentence.

### Actions

Report `attempted`, `applied`, `skipped`, and `failed`. Help routing is
read-only, so a successful selection normally reports one attempted and one
applied route with no mutations.

### Result

Return `outcome=ok|partial|error`, the selected workflow or CLI help path, its
one-line rationale, and at least one executable example. Never return a planned
but unauthored skill as invocable.

### Warnings

Identify ambiguity, unavailable planned workflows, or degraded scope signal.
Do not present normal CLI fallback routing as a failure.

### Next actions

Give zero to three executable commands, beginning with the selected example.

### Recovery

When routing fails or a named verb is unavailable, give the exact inspection
command: `planar help`, `planar <subcommand> --help`, or `planar schema`.

## Docs Domain

Outward-facing documentation under `docs/` is managed through the separately
installed `tabularium` tool and its machine-local manifest database:

- `tabularium build` — recompute the manifest from the current working tree.
- `tabularium verify` — compare the stored manifest root against the current tree (the O(1) root compare).
- `tabularium diff` — list drift records via the drift classifier (regenerate-candidate / hand-edit / new-authoring / deletion / nodoc-stale).
- `tabularium cover <doc-path> <source-path>` — add or remove a source path on a doc entry.
- `tabularium nodoc <path>` — add or remove a path in the nodoc map.
- `tabularium lint [--path <dir>]` — validate GFM footnote citations against the structured `references:` front-matter block; flags `undeclared_citation`, `unused_declaration`, `unresolvable_external`, `unresolvable_planar`, and `malformed_entry` issues.
- `tabularium schema` — print the `tabularium` command tree as a JSON catalog.

> The historical synthesis verbs (`promote`, `regenerate`, `backlinks`,
> `orphans`, `coverage`) are not part of the current Planar CLI.

See [Features: outward-facing docs system](../../docs/features/doc-system.md) for the full mental model.

## Context

Report the requested command path, tool (`planar` or `tabularium`), and text
help mode. If no path was supplied, say that top-level discovery was used.

## Intent

State in one sentence which command or workflow the operator wants to discover.

## Actions

Report `attempted`, `applied=0`, `skipped`, and `failed` counts for help lookups.
An omitted command path is a deliberate top-level lookup, not a skipped action.

## Result

Always report `outcome=ok|partial|error` and a concise summary of the verified
command path, supported arguments, and relevant reference workflow. When no
matching command exists, report that explicit empty result rather than
inventing a verb.

## Warnings

Name ambiguous command paths, unavailable binary help, or documentation that
describes an unshipped surface. A successful top-level listing has no warning.

## Next actions

Give zero to three executable help or workflow invocations that directly match
the request; do not pad the response with unrelated commands.

## Recovery

For a failed lookup, provide the exact broader command, such as `planar help`,
`planar help <subcommand>`, or `tabularium schema`. This skill is read-only and
has no undo path.

## Vendor Notes
