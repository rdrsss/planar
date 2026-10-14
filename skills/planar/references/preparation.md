# Preparing work

This reference describes what Planar needs in order to hold a unit of work and
call it ready, and how to give it that. Planar requires a list of work. It
imposes no particular documents or process on how that list is produced.
Everything else on this page is optional input that Planar reads when present.

Planning writes refuse inside a git worktree (exit 8), so run the verbs below
from the parent checkout. The write verbs use the strict scope resolver: run
from a directory inside the target project, or pass `--scope`.

## Required input: a list of work

Give Planar one of two things.

**Tasks created directly.** Create a plan and add tasks to it:

```
planar plan create "<title>" --slug <slug>
planar task add "<title>" --plan <plan-id> [--slug <slug>] [--body <text|@file>] [--next-action <text>]
```

A task made this way is ready once it has a title and an owning plan and is
unblocked (next section). Nothing else is required.

**A roadmap-shaped artifact, ingested.** Put the work list in one artifact
body and run `planar spec ingest`. Ingest takes the work list from the
artifact of kind `roadmap`; the lowest-id one wins. With no `roadmap`
artifact it takes the single artifact on the anchor whose body parses as a
work list, whatever its kind. With none it refuses with `no work list`; with
two or more and no `roadmap` it refuses as ambiguous (exit 2). The grammar is
under [Work-list grammar](#work-list-grammar).

## Optional input

Ingest also reads these from every artifact on the anchor plan, whatever its
kind. None of them is required, and none of them affects task readiness.

- **Decisions.** Each `###` heading under a `## Decisions` section becomes a
  decision. If the section has no `###` headings, each `-` or `*` bullet
  becomes one decision.
- **Open questions.** Each `###` heading under `## Open Questions` becomes a
  question. A body whose first non-blank line begins with the case-sensitive
  token `Resolution:` also marks the question answered.
- **Scenarios.** Each `### Scenario: <title>` under `## Scenarios` becomes a
  test scenario. A `**Verifies:** task:<slug>[, task:<slug>]` line links it to
  tasks by slug; `**Kind:**` and `**Acceptance:**` lines are lifted out of the
  body. A `#### Scenario:` nested under a group heading also parses.

A title that appears in two artifacts is refused, because each row is keyed by
its title (compared trimmed and case-insensitively).

## When a task is ready

```
planar task packet <task-id> [--json]
planar task packet <task-id> --expand <ref> [--json]
```

`task packet` compiles the current packet for one task and reports
`ready: true` or `ready: false` with named reasons. Readiness covers
coordination facts only: whether the task can be claimed, whether anything
blocks it, whether its citations resolve, and whether an open question or a
proposed decision is attached. The task's body, its next action, touches,
scenarios and cited documents never change `ready`.

The packet's `policy_version` is `routing-packet-v3`. Two packets are
comparable only under the same policy version. `reasons` is emitted in the
order below, each at most once. Read a reason by name, never by position.

| Reason | Fires when | What fixes it |
|--------|------------|---------------|
| `missing_title` | The title is blank. | `planar task update <id> --title "<title>"` |
| `invalid_task_status` | The task is not claimable: its status is not `todo` or `doing`, or a live claim holds it. A `doing` task whose claim is stale, expired or absent is claimable. | Wait for the claim to end or lapse, or reopen the task. |
| `missing_owning_plan` | The task has no plan. | `planar task update <id> --plan <plan-id>` |
| `unresolved_citation` | A cited artifact has no digest or its status is not `active`, `draft` or `ready`. | Restore the artifact (`planar artifact update <id> --status active`) or remove the link. |
| `blocked_by_open_task` | A `depends-on` link from the task reaches a task that is not `done` or `cancelled`. | Finish or cancel the dependency, or drop the link. |
| `open_question_linked` | A question linked to the task, in either direction, is `open`. | `planar question answer <id> --answer "<text>"` or `planar question wontfix <id>`. |
| `proposed_decision_linked` | A decision linked to the task, in either direction, is `proposed`. | `planar decision accept <id>`, `planar decision withdraw <id>`, or supersede it. |

`task packet` takes no caller, so a live claim always makes the task not
ready. `planar-agent claim` and `pull` evaluate the same readiness inside the
claim transaction, `plan next` and `plan recommend-strategy` read the same
verdict, and `planar-agent brief` lists it per unclaimed `todo` task. A
packet's exit code is 0 whether or not the task is ready; it is 1 for an
unknown task and 2 for a non-integer id.

## References and `--expand`

A packet carries no document text. Each artifact the task cites (an
`entity_links` row from the task to the artifact with relationship `cites`) is
listed as a reference: `ref` (`artifact:<id>`), `kind`, `id`, `locator`,
`digest`, `label` and `status`. The digest is read live, so editing a cited
artifact moves the reference digest and the packet digest without changing
`ready`. A changed packet digest tells a reader that the cited text changed
after an earlier read.

`--expand` prints one cited artifact, or one section of it, and nothing else:

```
planar task packet <task-id> --expand artifact:<id>
planar task packet <task-id> --expand 'artifact:<id>#<heading>'
```

The heading match is case-insensitive, and a section runs to the next heading
of the same or shallower level. `--json` prints
`{"ref","digest","text"}`. The digest of a whole-artifact expansion equals the
reference's digest. `--expand` exits 1 when the reference is not among the
task's `cites` links, is malformed, or names a heading the artifact does not
have.

To make a task cite an artifact by hand:

```
planar task link <task-id> artifact:<artifact-id> --relationship cites
```

Ingest writes these links itself: every task under the anchor cites each
anchor artifact of kind `product_spec`, `tech_spec`, `roadmap` or `test_spec`
and the selected work-list artifact, whatever its kind. It also links each
accepted decision derived from the anchor. The links are reconciled on every
apply.

## Touches and decisions

**Touches** declare which repos or files a task changes. They never affect
readiness; `plan recommend-strategy` reads them to judge whether tasks can run
in parallel.

```
planar task touches add <task-id> <repo-slug> [--path <repo-relative-path>]
```

Without `--path` this writes a repo-level `touches` link. With `--path` it also
records a path-level row; repeat the verb once per file. In a work list the
same declaration is `[touches: <repo>, <repo>]` on the bullet.

**Decisions** are records, and a decision attaches to work through links:

```
planar decision add "<title>" --body "<text>" [--rationale "<text>"] --plan <plan-id>
planar decision link <decision-id> task:<task-id> --relationship <relationship>
planar decision accept <decision-id>
```

A linked decision with status `proposed` makes the task not ready until it is
accepted, withdrawn or superseded. An accepted decision never blocks. Questions
work the same way: `planar question add "<title>" --plan <plan-id>`, then
`planar question link <question-id> task:<task-id> --relationship <relationship>`.
The `link` verbs accept `derives-from`, `depends-on`, `addresses`, `verifies`,
`cites`, `supersedes` and `touches`; the readiness check looks at the link's
endpoints and status and ignores which relationship was used.

To make one task wait for another, link it with `depends-on`:

```
planar task link <task-id> task:<other-task-id> --relationship depends-on
```

## Work-list grammar

- Each `##` heading is a milestone and becomes a child plan of the anchor. The
  headings `## Decisions`, `## Open Questions` and `## Scenarios` are reserved
  and never count as milestones.
- Each bullet under a milestone becomes a task. Indented continuation lines
  are folded onto the bullet before annotations are read.
- Annotations in square brackets are lifted out of the task title:
  - `[slug: <slug>]` sets the task slug. It is sanitized to lowercase ASCII
    letters, digits and `-`, and it is globally unique across tasks.
  - `[touches: <repo>, <repo>]` declares touched repos.
  - `[depends: <slug>]` links the task with `depends-on` to the task that has
    that slug.
- Ingest matches an existing plan or task by its title, trimmed and compared
  case-insensitively, without the annotations. Rewording a bullet therefore
  proposes an addition plus a removal, and the new task's slug collides with
  the old task's.
- A task body is rewritten on re-ingest only when it byte-matches a body
  Planar generated itself. A body you replaced is kept. A slug is back-filled
  only when the stored slug is empty.

## Ingest: preview, strict, apply

```
planar spec ingest <plan> [--strict] [--json]
planar spec ingest <plan> --apply
planar spec ingest <plan> --apply --apply-removals
```

`<plan>` is an anchor plan id or slug. Without `--apply` the command is a
read-only preview: it prints the diff (additions, updates, proposed removals)
and a `coverage:` line, and exits 0.

**Strict preview.** `--strict` adds checks and still writes nothing without
`--apply`. It exits 2 on a slug collision with a live task, and on a reference
that resolves to no task: a scenario `**Verifies:** task:<slug>` or a
`[depends: <slug>]` naming a slug that no bullet declares and no task under
the anchor holds. Slug-bearing tasks with no verifying scenario
(`coverage.uncovered_task_slugs`) and scenarios with no parseable `Verifies:`
line (`coverage.orphan_scenarios`) are reported as `warning:` lines on stderr
and the exit stays 0. The `--json` form carries `coverage`,
`slug_collisions` and `summary`.

**`require_scenario_coverage`.** When the anchor plan's effective policy sets
`require_scenario_coverage = true`, `--strict` also refuses uncovered task
slugs and orphan scenarios (exit 2), lists them, and writes nothing. The
policy is the one for the anchor plan's scope in `config.toml`, with a default
of `false`; no environment variable overrides it. A preview or `--apply`
without `--strict` never reads the policy. A policy that cannot be applied, or
a plan whose scope cannot be read, refuses `--strict` at exit 2 with the
reason on stderr. `planar test-spec status <plan-id>` reads live rows, so it
shows coverage only after an apply.

**Apply.** `--apply` is atomic per anchor plan: child plans, tasks, decisions,
questions, scenarios, links, the optional removals and the anchor's `draft` to
`active` flip commit together or not at all. A failed apply leaves nothing
derived behind; fix the source and rerun. Newly added tasks that no authored
scenario covers receive a generated `Verify: <task title>` scenario. Removals
(cancelling orphan tasks, abandoning orphan plans) need `--apply
--apply-removals`; `--apply-removals` alone is a usage error (exit 2).

`--apply` is cross-scope guarded. A scope mismatch exits 5 with no bypass;
pass `--scope <slug>` or run from the anchor's repo. A preview is read-only and
may inspect an anchor outside the cwd scope.

Ingest stages no facts. Re-running the preview after an apply, with the
sources unchanged, shows zero additions, updates and removals.

| Exit | Meaning |
|------|---------|
| 0 | Preview or apply succeeded. |
| 1 | Plan not found, a linked `roadmap` (or, on `--apply`, any linked artifact) has no workbench file, or an apply failed. |
| 2 | No work list or an ambiguous one, a title in two artifacts, `--apply-removals` without `--apply`, or `--strict` refused. |
| 5 | `--apply` refused by the cross-scope guard. |
| 8 | Refused inside a git worktree. |

## Artifact bodies and the workbench

Ingest reads the workbench files of the artifacts attached to the anchor plan
(`artifact add --plan <plan-id>`), not the database bodies, so a body edited
with `artifact update` reaches ingest after a push.

```
planar artifact add "<title>" --kind <kind> --plan <plan-id> --body ""
planar artifact update <artifact-id> --body @<file>
planar workbench push <plan>
planar workbench lint <plan>
```

- The artifact body in the database is stored without front matter.
  `workbench push` renders each file with YAML front matter and a generated
  header above the body, in files named `<id>-<slug>.md`.
- `--body @<file>` reads the file's bytes verbatim. Pass a body-only file.
  Handing it a pushed workbench file stores the front matter inside the body,
  and the next push wraps it twice, so ingest sees no tasks.
- Two safe edit paths: edit the pushed file in place, keep its front matter,
  and run `planar workbench pull <plan>`; or write a body-only file, run
  `planar artifact update <artifact-id> --body @<file>`, then
  `planar workbench push <plan>`.
- `planar workbench lint <plan>` checks front matter without syncing.
  `planar workbench extract-questions <plan> --json` returns the question
  parse ingest would use.
- `planar workbench status <plan>` reports drift and conflicts without
  writing. `pull` applies file changes to the database, `push` applies
  database changes to the files, and `sync` does both; conflicts surface and
  are never merged silently. `planar workbench resolve <event-id> --prefer
  fs|db` settles one conflict.

`planar workbench archive <plan>` removes a feature's workbench files and
leaves the database untouched; `planar workbench restore <plan>` brings them
back. `planar plan closeout <plan-id> --dry-run` previews closing out a plan.

Editing a spec artifact after ingest moves the digest of its reference in each
task packet under the anchor. It leaves readiness alone.
