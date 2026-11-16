# Task record

The single definition of the task record: its format, its states, who writes
it, and how to resume from it. Implements PRD-R19 (task record), PRD-R35 (run
records), TS-7 (TS-7.1 to TS-7.5, with the comparison rule of TS-9.4 and the
gap rule of TS-12), TS-8.11 and TS-23 (TS-23.3, TS-23.5 and TS-23.14) and layout entries LAY-1.5.6 and LAY-3.23. Follow it whenever you create,
update or read a record.

## Where it lives

Each task has a directory, `tasks/<task-id>/`, in the project directory. The
record is `tasks/<task-id>/record.md`. The files the steps produce (plan,
symbol maps, packets, worker reports, check results, reviews) sit beside it.
The project reference gives their names and writers
(${CLAUDE_PLUGIN_ROOT}/references/project.md).

## Who writes what

- Whoever completes a step writes that step's file, and nothing else in the
  project directory.
- Only the orchestrating actor writes `record.md`: the session under a
  profile whose driver is `session`, otherwise the orchestrator agent or the
  workflow. When a dispatched agent returns, the orchestrating actor appends
  that step's log line, with the model and effort it dispatched the agent
  with, and updates `state`. The record therefore has one writer even when
  several tasks run at once. The one exception is the `answered` line: the
  session that asked the operator may append it, whoever the orchestrating
  actor is (TS-23.3).
- The task list holds only a done checkbox per task. The orchestrating actor
  ticks it when the task is approved, and writes no other state there
  (${CLAUDE_PLUGIN_ROOT}/references/tasks.md).

## Format

`record.md` is a front matter block followed by a `## Log` section. Create it
when the task is first picked up, with `state: pending`, `attempt: 1` and an
empty log. A record made when the task is planned, before the task has
entered a round, has no `base` yet; `base` is set when the task first enters
a round.

```markdown
---
task: T-003
state: approved
attempt: 2
base: 4f2a9c1
profile: basic
rung: 2
---

## Log

- 2026-11-14 16:00 | planned | attempt 1 | planner claude-opus-5-5 | skill | plan.md
- 2026-11-14 16:02 | mapped | attempt 1 | mapper claude-sonnet-5-5 | skill | map-1.md
- 2026-11-14 16:05 | packet-rendered | attempt 1 | orchestrator claude-sonnet-5-5 | skill | worker-packet-1.md
- 2026-11-14 16:19 | worker-done | attempt 1 | worker claude-haiku-5-5/high | skill | worker-report-1.md
- 2026-11-14 16:24 | checks-done | attempt 1 | orchestrator claude-sonnet-5-5 | skill | checks-1.md: 1 passed, 1 failed
- 2026-11-14 16:24 | changes-requested | attempt 1 | orchestrator claude-sonnet-5-5 | skill | checks-1.md: TEST-C6 failed
- 2026-11-14 16:26 | mapped | attempt 2 | mapper claude-sonnet-5-5 | skill | map-2.md
- 2026-11-14 16:29 | packet-rendered | attempt 2 | orchestrator claude-sonnet-5-5 | skill | worker-packet-2.md
- 2026-11-14 16:44 | worker-done | attempt 2 | worker claude-sonnet-5-5/medium | skill | worker-report-2.md
- 2026-11-14 16:48 | checks-done | attempt 2 | orchestrator claude-sonnet-5-5 | skill | checks-2.md: 2 passed, 0 failed
- 2026-11-14 16:50 | review-packet-rendered | attempt 2 | orchestrator claude-sonnet-5-5 | skill | reviewer-packet-2.md
- 2026-11-14 16:58 | reviewed | attempt 2 | reviewer claude-opus-5-5 | skill | review-2-spec-fit.md: approve
- 2026-11-14 16:59 | reviewed | attempt 2 | reviewer claude-opus-5-5 | skill | review-2-correctness.md: approve
- 2026-11-14 17:00 | approved | attempt 2 | orchestrator claude-sonnet-5-5 | skill | all reviewers approved, commit 9b3e5d2
```

### Front matter

