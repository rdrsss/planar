---
slug: pl-knowledge
description: "Manage durable decisions, artifacts, annotations, and relationships with scope-safe writes and verified post-state."
source: docs/cli-reference.md#domain-decision
vendor:
  claude:
    argument_hint: "<capture|artifact|annotate|link|show> [args]"
    invocation_examples: |
      /pl-knowledge capture "Adopt SQLite WAL" --plan 42 --artifact 17
      /pl-knowledge annotate --anchor-path src/db/db.zig --line-start 88 "Explain the retry boundary"
      /pl-knowledge link decision:9 artifact:17 --relationship cites
shared_notes:
  - "Resolved scope and knowledge state come from the CLI; resolve every natural-language target to one typed entity before any write."
---

# Planar Knowledge ({{.VendorTitle}})

Manages durable project knowledge through decisions, artifacts, anchored
annotations, and typed entity relationships.

## What It Does

Translate an intent such as “record why we chose this,” “attach the supporting
spec,” or “annotate this implementation” into the narrowest existing Planar CLI
operations. Inspect first, stop on ambiguous targets, apply scope-safe writes,
then read the affected entities and links back before reporting success.

This skill does not create a parallel knowledge store and never writes SQLite
directly.

## CLI Commands

Use only the scope-resolved create operations from
[`decision`](../../docs/cli-reference.md#domain-decision),
[`artifact`](../../docs/cli-reference.md#domain-artifact), and
[`annotate`](../../docs/cli-reference.md#domain-annotate), plus the explicitly
cross-scope [`links`](../../docs/cli-reference.md#domain-links) surface and the
planning-entity reads needed for resolution:

```text
planar scope show --json

planar decision list [--scope <scope>] [--status <status>] [--plan <plan-id>] --json
planar decision show <decision-id> --json
planar decision add <title> --body <text> [--rationale <text>] [--scope <scope>] --json

planar artifact list [--scope <scope>] [--kind <kind>] [--status <status>] [--plan <plan-id>] --json
planar artifact show <artifact-id> --json
planar artifact add <title> --kind <kind> [--body <text> | --from-file <path>] [--source-path <path>] [--scope <scope>] --json

planar annotate list [--anchor-path <path>] [--status <status>] [--plan <id>] [--task <id>] [--vendor <v>] [--tag <tag>] [--scope <scope>] --json
planar annotate show <annotation-id> --json
planar annotate add --text <note> --anchor-path <path> [--line-start <n>] [--line-end <n>] [--commit-sha <sha>] [--text-hash <hash>] [--title <t>] [--body <text>] [--vendor <v>] [--tags <csv>] [--scope <scope>] --json
planar annotate verify [--anchor-path <path>] [--scope <scope>] --json

planar plan list [--scope <scope>] [--status <status>] --json
planar plan show <plan-id> --json
planar task list [--scope <scope>] [--status <status>] [--plan <plan-id>] --json
planar task show <task-id> --json
planar question list [--scope <scope>] [--status <status>] [--plan <plan-id>] --json
planar question show <question-id> --json
planar scenario list [--scope <scope>] [--status <status>] --json
planar scenario show <scenario-id> --json

planar links list <kind:id> --json
planar links add <from-kind:from-id> <to-kind:to-id> --relationship <derives-from|blocks|addresses|verifies|cites|supersedes|touches> --json
```

Artifact kinds are schema-defined. Read `planar artifact add --help` and use an
accepted value; do not infer a kind from prose when more than one fits.

Entity-targeted updates and lifecycle transitions are outside this skill's
supported surface in this cycle. This includes `artifact update`, `annotate
update`, `annotate tag`, `annotate resolve`, `annotate dismiss`, `annotate
archive`, and the decision lifecycle verbs (`decision accept`, `decision
withdraw`, and `decision supersede`). Their current handlers do not compare the
existing entity's scope with the operator's resolved write scope. Do not call
them or simulate their state changes with other verbs; they remain excluded
until the handlers enforce operator-vs-entity scope agreement.

Although `annotate add` resolves the scope of the new annotation before
insertion, this workflow does not pass its `--plan` or `--task` association
flags because the handler does not verify that those existing targets share
the chosen scope. Create the anchored annotation in the resolved scope, then
use an explicit entity link when the operator requests a relationship.

The supported `links` endpoint contract is deliberately narrower than the
engine's storage enum. This skill may link only `plan`, `task`, `question`,
`test_scenario`, `artifact`, `decision`, and `annotation` numeric endpoints.
Resolve a `test_scenario:<id>` through `planar scenario show/list`, and resolve
an `annotation:<id>` through `planar annotate show/list`. `plan_step`,
`session`, and `repo` endpoints are not supported because the current public
read surfaces cannot resolve and inspect them with the same certainty before a
write. Never pass a slug to `links add`; it accepts typed integer ids only.

## Resolve Before Writing

Complete this phase for every target and relationship endpoint before running
the first mutating command:

1. Resolve the cwd-derived scope with `planar scope show --json`. If the
   request names another scope, require the operator to name it explicitly and
   pass `--scope <slug>` on supported reads and writes.
2. Convert explicit references such as `plan:42`, `decision:9`, and
   `artifact:17` directly to typed targets, then inspect them with the matching
   `show ... --json` command. For a name or phrase, run the matching scoped
   `list ... --json` command from the inventory above (including `--plan` when
   the request supplies a containing plan). Match a case-sensitive exact title
   first, then a case-insensitive exact title; if neither exists, candidate
   rows are only case-insensitive title substrings. Use other metadata only to
   present or refine those candidates, never to silently rank one above
   another. Never resolve one entity kind by searching another kind's results.
3. Continue only when every explicit id exists and every phrase produces
   exactly one candidate in its requested kind and scope. Zero candidates is
   `outcome=error` with the exact scoped list command and a create-or-refine
   next action. Two or more candidates is `outcome=error`: print every
   candidate's typed id, title, scope, status, and containing plan when
   available, then request one typed integer id. Stop before the first write;
   do not create, annotate, or link any target in that invocation.
4. Inspect both endpoints of a requested link and validate the relationship
   against the allowed values before mutation. `links` verbs deliberately
   permit cross-scope relationships. If endpoint scopes differ, state that
   fact and require explicit confirmation of the exact typed endpoints and
   relationship; never use cross-scope link semantics to mutate either entity.

The supported create verbs resolve the scope for the new entity. Run from the
owning repository or pass the intended `--scope <slug>`. Existing-entity
mutations are not supported by this skill because their handlers do not yet
enforce the cross-scope guard. Never bypass the scope check in this workflow.

## Knowledge Workflows

### Record a decision chain

For a decision tied to a plan and supporting artifact:

1. Resolve and inspect the plan and artifact, including both scopes, before any
   write. Resolve any requested annotation target at the same time.
2. Create the decision with `decision add ... --json`, retaining its returned
   id. Inspect it with `decision show <id> --json`.
3. Create the requested durable relationships with `links add`, normally
   `decision:<id> --[addresses]--> plan:<id>` and
   `decision:<id> --[cites]--> artifact:<id>`. Use the relationship expressed
   by the operator when it differs; never invent an unsupported relationship.
4. If an anchored implementation note was requested, add it with
   `annotate add` in the resolved scope, with anchor metadata that is actually
   known and without `--plan` or `--task`. Add an explicit annotation
   relationship only when the operator requested one and both typed endpoints
   were inspected.
5. Verify with `decision show`, `artifact show`, `annotate show`, and
   `links list decision:<id> --json`. Report the chain as typed ids plus the
   exact relationship rows and annotation association. Exit code alone is not
   proof of success.

### Manage one knowledge entity

- Decisions: add and inspect. Lifecycle transitions are not supported until
  their handlers enforce the scope-safe contract described above. An explicit
  `supersedes` entity-link records a relationship only; it does not transition
  either decision's status and must not be reported as though it did.
- Artifacts: add and inspect. Prefer `--from-file` for an existing durable
  document and `--body` for inline content. Artifact updates are excluded until
  their handler verifies operator-vs-entity scope agreement. After creation,
  show the artifact and verify its id, kind, status, source path, and requested
  fields.
- Annotations: use them for anchored review notes, not general documents. Add
  only anchor facts that are known and do not attach a plan or task through the
  create command. Annotation update, tagging, and lifecycle mutations are
  excluded until their handlers verify operator-vs-entity scope agreement.
  After creation, show the annotation; when anchor verification was requested,
  also run `annotate verify` and report clean or drifted state.
- Relationships: list before adding to detect an existing identical tuple.
  Treat an already-present tuple as a successful no-op (`applied=0`,
  `skipped=1`), not as a failed duplicate write.

Independent writes are not one atomic transaction. If a later step fails,
preserve and report each verified earlier write; do not claim that the chain
was rolled back.

## Context

Report the resolved scope, exact typed targets, and mode (`inspect`, `create`,
or `link`).

## Intent

State one sentence describing the interpreted knowledge operation.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts. Count each
independent mutation target once.

## Result

Always include `outcome=ok|partial|error`; for writes, include verified
post-state ids and the durable chain in `kind:id --[relationship]--> kind:id`
form, plus annotation associations.

## Warnings

Name assumptions, cross-scope endpoints explicitly confirmed, partial
failures, or degraded post-state verification.

## Next actions

Give zero to three executable recommendations.

## Recovery

Give exact idempotent inspect or retry commands for every unresolved or failed
target. For ambiguity, use scoped list commands and request a typed id; do not
offer a mutation command until resolution.

Omit empty sections except **Result**. A no-op reports zero applied and why it
was skipped without manufacturing a warning. A mixed multi-step result is
`partial`, lists completed and failed targets separately, and gives an exact
retry or inspection command for each failure. Never imply rollback across
independent CLI calls.

## When To Invoke

Use this skill to preserve a technical choice and its rationale, register
supporting material, capture an anchored review note, or create and inspect
durable relationships among Planar entities.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```text
{{.InvocationBlock -}}
```
{{- end}}
