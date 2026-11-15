# Escalation and autonomy

This reference is the single definition of when the operator is needed, how
autonomy changes that, what the decider agent may and may not decide, and the
format of `decisions.md`. It implements PRD-R28 (orchestrator stops and
restarts) and PRD-R33 (autonomy), and TS-20 (escalation and autonomy), TS-19.5
and TS-23 (TS-23.3 and TS-23.11), and the reference layout entry LAY-1.5.11.

## When a decision is the operator's

A decision is the operator's when any of these happens:

- A reviewer raises an open question.
- A planner raises a question, or the plan of a ready task is still
  `status: not-ready` after it was planned again. A plan still not ready is
  escalated as a question, and the reason the plan gives is the question.
- A task is `blocked`.
- An unattributed change cannot be removed. An unattributed change is
  escalated only then: the orchestrating actor first deletes or restores it
  (${CLAUDE_PLUGIN_ROOT}/references/loop.md, "Step 12"), and only when that
  fails are the round's tasks set to `blocked`. This decision always goes to
  the operator, never to the decider, whatever the autonomy setting
  (TS-19.5).

By default the operator is asked every time. Autonomy is off by default.

## Autonomy off (default)

The round's other tasks finish their steps first: one task that needs the
operator does not interrupt them. Then the actor that hit the decision
stops. If that actor is the `orchestrator`
agent or a workflow, it must first write the pending question to the task
record, with the task's `state` set to `question` or `blocked`, and then
return to the session with the task ID. It keeps no other state: the task
records are enough to resume.

The session then:

1. Asks the operator the question.
2. Records the answer: it appends an `answered` line to the task record,
   with the question and the answer, or the decision on a `blocked` task
   (${CLAUDE_PLUGIN_ROOT}/references/task-record.md). It writes nothing to a
   spec. When the orchestrating actor next runs, it writes an answer that
   cites a spec section into the owning spec and seals it
   (${CLAUDE_PLUGIN_ROOT}/references/project.md, "Amending an approved spec
   set"); any other answer or decision stays in the task record only
   (TS-23.3).
3. Starts the driver (orchestrator or workflow) again. It resumes from the
   task records.

## Autonomy on

The operator turns autonomy on for one run, or sets `autonomy.enabled` in
settings. The question then goes to the `decider` agent named in the profile.
Pass it the task record, the cited spec sections and the question.

The decider writes one entry to `decisions.md` and the run continues on that
decision. An entry is a `D-<n>` heading (`n` counts up from 1 across the
file) with four parts: the question, the options weighed, the decision and
the reasoning. Each entry sits under the heading of the run it belongs to.
The `run` skill writes that run heading, which carries the time the run
started, before the run's first entry.

Example of a run heading and one entry:

```markdown
## Run 2026-11-14T10:32Z

### D-3 Which retry policy does T-012 use?

- Task: T-012
- Question: The task says "retry on failure" but not how many times.
- Options: (a) retry once; (b) retry three times with backoff; (c) stop for
  the operator.
- Decision: (b) retry three times with backoff.
- Reasoning: The cited requirement allows transient failures, and three
  attempts matches the existing client behaviour; nothing in the spec forbids
  more than one retry.
```

## Allowance

`autonomy.escalations` is the number of decider decisions allowed in one run.
Its default is 3.

A run lasts from the operator starting or restarting `run` until the run next
returns to the operator. Each start therefore gets a fresh allowance.

When the allowance is used up, treat the next escalation as in "Autonomy
off": stop, write the question to the task record, and return to the session.

## What the decider may choose

| Situation | The decider may |
| --- | --- |
| A question (reviewer or open question) | Answer it. |
| A planner question | Amend the task entry, or stop for the operator. |
| A `blocked` task | Allow one more attempt on the top rung, amend the task entry, or stop for the operator. |

An extra attempt does not reset the task's stall count. A second stall on the
same task goes to the operator, even with allowance left.

The decider also decides what to do at a stall, within the `blocked` choices
above.

## Never delegated

The decider never does any of these:

- Approve a spec set.
- Push or merge.
- Change a policy, a profile or settings.
- Decide on an unattributed change that could not be removed.

A decision that would need one of these goes to the operator, whatever
the allowance. Autonomy does not widen what the decider may do.

## Review afterwards

At the end of a run the session lists for the operator the `D-` entries made
during that run and any amendments they sealed. The operator reviews them
there; the decider's reasoning is already recorded in each entry.