| Field | Value |
| --- | --- |
| `task` | The task ID. |
| `state` | Where the task stands now; one of the states below. |
| `attempt` | The number of the current attempt, from 1. |
| `base` | The commit the current attempt started from. Absent until the task first enters a round. |
| `profile` | The name of the profile in force for this run. |
| `rung` | The rung of the profile's worker ladder the current attempt uses, counted from 1. The profiles reference says how rungs are chosen and when they change (${CLAUDE_PLUGIN_ROOT}/references/profiles.md). |

Update the front matter whenever one of these changes.

### Log line

Append a start line before each new dispatch or driver step and a result line
as it completes, using the invocation rules below. A line is
six fields separated by ` | `, in this order:

1. Time: date and 24-hour time, `YYYY-MM-DD HH:MM`.
2. Step: the name of the state the step moved the task to (see States), or
   `started`, `session-observed`, `answered`, `unattributed` or `checks-rerun` (see "Lines that are not
   states").
3. Attempt: `attempt <n>`.
4. Actor: the agent or driver that did the step, then its model, then its
   effort after a `/` when one was set: `worker claude-haiku-5-5/high`,
   `mapper claude-sonnet-5-5`. Give the model and effort the actor was
   dispatched or run with. For Claude and classified legacy behavior, use the full model ID, never an
   alias; a stage with no model logs the driver's full model ID. For Codex,
   the provenance exception below takes precedence.
5. Mode: `skill` or `workflow`, the mode the run is in.
6. Result: the file the step wrote, if any, and after `: ` a short outcome.
   Checks: the counts of passed and failed. A review: the reviewer's verdict
   as its file gives it. `changes-requested`, `question` and `blocked`: why,
   and for a question the question itself. A `changes-requested` line for a
   failed check names the checks file and each failed check, and marks a
   check that also failed on the previous attempt as a repeat, with
   `(repeats attempt <m>)` after it (TS-8.11). `approved`: that all reviewers
   approved, then `commit <short hash>` when the task's change was committed,
   `not committed` when the policy commits nothing, or
   `no change, not committed` when the task's change is empty (TS-23.14).

For Codex preserve these six fields. Dispatched-agent actor model and effort
are the configured full ID and effort; the result includes
`model-source=configured`, `effort-source=configured`, observed model and
effort (or `unavailable`), and an evidence reference when available. For
driver and bookkeeping steps the actor uses host-observed model and effort,
or `unavailable` for each missing value; label each source `observed` or
`unavailable` in the result. Configured fields are `unavailable` unless the
session configuration was explicitly established. Follow the host reference
for discrepancy handling and metadata (${CLAUDE_PLUGIN_ROOT}/references/hosts.md).
Read legacy records without requiring these qualifications; do not rewrite them.

Two runs are compared by their log lines without the time and mode fields.
The comparison also leaves out the actor of a bookkeeping step and of a check
step, which differs with the mode, and the commit hash in an `approved` line
(TS-9.4, TS-23.5). Keep the rest of the other four fields exact and put
exclude invocation and observation links when comparing runs. Together
with `profile`, the lines show the model and effort at each step, the number of
attempts and every verdict.

### Session audit observations

For every new dispatch or driver step assign a project-local unique invocation
ID before work starts. Append a six-field `started` line without changing
state; its result names the invocation and its initial observation file. Use a
fresh ID for a retry and link its predecessor; reuse of a host session does
not reuse an invocation. Link a child invocation to its parent invocation when
known. Append completion, interruption and later discovery lines as
`session-observed` without changing state, linking their observation files in
the result. The ordinary completion line and its state transition still apply;
a start or observation alone never completes a step.

The record writer creates immutable `session-<invocation-id>-<sequence>.md`
files beside the record, sequences starting at 1 for each invocation. Each is
a separate observation, written at dispatch acceptance, completion or
interruption, and when later metadata is discovered. Never rewrite one or a
completed packet or result. Each observation records task, attempt, step,
role, invocation ID, parent invocation ID, predecessor invocation ID for a
retry, host and host version, timestamp with timezone, phase, packet and result
paths, known candidate/base identity, and the resolved configuration evidence
and model provenance required by the packets and host references. Mark unknown
values `unavailable` with a reason. Record driver session ID, dispatched native
session/thread ID, host agent ID and collaboration agent name as distinct
fields, each with its evidence source. A child may supplement the driver's
observation; do not depend on its report for dispatch evidence.

For a transcript locator record its source, storage scope (host, local,
container or remote), checked time and availability (`verified`, `missing`,
`inaccessible` or `unavailable`). Verify only from host metadata or an
exact-ID lookup whose content metadata confirms that native identity. Preserve
any host-exposed parent/child relationship; never substitute a parent locator
for a child, infer a locator from a path pattern, or treat a collaboration name
as a native ID. Use home-relative or configured-root-relative local paths.
If bytes are exported or hashed, record capture time and scope. Live files may
grow; a locator does not guarantee retention. Do not copy transcripts by
default, scan unrelated content, repeatedly probe a known-unavailable surface,
or add a collector, hook or service. Record a necessary collector as a gap.

Audit from task and attempt log links through invocation and parent links;
check the exact native ID before opening a transcript. Missing metadata is an
explicit audit gap, not a reason to erase results or stop normal continuation.
Legacy evidence stays unchanged; a supplemental observation for it must name
its reconstruction source and confidence. Resume does not depend on provider
transcripts. Final checks report linked and unlinked invocations and unresolved
locators. Before advertising a host as audit-linked, demonstrate real driver
and two distinct child-role captures and an exact-ID locator lookup or explicit
host limitation. Synthetic controls do not establish host capture.

### Lines that are not states

Besides the steps in the table under "States", the log has three lines that
do not move the task: `answered`, `unattributed`, `checks-pending` and `checks-rerun`
(TS-23.5). Each is a six-field line like any other. Leave `state` as it is
when you append one. The loop reference says when each is appended, by whom,
and what its result field holds
(${CLAUDE_PLUGIN_ROOT}/references/loop.md, "Log lines this file adds").

## States

`state` is the step most recently completed, or a stop. Write it with
hyphens, as in the log. A state a profile has no stage for is skipped: with no
plan stage there is no `planned`, and with no map stage there is no `mapped`,
so wherever the table sends a task to `mapped` it goes to `packet-rendered`
instead. Never log a skipped state.

| From | To | Cause |
| --- | --- | --- |
| (none) | `pending` | The orchestrating actor creates the record. |
| `pending` | `planned` | The planner finished: `plan.md` is complete. |
| `planned` | `planned` | The planner planned the task again or refreshed its plan. The attempt number does not change. |
| `planned` | `question` | The plan ends `status: question`, or still ends `status: not-ready` although the task is ready. Nothing is dispatched for the task. |
| `question` | `planned` | A planner's question was answered and the planner planned the task again. The attempt number does not change. |
| `planned` | `mapped` | The mapper finished: `map-<n>.md` is complete. |
| `mapped` | `packet-rendered` | The orchestrating actor wrote `worker-packet-<n>.md`. |
| `packet-rendered` | `worker-done` | The worker returned: `worker-report-<n>.md` is complete. |
| `worker-done` | `mapped` | The worker's report ends `status: map-wrong`. A new attempt starts and the mapper runs again. |
| `worker-done` | `checks-done` | All required checks at the first boundary have results and `checks-<n>.md` is complete. The state is `checks-done` whether checks passed or failed; pending batches leave it `worker-done`. |
| `checks-done` | `review-packet-rendered` | Every check passed and `reviewer-packet-<n>.md` was written. |
| `checks-done` | `changes-requested` | A check failed (in a parallel round, failed again on its second run). No reviewer is dispatched. A check that also failed on the previous attempt is marked as a repeat in the log line. |
| `review-packet-rendered` | `reviewed` | Every reviewer of the attempt returned a complete review. |
| `reviewed` | `approved` | Every reviewer approved. Tick the task's checkbox. `approved` is the end state. |
| `reviewed` | `changes-requested` | Any reviewer asked for changes. |
| `reviewed` | `question` | A reviewer raised an open question. |
| `changes-requested` | `mapped` | The next attempt begins. |
| `changes-requested` | `blocked` | A profile limit was reached, or the loop stalled on the same finding. |
| `worker-done` | `blocked` | The cap on map-wrong restarts was reached. |
| `question` | `mapped` | A reviewer's question was answered. |
| `blocked` | `mapped` | A decision was made on how to go on. |

Rules that apply to the table:

- Going back to `mapped` always starts a new attempt: add 1 to `attempt`,
  update `base` and `rung`, and write new numbered files.
- Before setting `question` or `blocked`, append its log line with the question
  or the reason. These two states stop the loop and hand the task to the
  operator, or to the decider in an autonomous run. The escalation reference
  says who decides and what happens next
  (${CLAUDE_PLUGIN_ROOT}/references/escalation.md). The profiles reference
  defines the limits and the stall rule
  (${CLAUDE_PLUGIN_ROOT}/references/profiles.md), and the plans reference
  defines a map-wrong restart (${CLAUDE_PLUGIN_ROOT}/references/plans.md).

## Files are the truth

A step is complete only if its file exists and its last line is
`status: <value>`. A review's last two lines are `verdict: <value>` and
`status: complete`. If the front matter or the log and the files disagree, the
files win: set `state` to match them, and record the mismatch as a gap in the
project's gap log. A step that writes no file (`changes-requested`,
`question`, `blocked`, `approved`, and the lines that are not states) is
taken from its log line. The one exception is `approved`: a record with
`state: approved`, a ticked checkbox and no `approved` line is not a
mismatch. It is an interrupted approval, which the loop reference's "At the
start of every run" finishes (${CLAUDE_PLUGIN_ROOT}/references/loop.md); change
neither `state` nor the checkbox and record no gap for it here. A complete
file whose step has no log line is a mismatch too: append the missing line,
then record the gap. The late line has the time it is written, the session
as actor using the host-specific driver provenance rules under "Log line",
and a result that says it was appended on resume (`<file>: appended on
resume`). For Codex, use the observed model and effort or `unavailable` as
those rules require; do not guess the session identity.

