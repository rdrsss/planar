# Task record

The single definition of the task record: its format, its states, who writes
it, and how to resume from it. Implements PRD-R19 (task record), PRD-R35 (run
records), TS-7 (TS-7.1 to TS-7.4, with the comparison rule of TS-9.4 and the
gap rule of TS-12), TS-8.11 and TS-23 (TS-23.3, TS-23.5 and TS-23.14) and layout entry LAY-1.5.6. Follow it whenever you create,
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

Append one line per step, at the end of the log, as it completes. A line is
six fields separated by ` | `, in this order:

1. Time: date and 24-hour time, `YYYY-MM-DD HH:MM`.
2. Step: the name of the state the step moved the task to (see States), or
   `answered`, `unattributed` or `checks-rerun` (see "Lines that are not
   states").
3. Attempt: `attempt <n>`.
4. Actor: the agent or driver that did the step, then its model, then its
   effort after a `/` when one was set: `worker claude-haiku-5-5/high`,
   `mapper claude-sonnet-5-5`. Give the model and effort the actor was
   dispatched or run with. The model is always its full model ID, for
   example `claude-sonnet-5-5`, never an alias or a short name. A stage
   with no model logs the driver's full model ID. A session knows its own
   full model ID from its environment and writes that.
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

Two runs are compared by their log lines without the time and mode fields.
The comparison also leaves out the actor of a bookkeeping step and of a check
step, which differs with the mode, and the commit hash in an `approved` line
(TS-9.4, TS-23.5). Keep the rest of the other four fields exact and put
nothing else run-specific in them. Together
with `profile`, the lines show the model and effort at each step, the number of
attempts and every verdict.

### Lines that are not states

Besides the steps in the table under "States", the log has three lines that
do not move the task: `answered`, `unattributed` and `checks-rerun`
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
| `worker-done` | `checks-done` | The task's checks were run independently of the worker and `checks-<n>.md` is complete. The state is `checks-done` whether the checks passed or failed. |
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
as actor with its full model ID, and a result that says it was appended on
resume (`<file>: appended on resume`).

## Resume

To continue a task, the orchestrating actor:

1. Reads `record.md`.
2. Confirms it against the files in the task directory, applying the rule
   above.
3. Continues from the step after the last complete one.

An incomplete step is run again from its start. For a worker that means a new
worker is given the same packet plus a note that the working tree may hold a
partial change. Never run a complete step again. Resume works only from the
record and the files; a workflow's own run cache is not used.

## Append only

Never edit or delete a log line or a numbered file. The front matter is the
one part of `record.md` you update in place. A new attempt gets a new number,
and so new files.
