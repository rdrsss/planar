# Task record

The single definition of the task record: its format, its states, who writes
it, and how to resume from it. Implements PRD-R19 (task record), PRD-R35 (run
records), TS-7 (TS-7.1 to TS-7.4, with the comparison rule of TS-9.4 and the
gap rule of TS-12) and layout entry LAY-1.5.6. Follow it whenever you create,
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
  several tasks run at once.
- The task list holds only a done checkbox per task. The orchestrating actor
  ticks it when the task is approved, and writes no other state there
  (${CLAUDE_PLUGIN_ROOT}/references/tasks.md).

## Format

`record.md` is a front matter block followed by a `## Log` section. Create it
when the task is first picked up, with `state: pending`, `attempt: 1` and an
empty log.

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

- 2026-11-14 16:00 | planned | attempt 1 | planner opus | skill | plan.md
- 2026-11-14 16:02 | mapped | attempt 1 | mapper sonnet | skill | map-1.md
- 2026-11-14 16:05 | packet-rendered | attempt 1 | orchestrator sonnet | skill | worker-packet-1.md
- 2026-11-14 16:19 | worker-done | attempt 1 | worker haiku/high | skill | worker-report-1.md
- 2026-11-14 16:24 | checks-done | attempt 1 | orchestrator sonnet | skill | checks-1.md: 1 passed, 1 failed
- 2026-11-14 16:24 | changes-requested | attempt 1 | orchestrator sonnet | skill | checks-1.md: TEST-C6 failed
- 2026-11-14 16:26 | mapped | attempt 2 | mapper sonnet | skill | map-2.md
- 2026-11-14 16:29 | packet-rendered | attempt 2 | orchestrator sonnet | skill | worker-packet-2.md
- 2026-11-14 16:44 | worker-done | attempt 2 | worker sonnet/medium | skill | worker-report-2.md
- 2026-11-14 16:48 | checks-done | attempt 2 | orchestrator sonnet | skill | checks-2.md: 2 passed, 0 failed
- 2026-11-14 16:50 | review-packet-rendered | attempt 2 | orchestrator sonnet | skill | reviewer-packet-2.md
- 2026-11-14 16:58 | reviewed | attempt 2 | reviewer opus | skill | review-2-spec-fit.md: approve
- 2026-11-14 16:59 | reviewed | attempt 2 | reviewer opus | skill | review-2-correctness.md: approve
- 2026-11-14 17:00 | approved | attempt 2 | orchestrator sonnet | skill | all reviewers approved
```

### Front matter

| Field | Value |
| --- | --- |
| `task` | The task ID. |
| `state` | Where the task stands now; one of the states below. |
| `attempt` | The number of the current attempt, from 1. |
| `base` | The commit the current attempt started from. |
| `profile` | The name of the profile in force for this run. |
| `rung` | The rung of the profile's worker ladder the current attempt uses, counted from 1. The profiles reference says how rungs are chosen and when they change (${CLAUDE_PLUGIN_ROOT}/references/profiles.md). |

Update the front matter whenever one of these changes.

### Log line

Append one line per step, at the end of the log, as it completes. A line is
six fields separated by ` | `, in this order:

1. Time: date and 24-hour time, `YYYY-MM-DD HH:MM`.
2. Step: the name of the state the step moved the task to (see States).
3. Attempt: `attempt <n>`.
4. Actor: the agent or driver that did the step, then its model, then its
   effort after a `/` when one was set: `worker haiku/high`, `mapper sonnet`.
   Give the model and effort the actor was dispatched or run with.
5. Mode: `skill` or `workflow`, the mode the run is in.
6. Result: the file the step wrote, if any, and after `: ` a short outcome.
   Checks: the counts of passed and failed. A review: the reviewer's verdict
   as its file gives it. `changes-requested`, `question` and `blocked`: why,
   and for a question the question itself. `approved`: that all reviewers
   approved.

Two runs are compared by their log lines without the time and mode fields, so
keep those four fields exact and put nothing run-specific in them. Together
with `profile`, the lines show the model and effort at each step, the number of
attempts and every verdict.

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
| `planned` | `mapped` | The mapper finished: `map-<n>.md` is complete. |
| `mapped` | `packet-rendered` | The orchestrating actor wrote `worker-packet-<n>.md`. |
| `packet-rendered` | `worker-done` | The worker returned: `worker-report-<n>.md` is complete. |
| `worker-done` | `mapped` | The worker's report ends `status: map-wrong`. A new attempt starts and the mapper runs again. |
| `worker-done` | `checks-done` | The task's checks were run independently of the worker and `checks-<n>.md` is complete. The state is `checks-done` whether the checks passed or failed. |
| `checks-done` | `review-packet-rendered` | Every check passed and `reviewer-packet-<n>.md` was written. |
| `checks-done` | `changes-requested` | A check failed. No reviewer is dispatched. |
| `review-packet-rendered` | `reviewed` | Every reviewer of the attempt returned a complete review. |
| `reviewed` | `approved` | Every reviewer approved. Tick the task's checkbox. `approved` is the end state. |
| `reviewed` | `changes-requested` | Any reviewer asked for changes. |
| `reviewed` | `question` | A reviewer raised an open question. |
| `changes-requested` | `mapped` | The next attempt begins. |
| `changes-requested` | `blocked` | A profile limit was reached, or the loop stalled on the same finding. |
| `worker-done` | `blocked` | The cap on map-wrong restarts was reached. |
| `question` | `mapped` | The question was answered. |
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
project's gap log.

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
