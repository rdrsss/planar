# Methodology

This document describes how work is done with Planar. It is written for
people and is kept in step with the system: when the method changes, this
document changes with it.

> **Status.** The plugin contains instructions for this method. Claude
> end-to-end scenarios remain pending, and the Codex minimal loop and its shared
> profile are implemented but unverified. The steps below describe the intended
> operation rather than asserting that each scenario has passed.

For the parts the method uses, see [architecture.md](architecture.md).

The Codex minimal loop
uses a selected non-Astra driver, a pinned worker, independent checks and a
separate pinned reviewer. Configured model and effort are recorded separately
from host observations; unavailable observations are recorded as such, never
guessed. See [the host contract](../plugins/planar/references/hosts.md) for the tested
surfaces, restrictions and remaining acceptance work. Richer Codex profiles
remain deferred. Skills select the host first and stop before project writes
when the required Codex capability cannot be honoured. Codex skills read the
host contract relative to their discovered installed location; the contract
resolves bundled links from the package root. Each bundled role's complete
instructions and bounded inputs go to a fresh context, with its write and
review restrictions preserved. These Planar-specific paths and dispatches
still require scenario verification. A scratch app-server probe observed an
installed skill reading a bundled reference, fresh worker and reviewer
contexts with an independent check, and a later session reading durable files.
A separate native collaboration probe exercised explicit model and effort
pins; its responses did not expose backend observations. Neither probe proves
installed Planar discovery, the complete loop or cross-host recovery. See the
[installation status](../README.md#installation-and-status) and host contract
for the tested procedure and limits.

## The idea

Long-running work fails when the understanding of it lives in one
conversation. Planar moves that understanding into Markdown specs, then
executes the work in small tasks. Each task is done by a worker that sees
only what the specs give it, and judged by a reviewer that did not write it.

The method has two phases:

1. **Specs.** Agree what is being built, in writing, before building it.
2. **Execution.** Build it one task at a time, with independent review.

```mermaid
flowchart TD
    A[Operator states the work] --> B[Gather context]
    B --> C[Draft the five specs]
    C --> D[Check IDs, citations and traceability]
    D --> E{Spec reviewers}
    E -- request changes --> C
    E -- approve --> F{Operator}
    F -- changes --> C
    F -- approve --> G[Pick next task]
    G --> H[Render worker packet]
    H --> I[Worker implements]
    I --> J[Checks run independently]
    J -- pending --> P[Preserve results and resume outstanding checks]
    P --> J
    J -- all results available, a check fails --> H
    J -- pass --> K[Render reviewer packet]
    K --> L{Reviewer}
    L -- request changes --> H
    L -- open question --> Q[Ask operator]
    Q --> H
    L -- approve --> M[Tick the task]
    M --> N{Tasks left?}
    N -- yes --> G
    N -- no --> O[Report to operator]
```

## Roles

- **Operator.** The person directing the work. Answers open questions,
  approves the specs and decides what merges.
- **Session.** The host session the operator is talking to. Runs the
  skills and reports. Under the minimal profile it also drives the loop.
- **Orchestrator.** An agent that drives the loop and keeps the task
  records, under the basic profiles.
- **Spec checker** and **spec reviewer.** Agents that check and judge a spec
  set before the operator is asked to approve it.
- **Planner** and **mapper.** Agents that write each task's plan and symbol
  map, under the basic profiles.
- **Worker.** An agent that implements one task.
- **Reviewer.** An agent that judges one task's result.
- **Decider.** An agent that takes the operator's decisions in an
  autonomous run, within limits.

No role approves its own work. Whoever drives the loop does not review, the
worker does not approve, and no reviewer makes the fixes it asks for.

## Phase 1: specs

### 1. Name the project

The work gets a project name and a directory. The name is derived from
context unless settings or the operator say otherwise. The operator is told
the name before anything is written, and is asked what to do if it is
already taken.

### 2. Gather context

Before drafting, the orchestrator collects what the specs will cite: the
relevant code, existing documents and what the operator has said. Specs make
claims, and claims need sources.

### 3. Draft the five specs

The specs are drafted in order, each building on the last:

1. **PRD**: what is wanted and why, with acceptance scenarios in Gherkin.
2. **Tech spec**: how it will be built.
3. **Layout**: the files and directories involved.
4. **Testing spec**: how each requirement is checked.
5. **Tasks**: ordered units of work, each small enough for one worker.

While drafting:

- give every referable section a stable ID;
- cite a source for every factual claim, or mark it as an assumption;
- write down open questions instead of guessing, and record the operator's
  answers with the date.

### 4. Check IDs, citations and traceability

Before review, a check confirms that:

- every section ID is unique;
- no existing ID has changed or disappeared without being marked withdrawn;
- every citation resolves to a real section, file or decision;
- every requirement is built by a task and tested by a check, or says why
  not; every task cites what it implements; every check is used.

A spec set that fails this check does not go to review.

### 5. Spec review

Separate spec-review agents read the set adversarially. The project's
profile says how many there are and what each looks at. There are three
standard focuses:

| Focus | Looks for |
| --- | --- |
| Intent | Each thing the operator asked for, matched to the sections that carry it out, or reported as a gap. Also anything specified that nobody asked for, and questions that should have been asked. |
| Consistency | Places where the specs disagree, requirements nothing implements, parts no task builds, and checks that could not actually fail. |
| Feasibility | Steps that cannot be done as written, and claims about the platform or codebase that need to be tried first. |

The minimal profile uses one reviewer for all three; the basic profile uses
one each. A project can add lenses for what its work touches: API and
interfaces, security, and stored data and migration.

Each reviewer writes its own report and returns approve or request changes.
If any asks for changes, the set goes back to drafting. No reviewer
rewrites the specs. On later rounds a reviewer first says which of its
earlier findings are closed, then looks only at what changed.

### 6. Operator approval

Only after every spec reviewer approves is the operator asked to approve.
Execution does not start without both.

## Phase 2: execution

By default the work is done in a git worktree for the project, on its own
branch. A policy can instead select the repository checkout and can leave
changes uncommitted. Which stages run depends on the
project's **profile**. The steps below are the ones every profile has; the
basic profiles add two before them:

- **Plan.** After approval, a planner writes an implementation plan for
  every task that has none. A plan is refreshed later if the files it names
  have changed. If the planner finds a task cannot be done as specified, it
  says so before any coding starts. Once the question is answered, the task
  is planned again and the planner is given the answer.
- **Map.** Before a task is coded, a mapper writes its symbol map: what to
  change, where new code goes and what to imitate. The worker may change
  only what the map names.

Tasks are taken from the task list in order. Under a parallel profile, all
the tasks that are ready and touch different files are coded at the same
time, then checked and reviewed. For each task:

### 1. Render the worker packet

The packet holds the task, the spec sections it cites, the files it may
touch and the checks it must pass. It is written so that a worker with no
other context can do the task.

### 2. The worker implements

The worker makes the change and returns it. It does not mark the task done
and does not approve its own work.

### 3. The checks are run independently

The checks named in the testing spec for this task are run by someone other
than the worker: whoever drives the loop in skill mode, a check-runner agent
in workflow mode. The worker's own report of the results is not used. When
the last task is approved, the same runner runs every task's command checks
once more on the finished work.

When a required check is unavailable, the runner preserves obtained results
and pending entries in an immutable batch. The task stays at the checks
boundary, even if another check failed. On resume, the runner retains completed
results and runs the outstanding checks; the aggregate is written when all
required results are available. Only then does a failed check send the task
back to a worker without review. In a parallel round a failed check is first
run once more when the other tasks of the round have settled, because a
sibling's unfinished change can make it fail; only a second failure counts.
The same pending boundary applies to that rerun.

### 4. Render the reviewer packet

This packet holds the task, the cited spec sections, the diff and the check
results from that independent run.

### 5. The reviewer judges

Each reviewer returns one verdict, with findings that cite spec sections. A
profile may have several reviewers looking at different things; if any of
them asks for changes, the task goes back.

| Verdict | What happens next |
| --- | --- |
| Approve | The task's checkbox is ticked in the task list and, under the default policy, the change is committed. |
| Request changes | The task goes back to a worker with the findings, on a stronger model if the profile has one. There is no limit on rounds by default, but if the same finding is raised on two attempts running, the loop stops and escalates. |
| Open question | The operator is asked (or the decider, in an autonomous run), the answer is recorded in the task record (and in the spec too, when it settles a point a spec section covers), and the task continues. |

### Recording as you go

Every step above is written to the task's own record as it happens: the
packets, each attempt, the check results and each verdict. The task list is
touched only once, when the task is approved.

## Seeing dependencies

Every task list includes a complete Mermaid flowchart derived from task entries:
each task, including isolated tasks, has a label with its ID and current title,
and each `after` dependency is an arrow from prerequisite to dependent. The
entries are authoritative. Complete-graph titles omit inline code backticks
and normalize whitespace for mechanical comparison. Authors create the graph
with the tasks and update it in the same edit as task or dependency changes,
including label changes for renamed tasks. Large graphs can be grouped or split with explicit boundary IDs;
a labelled subset overview may use shorter titles and accompany, but cannot
replace, the complete graph. Subset overviews are excluded from complete-graph
checks. An empty list says no tasks are defined. The graph carries no execution
state. The spec checker compares its nodes and edges to the entries and rejects
drift, unknown or self dependencies and cycles before review.

Alongside the graph, explain potential parallel paths and shared files or
directory overlaps that constrain them. Shared paths do not add dependency
arrows. Independent paths alone do not authorize dispatch: readiness, profile
concurrency and file ownership determine each round.

## Stopping and resuming

Sessions can stop at any step, including after a usage limit, cancellation,
crash, connectivity failure or deliberate stop. Before a replacement run,
the operator confirms the previous driver and all its children have stopped;
an explicit confirmation already given is sufficient. A parent reaching its
limit alone does not establish that its children stopped.

A fresh session can continue with the project location and request. It reads
specs, task records, packets, artifacts and Git state, verifies the approval
digests, working tree, profile and attribution, and explains the recovered
task, attempt, completed steps, next step and blockers before dispatch. It
needs no old conversation, handoff summary, provider cache or source-vendor
model call. Missing evidence or ambiguous changes stop recovery rather than
being guessed.

Each new step starts with an invocation recorded before work. The driver adds
immutable sourced observations for acceptance, completion or interruption and
later locator discoveries. Native IDs and collaboration names are distinct;
unavailable or unverifiable locators remain audit gaps. Final checks report
linked and unlinked invocations and unresolved locators. Recovery uses durable
project evidence, independently of provider transcripts.

A completed worker report leads to checks without repeating the worker. A
partial worker is restarted with the same packet and a note about partial
changes. An unfinished check or review is rerun, retaining completed
independent results for the same candidate; pending check batches retain
their completed results. A completed file with a stale log is reconciled,
and interrupted approval or commit bookkeeping is finished without duplicates.
Past evidence, verdicts, attempts and revision limits survive a host change.
A compatible profile change affects future dispatches; an incompatible one
blocks the affected dispatch. A required check on an unavailable source host
stays pending and blocks approval. Live cross-host recovery in both directions
remains to be verified.

## Changing a spec after approval

Approval is of the exact text, apart from ticking tasks off. If any spec
changes afterwards, the run stops. A change goes back through the ID and
citation check, and the operator then either signs off a small amendment,
such as a typo, or sends a changed requirement back through spec review.

An answer the operator gives to a question during a run is its own
sign-off: it is written into the spec and the task record, and the run
continues. In an autonomous run the decider can answer questions and adjust
task entries, never requirements, and the operator is shown each one.

## Autonomous runs

By default a run stops whenever a decision is the operator's. If the
operator asks for an autonomous run, a decider agent takes those decisions
instead, up to a set number per run, and records each with its reasoning.
The decider never approves a spec set, pushes, merges or changes settings.
At the end of the run the operator is shown what was decided.

## Skill mode and workflow mode

The execution loop can be run by Planar's own orchestration skill or by a
Claude Code dynamic workflow. Codex is scoped to the minimal skill-mode
profile; workflow mode, richer profiles and autonomy are deferred there. The
instructions specify the same steps, packets, agents and records in both, and
both read the same profile. Skill mode is the default; workflow mode
requires an available and permitted host workflow. Continuation between modes
is specified but remains subject to scenario verification.

## Replication

If replication is turned on for the project, its Planar files are copied into
the repository: first when the operator approves the spec set, then again
each time a task completes. It is off unless the operator turns it on.

## Resource lifecycle

The driver records ownership and intended lifetime before creating run
resources, and preserves results and audit evidence before retiring children.
It reconciles owned resources on resume and at boundaries, including failures
and cancellations, and reports retained, unsupported or failed cleanup as
well as releases. It protects user changes and sole evidence. The
[lifecycle reference](../plugins/planar/references/lifecycle.md) defines the
rules and their host capability limits (TS-19.9).

## Progress and stops

Before dispatch the driver names the next bounded milestone, its owner and
any needed input. While a parent turn is active it reports observed progress,
failures and stops, including the next boundary. A coordinator returns its
result and evidence to its parent; a normal round return lets the session start
the next authorized round. Planned work alone does not mean an actor is still
running. The final report identifies who actually continues, or why work has
stopped, plus pending acceptance and cleanup.

The replaceable `reviews/run-status.md` records the last observation and its
clock and identity sources, evidence, next action, input and owner, and pending
checks and cleanup. It is a convenience, not live status; on resume the driver
checks the host and task records. Unknown observations stay unknown. The host
may not deliver an unsolicited update after the turn ends, and an unexpected
termination may prevent a final report. No background watcher is implied. See
the [lifecycle reference](../plugins/planar/references/lifecycle.md) (TS-9.6).

## When the method falls short

Planar has no engine by choice. When a step goes wrong, or goes slowly, in a
way that validation, identities or a database would have prevented, the
orchestrator writes the gap down with the evidence: what happened, what it
cost and what would have helped. It does not build the missing tool.

The record of gaps is reviewed after the first release. Tooling is added only
where that record shows it is needed.

## Keeping these documents true

`architecture.md` and `methodology.md` describe the system as it is. A change
to how Planar works is not finished until these two documents say the same
thing.