A packet's status line establishes file completion, but does not establish that
its required content is valid for dispatch. Validate and, if necessary, replace
it under the packets reference, "Replacing a malformed completed packet".
The latest unambiguous replacement selection log line identifies the packet to
use for that kind and attempt; retain the original and all earlier selections.
For both worker and reviewer replacements, the selection line is an exception
to the usual state update: leave `state` unchanged when appending it. A completed
replacement without its selection line is verified and selected on resume as
described there. Reconcile all applicable later completed files and log-only
states as usual, retaining the latest valid boundary; selection never regresses
it. Preserve valid worker reports, checks and reviews and continue from the first
unfinished step without redispatching completed work. If later-result
applicability or selection is ambiguous, stop and report it. This is a repair
within the same attempt, not a new attempt or a reset.

A complete pending checks batch is evidence of a partial boundary, not a
`checks-done` or `checks-rerun` result. Reconcile each such batch with a
`checks-pending` log line naming its file and counts; append a missing line
on resume under the same late-line provenance rule. Keep the state at
`worker-done` for the first boundary or `checks-done` for a parallel rerun.
If the final checks file exists, reconcile its corresponding completion line
as usual; the earlier pending batches remain unchanged. Never infer a pass
from a pending entry or from a completion marker on a pending batch.

## Resume

After confirming the previous driver and its children stopped and verifying
the project and working tree as the host reference requires
(${CLAUDE_PLUGIN_ROOT}/references/hosts.md), reconstruct from durable files,
not conversation or a provider cache (PRD-R41, TS-24.5). Report the recovered
task, attempt, completed steps, first unfinished step and capability blockers
before dispatch. Missing required evidence or ambiguous attribution blocks
recovery; do not infer it.

To continue a task, the orchestrating actor:

1. Reads `record.md`.
2. Confirms it against the files in the task directory, applying the rule
   above.
3. Continues from the step after the last complete one.

An incomplete step is run again from its start, except a checks boundary with
complete pending batches: retain their completed results and run only the
outstanding checks as the packets reference defines. For a worker that means a new
worker is given the authoritative packet for that attempt plus a note that the working tree may hold a
partial change. A complete worker report resumes at checks. An incomplete review is run again
from its start; retain completed independent results for the same candidate.
Preserve prior verdicts, attempt history and counters: changing host alone is
not a failed review. Never run a complete step again. Resume works only from the
record and the files; a workflow's own run cache is not used.

## Append only

Never edit or delete a log line or a numbered file. The front matter is the
one part of `record.md` you update in place. A new attempt gets a new number,
and so new files.
