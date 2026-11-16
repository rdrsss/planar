# Execution loop

The single step-by-step description of the execution loop: how approved
tasks are taken in rounds through mapping, coding, checks, review and
approval, and what is written at each step. Every driver follows it, in both
orchestration modes. Implements PRD-N6, PRD-R18, PRD-R31, PRD-R32, PRD-R34 and PRD-R35,
TS-7.5, TS-8 (TS-8.1 to TS-8.12), TS-9.2 to TS-9.4, TS-17.4 to TS-17.6, TS-17.8, TS-20.1, TS-19 (TS-19.1 to TS-19.9), TS-23.14
and the reference layout entry LAY-1.5.8.

This file gives the order of the steps and the rules that belong to the loop
itself. Formats and rules defined elsewhere are linked at the step that uses
them and are not repeated here; read the linked reference before you do that
step for the first time.

## Resource lifecycle

Every driver follows `${CLAUDE_PLUGIN_ROOT}/references/lifecycle.md` in full:
record ownership before creation, persist child results and audit before
retirement, reconcile on resume and at task/round boundaries, and report
teardown on success, failure, timeout, cancellation and before returning.
This also applies while blocked. The lifecycle reference governs exact handles,
evidence retention and unsupported host capabilities (TS-19.9).

## Who acts

**The driver** is the orchestrating actor: whoever does the loop's
bookkeeping. "You" in this file is the driver.

| Mode | Profile `driver` | The driver is | Checks are run by |
| --- | --- | --- | --- |
| skill | `session` | the session the operator is talking to | the driver |
| skill | `orchestrator` | the orchestrator agent, dispatched by the session | the driver |
| workflow | | the workflow script, which orders the steps and calls the orchestrator agent for each bookkeeping step | the check-runner agent |

Under `driver: orchestrator` in skill mode, an orchestrator agent drives one
round only. It returns to the session after every round, and the session
dispatches a fresh orchestrator agent for the next (TS-9.1). Nothing passes
from one to the next except the task records and the session's explicit run
context: resolved autonomy enabled value, allowance and run start time.

Whichever it is, the driver:

- is the only writer of `tasks/<task-id>/record.md`, of the packets and of
  the checkboxes in the task list. The one exception is the `answered` line,
  which the session writes when it has asked the operator (step 15). The
  record's format, its states, the log line, "files are the truth", resume
  and the append-only rule are in
  `${CLAUDE_PLUGIN_ROOT}/references/task-record.md`. Each step below names
  the log line it appends; set `state` to the same name when you append it,
  unless the step says otherwise. A worker or reviewer replacement selection
  line is such an exception: it does not change `state` or regress a later
  completed boundary; reconcile and resume as the task record reference says.
- before each dispatch or driver step, starts and subsequently supplements its
  invocation audit in the task record reference, including interruptions and
  later discoveries; missing metadata does not complete or block the step.
  For an evidenced omitted pre-render start on a completed packet, apply the
  independent verification and disclosure procedure in the task record
  reference before continuation (TS-7.5).
- dispatches every agent with the resolved active-host model and effort the profile gives for it
  (`${CLAUDE_PLUGIN_ROOT}/references/profiles.md`) and writes that model and
  effort in the agent's log line.
- keeps no state outside the task records. It never reviews, never changes a
  path in the working tree except as step 12 says, and never merges, pushes
  or removes the branch or the working tree.

The mode comes from settings, and so do the policy rules `commit` and
`worktree` and the project's replication setting
(`${CLAUDE_PLUGIN_ROOT}/references/settings.md`). In the commands below,
`<base>` is the `base` in the task's record and `<touches>` is the task's
`touches` paths. Run every git command and every check from the root of the
working tree.

### What each agent is given

Every dispatch gives the agent the path of the working tree and tells it to
work there (TS-19.7). Besides that path, give:

| Agent | Also given |
| --- | --- |
| orchestrator | the path of the project directory; the tasks the operator asked for and any stop instruction; the resolved autonomy enabled value, escalation allowance and run start time; the frozen profile source path, digest, version, host, selected bindings and their source paths |
| planner | the path of the project directory |
| mapper | the path of the project directory, the task ID and the attempt number |
| worker | the path of its worker packet |
| check-runner | the path of the attempt's worker packet; for the final check ("After the last task"), the path of the project directory |
| reviewer | the path of the reviewer packet, and its focus |

Give nothing else. The wording for the worker and the reviewers is in
`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Dispatch". These paths are
written in the dispatch only, never in a packet, a report or a record.

### Log lines this file adds

Each uses the six-field line of the task record reference. The first three
do not move the task: leave `state` as it is when you append one.

| Step name | Appended by | Result field |
| --- | --- | --- |
| `answered` | the session after asking the operator; the driver after a decider's decision (step 15) | what was asked (the review file and finding or question, or `plan.md`) and where the answer is recorded |
| `unattributed` | the driver (step 12) | the path, and `deleted` or `restored` |
| `checks-rerun` | the driver (step 16) | as for `checks-done`, with the file `checks-<n>-rerun.md` |
| `checks-pending` | the driver (steps 7 or 16) | pending batch file and passed, failed and pending counts; does not change state |
| `approved` | the driver (step 13) | `all reviewers approved`, then `commit <short hash>` under `per-task`, `not committed` under `none`, or `no change, not committed` when the task's change is empty |

## Uncommitted work and attribution

The tasks share one working tree, and a task's change is everything under
its `touches` that differs from its `base`
(`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Reviewer packet"). Several
tasks can have a change in the tree that no commit holds yet. For each
task, include new files under its `touches` with `git add --intent-to-add`,
then take `git diff <base> -- <touches>` for its reviewer packet. This
keeps sibling tasks' changes out of that task's change (TS-19.5).

A **task with uncommitted work** is any of these. Read which from the
records:

- a task in the current round, or one whose attempt is unfinished because
  a run was interrupted or stopped at a stop point;
- an approved task that was not committed: every approved task under the
  policy rule `commit: none`, and under `per-task` one whose commit was
  interrupted (its record has no `approved` line yet);
- a task that was sent back (for changes, or for a new map), questioned or
  blocked and whose partial change is still in the tree.

A changed path is **attributed** when it lies under the `touches` of a task
with uncommitted work, or when the replicate step wrote it. Under the
project's replication target, the paths the replicate step itself writes
are attributed to the task being approved (step 13), or to the spec
approval for the first copy, made when the spec set was approved. Any other
change under that target is treated like a change anywhere else. Every
other path that differs from the commit the tree is at is an **unattributed
change** (TS-19.5). This definition governs wherever another reference says
"no task in the round".

Never delete or restore an attributed path, whichever round its task
belongs to (TS-8.8). Steps 8, 9 and 12 say how the two kinds are listed and
what is done with an unattributed one.

## Communication and continuation

Follow the progress, handoff and last-observation contract in
`${CLAUDE_PLUGIN_ROOT}/references/lifecycle.md` (TS-9.6, LAY-3.25) for
every driver, including workflow bookkeeping. Refresh the snapshot at
observed transitions and reconcile live host state on resume. Report each
round return to the session; the session continues authorized work by starting
the next round. A round return alone does not establish a live successor.

## Before the first round

The run skill starts the driver only when all of these hold. If you find one
that does not, do no step of the loop; stop and say which.

1. The profile in force was read and passed validation
   (`${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Validating a profile").
2. The spec set passes the approval gate
   (`${CLAUDE_PLUGIN_ROOT}/references/project.md`, "The approval gate").
3. The project repository is a git repository.
4. The working tree exists. It is prepared once, when the operator approves
   the spec set, or by the run skill if it does not exist yet, according to
   the policy's `worktree` rule:
   - `project`: one git worktree for the project at `<project>/worktree/`
     in the project directory, on a new branch `planar/<project>` cut from
     the commit the repository has checked out. If the repository's checkout
     has uncommitted changes, the operator is asked first, because those
     changes will not be in the worktree.
   - `none`: the repository's own checkout on its current branch. When the
     project's run first starts, that is, when no task directory exists yet,
     the run does not start if that tree has uncommitted changes outside the
     replication target. A restart is handled under "At the start of every
     run".
5. If the projects directory is inside the repository, it is ignored by git.
   Otherwise the run does not start.

Then, when the profile has a `plan` stage, dispatch the planner once, for
the tasks that have no `plan.md` only; a `plan.md` with no status line is
incomplete and counts as none. If every task has a plan, do not dispatch
the planner. It writes `tasks/<task-id>/plan.md` for those tasks, as
`${CLAUDE_PLUGIN_ROOT}/references/plans.md` says. For each task it planned,
create the record if it has none, with `state: pending`, `attempt: 1`, the
profile's `name` as `profile`, `rung: 1` and no `base` yet, and append
`planned`, with the planner as actor and `plan.md` as the file.

This holds at every start of the driver. A restart never plans a task
again that already has a plan, whatever the plan's status line says and
whether or not any worker has been dispatched: its plan and its `planned`
line are left as they are. A task that has a plan is planned again only in
step 2.

## At the start of every run

On continuation, first follow the confirmation, verification and operator
explanation in `${CLAUDE_PLUGIN_ROOT}/references/hosts.md`, "Interrupted run
and host handoff" (PRD-R41, TS-24.5). A source-host check that cannot run
remains pending under step 7; a host switch never waives it. Keep applicable
completed independent results, prior verdicts and counters.

Do these, in this order, each time the driver starts, before step 1:

1. **Finish an interrupted approval.** For each task whose checkbox is
   ticked or whose `state` is `approved`, but whose log has no `approved`
   line: finish step 13 from its first part not yet done. Under `per-task`,
   if `git status --porcelain -- <touches>` prints nothing, there is
   nothing left to commit. If `git diff --quiet <base> HEAD -- <touches>`
   then exits 0, the task's change was empty: look for no commit, and
   append the line with `no change, not committed` (TS-23.14). Otherwise
   the commit was already made; take its hash from
   `git log -1 --format=%h -- <touches>` and append the line.
2. **Seal recorded answers.** Only an answer to a question that cites a
   spec section goes into a spec. For each `answered` line of that kind
   whose answer is not yet in the spec the question cites, write it there
   and seal it as `${CLAUDE_PLUGIN_ROOT}/references/project.md`, "Amending
   an approved spec set", says: "An answer given during a run" for the
   operator's answer, "A decider amendment" for a decider's. An entry for
   an answer given during a run adds no ID. Write it under "Answered
   questions" of the spec that defines the cited section, in place of a
   "None." there, as `- <task-id>, attempt <n>, on <cited ID>: <the
   question>` and, on the next line, indented two spaces,
   `Answered <YYYY-MM-DD>: <the answer>`. An operator's
   decision on a `blocked` task, and any answer that concerns no spec
   section, stays in the task record only: write nothing to a spec and seal
   nothing for it. This comes before step 1 because sealing rewrites the
   digests step 1 compares.
3. **Under `worktree: none`, on a restart:** list the unattributed changes
   as step 8 does. Changes of tasks with uncommitted work are expected. Put
   any unattributed change to the operator before continuing; do not delete
   or restore it yourself here.

4. **Handle pending escalations on restart.** Before forming a round, find
   asked tasks in `question` or `blocked` with no later `answered` line.
   For each, use its existing record and go directly to step 15 items 3 and
   4; do not append another question or blocked line. The previous round has
   already finished. Apply the escalation reference's eligibility, allowance,
   second-stall and never-delegated rules using this run's supplied autonomy
   and start time. If a decision needs the operator, return that task to the
   session; do not form a round first. Otherwise seal any decider amendment
   as step 15 says, then continue to step 1 with the same run context.

## Rounds

The loop runs in rounds. A round is a set of ready tasks; the ready rule is
in `${CLAUDE_PLUGIN_ROOT}/references/tasks.md`, "Next task". A **parallel
round** is a round under the profile's `parallel: true`, however many tasks
it holds; under `parallel: false` a round is sequential.

### Step 1. Form the round

1. Recompute the spec digests. On a mismatch, stop the run
   (`${CLAUDE_PLUGIN_ROOT}/references/project.md`, "Digests").
2. Read the task list and find the ready tasks. A task whose record is in
   `question` or `blocked` with no `answered` line after it is not taken.
3. Choose the round's tasks:
   - `parallel: false`: one task, alone: the first in file order that has
     an unfinished attempt, if there is one; otherwise the first ready task
     in file order.
   - `parallel: true`: first every task with an unfinished attempt. If one
     of them is waiting for a second run of a failed check (step 16), add
     no other task. Otherwise go through the remaining ready tasks in file
     order and take each one whose `touches` do not overlap those of a task
     already in the round. Two `touches` overlap when one path equals the
     other or lies under it. A task left out waits for a later round.
   - In both forms, under `commit: per-task`: do not take a ready task whose
     `touches` overlap the uncommitted change of another task that is not in
     the round, that is, one sent back, questioned or blocked whose partial
     change is still in the tree ("Uncommitted work and attribution"). Its
     commit would hold the other task's hunks. It enters no round until
     that change is committed or removed; under `parallel: false`, take the
     first ready task in file order that this does not hold back.
4. The round's base is the commit the working tree is at now.
5. If every task is ticked, go to "After the last task". If tasks remain
   unticked and none can be taken, stop and return, naming them.

A round has no state of its own. Each task's record says where that task
is, so after an interruption each task of the round resumes by itself from
the step after its last complete one
(`${CLAUDE_PLUGIN_ROOT}/references/task-record.md`, "Resume"), and a task
sent back for changes simply joins a later round through step 1. Steps 8
and 12 belong to the round and leave no line of their own to resume from:
after an interruption, do step 8 again.

### Order within a round

The tasks of a round share one working tree, so the round moves stage by
stage, not task by task (TS-19.4):

1. Steps 2 and 3 for every task: all maps are written first.
2. Steps 4 and 5 for every task: all workers are dispatched, at the same
   time in a parallel round.
3. When every worker has returned: steps 6 and 7 for each task. Checks wait
   for all workers because a check sees every task's change.
4. Step 8 once. Then steps 9 to 11 for each task whose every check passed.
5. Step 12 once. Then the outcome step for each task that has a result:
   step 13, 14 or 15 by its reviews, or step 14 for a failed check that
   stands.
6. In a parallel round, step 16 for the tasks waiting for a second run.
7. If a task of the round now waits for an answer or a decision, escalate
   (step 15). Otherwise the next round starts at step 1; an orchestrator
   agent returns to the session first, and a fresh one is dispatched for
   that round ("Who acts").

Steps 8 and 12 run in every round in which a worker returned, whatever the
checks said and whether or not any review follows. One task that needs the
operator does not interrupt the others: every other task of the round
finishes its steps, through its outcome step and step 16, before you
escalate or stop.

In a sequential round this is simply the steps in order for the one task.

## Steps for one task

### Step 2. Start the task, or its next attempt

- **The task enters a round for the first time** (it has no record, or its
  record has no `base`): create the record if there is none, with
  `state: pending`, `attempt: 1`, the profile's `name` as `profile` and
  `rung: 1`. Set `base` to the round's base. This is attempt 1.
- **A record with an unfinished attempt:** continue from the step after the
  last complete one. Do not start a new attempt.
- **A record in one of the states below:** start a new attempt. Add 1 to
  `attempt`, set `base` to the round's base, and set `rung`. The last row
  is the exception: no numbered file of the attempt exists yet, so
  `attempt` stays as it is; set `base` to the round's base and do the plan
  block below.

  | State | Rung of the new attempt |
  | --- | --- |
  | `changes-requested` | one rung up; on the top rung, the same rung |
  | `worker-done`, report ended `status: map-wrong` | the same rung |
  | `question` raised by a reviewer, answered | the same rung |
  | `blocked`, decided | as the decision says (`${CLAUDE_PLUGIN_ROOT}/references/escalation.md`) |
  | `question` raised by the planner (its line names `plan.md`), answered | no new attempt; the planner plans the task again and the rung comes from that plan (the plan block below) |

  The ladder and how rungs are numbered are in
  `${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Ladder".

When the profile has a `plan` stage, attempt 1 does the following before
step 3 or 4, that is, before any map or worker packet of the task exists.
It applies however and whenever the record was created, and again after a
planner's question has been answered:

1. If `plan.md` ends `status: not-ready`, or ends `status: question` and
   the record now has an `answered` line for it, dispatch the planner to
   plan the task again, and append `planned`. The task stays on attempt 1:
   no numbered file of the attempt exists yet. A plan written again after
   an answered question takes the form under "Refresh" in
   `${CLAUDE_PLUGIN_ROOT}/references/plans.md`: the planner adds a
   `## Refresh <YYYY-MM-DD>` section and sets the status line, and all
   earlier text is kept. These are the only cases in which a task that has
   a plan is planned again.
2. Otherwise, the first time the task enters a round only, make the check
   under "Refresh" in `${CLAUDE_PLUGIN_ROOT}/references/plans.md`, and have
   the planner refresh the plan if that check says so.
3. If the plan now ends `status: question`, or still ends
   `status: not-ready` although the task is ready, dispatch nothing for the
   task. It stays out of the round: go to step 15 with the planner's
   question, or with the reason the plan gives for not being ready as the
   question. This holds every time: a plan written again that is still a
   question or still not ready is escalated again, and the task is planned
   once more only after that answer.
4. Set `rung` to the starting rung the plan now gives.

With no `plan` stage, attempt 1 is on rung 1.

Record: the front matter, and `planned` when the planner ran. The driver
writes both.

### Step 3. Map

Only when the profile has a `map` stage. Dispatch the mapper for the task.
It reads the code as it is now and writes `map-<n>.md`
(`${CLAUDE_PLUGIN_ROOT}/references/plans.md`, "Symbol map"). After a
map-wrong restart it finds the previous attempt's worker report in the task
directory, from what it is given.

Record: the driver appends `mapped`, actor the mapper, file `map-<n>.md`.

### Step 4. Render the worker packet

Write `worker-packet-<n>.md` exactly as
`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Worker packet", says. Take
the base commit from the record. When the previous attempt ended on a
failed check, the packet also carries that attempt's `checks-<n>.md` in
full, and its `checks-<n>-rerun.md` if there is one (TS-8.11).

Validate the packet before its final status line and use the replacement
procedure in the packets reference if a completed packet is malformed.
Record: the driver appends `packet-rendered`, file `worker-packet-<n>.md`
or the selected replacement. A replacement selection leaves `state` unchanged.

### Step 5. Dispatch the worker

Dispatch with the authoritative valid worker packet for this attempt. Dispatch
the `implement` stage's agent on the model and effort of the
record's `rung`. It writes `worker-report-<n>.md`.

Record: when the worker returns and its report is complete, the driver
appends `worker-done`, actor the worker with its model and effort, file
`worker-report-<n>.md`. A report is complete when it ends with a status
line, whatever its value, including a last line the worker report format
does not allow: append `worker-done` for it too, so that the line
records the rung's model for this attempt. If the report is missing or has
no status line, the step is incomplete: run it again as "Resume" in the task
record reference says.

### Step 6. A wrong map

Read the last line of the worker's report.

- `status: done`, or any last line other than `status: map-wrong` (one the
  worker report format does not allow): go to step 7. Do not stop and do not ask the
  operator: an unfinished change fails its checks or its review, and that
  `request-changes` moves the task up the ladder as step 2 says (TS-17.4).
- `status: map-wrong`: run no checks and dispatch no reviewer for this
  attempt. This is not a `request-changes` round: it does not count toward
  `revisions` and does not move the task up the ladder.
  - If the task has already had two map-wrong restarts, append `blocked`
    with the reason; the task's outcome step is step 15.
  - Otherwise leave the task as it is. It joins a later round, where step 2
    starts a new attempt and step 3 runs the mapper again.

### Step 7. Run the checks

Who: the driver in skill mode; in workflow mode the check-runner agent,
dispatched by the driver. The worker's report is never a source for a check
result (TS-8.3).

Which checks: the task's `checks` and the common checks that apply to its
paths (`${CLAUDE_PLUGIN_ROOT}/references/tasks.md`).

- Command checks run on every attempt.
- Scenario checks: on attempt 1, all of them. On a later attempt, first
  only those that failed on the previous attempt; when those pass, all of
  them once, before review (TS-8.12).

If a required check cannot run on its required host, leave it pending, not
passed or failed. Stop the task before review and approval, preserve its
completed artifacts and changes, and report the unavailable check. Resume at
this boundary when the required host is available; do not repeat a completed
worker step. Write and log a pending batch as the packets and task-record
references define, retaining completed results. Resume only outstanding checks
at that boundary, including the parallel rerun boundary. Never use a Codex
run as evidence that a Claude check passed.

How, and the format of `checks-<n>.md`:
`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Checks file".

Record: the driver appends `checks-done`, actor whoever ran the checks,
file `checks-<n>.md` with the counts of passed and failed. Append it
only after all required checks have results, before you act on the result.
While any are pending, append `checks-pending` for each durable batch and
stop before review or approval, even if another check failed.

Then:

- Every check passed: the task goes on to review (steps 9 to 11).
- A check failed, sequential round: the failed check stands.
- A check failed, parallel round: the failure may come from a sibling
  task's unfinished change, so it does not stand yet. The task waits for
  step 16.

A failed check that stands skips review: render no reviewer packet and
dispatch no reviewer. It is a `request-changes` round whose finding is the
failed check (TS-8.11), and the task's outcome step is step 14.

In every case the round continues with step 8.

### Step 8. Find unattributed changes

Once per round, when step 7 is done for every task that went on from step 6.
List every path that differs from the commit the tree is
at, untracked files included:

```sh
git status --porcelain -uall
```

From that list remove every attributed path ("Uncommitted work and
attribution"); under the replication target that is only what the replicate
step wrote. Each path left is
an unattributed change. It is new when its status code is `??` or holds an
`A`; otherwise it is a tracked file that was changed or deleted. Take its
diff against the round's base; for a new file, its whole content.

Keep two lists for step 9: the unattributed changes with their diffs, and
the attributed paths that belong to tasks outside the round, each with the
task it belongs to.

Record: nothing here; step 12 records each path.

### Step 9. Render the reviewer packet

Only for a task whose every check passed. Write `reviewer-packet-<n>.md`
exactly as `${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Reviewer packet",
says, with these from step 8:

- under `## Unattributed changes`, the round's unattributed changes, the
  same in every reviewer packet of the round;
- after it, under `## Earlier uncommitted tasks`, the paths of tasks with
  uncommitted work outside the round, each with its task; "None." if there
  are none. Where such a task's `touches` overlap this task's, add the
  `## Change` section of that task's latest reviewer packet, in a tilde
  fence, so the reviewer can tell the hunks apart (TS-8.8).

After a second run (step 16), `## Checks` holds `checks-<n>.md` and then
`checks-<n>-rerun.md`.

Validate before its final status line and use the packets reference replacement
procedure if a completed packet is malformed. Record: the driver appends
`review-packet-rendered`, file `reviewer-packet-<n>.md` or the selected
replacement. A replacement selection leaves `state` unchanged.

### Step 10. Dispatch the reviewers

Dispatch with the authoritative valid reviewer packet for this attempt.
Dispatch each entry of the profile's `review.reviewers` separately, on its
own model and effort. Each writes only `review-<n>-<focus>.md`.

Record: as each reviewer returns with a complete review, the driver appends
`reviewed`, actor that reviewer, file `review-<n>-<focus>.md` with its
verdict. The step is complete when every reviewer of the attempt has
returned.

### Step 11. Combine the verdicts

Read the `verdict:` line of each review and combine them into the task's
result as `${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Reviewers", says
(TS-17.5). Do not act on the result until step 12 is done for the round.

### Step 12. Deal with unattributed changes

Once per round, after the reviews and before any outcome step. Only the
driver does this; no other actor may touch a path outside its `touches`.
For each unattributed change from step 8, in this order:

1. Record: append `unattributed` to the record of every task in the round,
   with the path and `deleted` or `restored`.
2. Remove or restore it:
   - a new file, status `??`: `rm -- <path>`;
   - a new file that was staged, status holding `A`:
     `git rm -f --quiet -- <path>`;
   - a tracked file that was changed or deleted:
     `git checkout <round's base> -- <path>`.
3. Only if that fails, or the path is still listed by the command of step 8
   afterwards: approve no task of the round. Append `blocked` to the record
   of every task of the round that has no `question` or `blocked` line of
   this attempt yet, with the reason: the path, and that it could not be
   deleted or restored. Those tasks do not go to step 13, 14 or 16; the
   outcome step of each is step 15, which puts the change to the operator
   as a change no task accounts for
   (`${CLAUDE_PLUGIN_ROOT}/references/escalation.md`). Its diff is already
   in the reviewer packets.

Then take each task to its outcome step.

### Step 13. Approve

For a task whose result is `approve`, in this order:

1. Tick the task's checkbox in the task list, changing nothing else there.
2. Set `state: approved` in the record's front matter. `approved` is the
   end state.
3. If replication is on for the project, replicate as the replicate skill
   says.
4. Apply the policy's `commit` rule:
   - `per-task`: make one commit holding the task's `touches` paths and any
     replicated files. Stage each by explicit path. Never push. The commit
     follows the target repository's own rules: before the first commit of
     the run, read that repository's `AGENTS.md`, `CLAUDE.md` or
     contributing guide for its message format and sign-off, and follow
     them. Where the repository states none, follow the style of its
     existing history (`git log`) and add nothing the history does not
     use, such as a trailer (TS-23.14). If the task's change is empty,
     that is, `git status --porcelain -- <touches>` prints nothing, make
     no commit.
   - `none`: commit nothing; the change stays in the working tree as
     uncommitted work. This is weaker than a clean base; expect it to
     produce gap entries.
5. Record: append `approved`, naming the commit's short hash under
   `per-task`, or `not committed` under `none`. For a task whose change
   is empty the line says `no change, not committed`, under either rule.

The line comes last so that a record with `state: approved` and no
`approved` line marks an approval that was interrupted. "At the start of
every run" finishes it.

### Step 14. Request changes: limits, stall guard, ladder

For a task whose result is `request-changes`, or whose failed check stands.

Record: append `changes-requested` with the reason. For a failed check the
reason names the checks file and each failed check, and adds
`(repeats attempt <m>)` after a check that also failed on the previous
attempt `<m>`.

Then apply the profile's limits
(`${CLAUDE_PLUGIN_ROOT}/references/profiles.md`, "Limits"), in this order:

1. **Revisions.** Count the task's `request-changes` rounds, failed-check
   rounds included. If `limits.revisions` is a number and the count has
   reached it, the task is blocked.
2. **Stall.** A finding is repeated when its review marks it `repeats:`
   (`${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Review"), or when it is
   a check marked as above. If one finding has now been raised on
   `limits.stall` consecutive attempts, the task is blocked.

An attempt that ended in a map-wrong restart is ignored in both counts and
when finding "the previous attempt": it neither adds to a chain of
consecutive attempts nor breaks one.

If the task is blocked: append `blocked` with the limit reached and the
finding, dispatch no further attempt, and go to step 15.

If neither limit is reached: leave the task in `changes-requested`. It
joins a later round, where step 2 starts the next attempt.

### Step 15. A question or a blocked task

This step is reached with a reviewer's open question (result
`open-question`), a planner's question (step 2), or a blocked task (steps 6,
12 and 14).

1. Record: for a question, append `question` with the question itself. A
   blocked task already has its `blocked` line. These two states stop the
   loop for this task. When the driver is the orchestrator agent or a
   workflow and autonomy is off, this line, with `state: question` or
   `blocked`, is written before it returns to the session with the task ID
   (TS-20.1).
2. Let every other task of the round finish, as "Order within a round"
   says.
3. Escalate as `${CLAUDE_PLUGIN_ROOT}/references/escalation.md` says. It
   defines who decides (the operator, or the decider in an autonomous run),
   when the driver must stop and return to the session, and what the
   decider may choose.
4. Record the answer or decision:
   - From the operator: the session that asked appends `answered` to the
     task record, with the question and the answer, or the decision on a
     blocked task. When the driver next runs, it writes an answer to a
     question that cites a spec section into that spec and seals it; any
     other answer or decision stays in the task record only ("At the start
     of every run"). Under `driver: session` the two are one actor.
   - From the decider: it writes its `D-<n>` entry; the driver appends
     `answered` naming that entry and, when the entry amends a spec, seals
     the amendment at once.
5. The task joins a later round through step 2. The packets of its next
   attempt carry the question and its answer as the packets reference says
   (TS-8.7).

### Step 16. Second run of a failed check (parallel rounds only)

For the tasks left waiting in step 7, after step 12.

When: once every other task of the round is settled. A task is settled
when it is approved (and committed, under `per-task`), sent back,
questioned, blocked, or itself waiting for a second run (TS-19.8).

What: run each failed check of each waiting task once more. If several
tasks are waiting, run all their failed checks together in one pass, in
task-list order. Each result stands; there is no third run. Who runs them
is as in step 7. Write `checks-<n>-rerun.md` for each task, in the checks
file format, holding only the checks run again; `checks-<n>.md` is not
changed. If any rerun is unavailable, preserve a pending batch and stop this
task at the rerun boundary; resume only outstanding reruns before recording
`checks-rerun` or acting on the results.

Record: the driver appends `checks-rerun`, actor whoever ran the checks,
file `checks-<n>-rerun.md` with the counts of passed and failed.

Then, for each task:

- Every check run again passed: all its checks have passed. Do steps 9 to
  11 and its outcome step. Its reviewer packet carries the round's lists
  from step 8 as they were.
- A check failed again: only now is it a failed check for the task. Go to
  step 14.

## After the last task

When the last task is ticked, run the final check (TS-19.6). If
`reviews/final-checks.md` is already complete, do not run it again.

1. Run every command check of every task once more, on the final tree. Who
   runs them is as in step 7: the driver in skill mode; in workflow mode
   the check-runner agent, dispatched by the driver.
2. Write the result to `reviews/final-checks.md` in the project directory,
   in the checks file format of
   `${CLAUDE_PLUGIN_ROOT}/references/packets.md`, "Checks file": the
   heading `# Final checks`, one `## <task-id> <check ID>` section for each
   command check of each task, the counts, and the last line
   `status: complete`.
3. Include linked and unlinked invocations and unresolved locators in the
   final checks, following the task record audit rules. Do not claim a fully
   traceable audit where links or locators remain unresolved.
4. If any check fails, report it to the operator and log it as a gap with
   the gap skill (`skills/gap/SKILL.md` in the plugin). A failure does not
   untick a task and changes no task record.

Then return to the session; the loop is finished.

## Stop point

The run skill accepts an instruction to stop after a named step of a named
task, or after one round, and passes it to the driver. Complete that step
in full, with its file and its log line, or that round, then stop and
return. Nothing else is saved: the next run resumes from the task records.
It is used to hand work over between sessions.

## Workflow handoff

This is the structured handoff between `workflows/run.js` and the
orchestrator (PRD-R18, PRD-N6, TS-9.2 to TS-9.4). The script sequences
calls; the orchestrator owns profile resolution, selection, bookkeeping and
the loop's decisions. The supplied run context is the working tree, project
directory, asked tasks, stop instruction, autonomy and allowance, run
start time, and frozen profile source path, digest, version, host, selected
bindings and their source paths. It also carries `driverOptions`, the
resolved driver invocation options described below. No handoff replaces the
records as durable state.

Before starting the workflow, the run skill constructs `driverOptions` from
the frozen selected driver binding for the actual host: include `model` and
`effort` when configured, or omit each when absent. For version 1 use the
resolved `driver_model` when applicable. This object contains invocation
options, not observations; unavailable observed model or effort must not be
substituted for configured values. The script applies these options, with
`agentType: "planar:orchestrator"`, to bootstrap, selection, and every named
bookkeeping call. The bootstrap compares the supplied options with the frozen
binding as well as the current resolution before proceeding.

The first orchestrator call carries that context and `step: "workflow
bootstrap"`. Reconcile a restart and compare the current resolution with the
supplied frozen configuration before selecting the round's tasks. Stop before
dispatch on any source path, digest, version, host, binding or binding source
mismatch and resolve it explicitly. Return JSON with `status: "ready"`,
`profile` containing the resolved profile, and `tasks` containing the
round's task IDs, including an empty list when no round can be formed. If a
refusal, stop or operator question prevents proceeding, return the terminal
response below instead. The script passes the bootstrap response to each
subsequent call; the orchestrator remains responsible for determining later
rounds from the records and the resolved profile.

After bootstrap, a selection call carries the run context, `step: "next
workflow step"`, `bootstrap`, and `previous`, an array initially empty and
thereafter holding `{task, step, result}` for every dispatch in the last
wave. Selection is read-only: it chooses the next operation without writing
records or log lines. If bookkeeping is next, return JSON with
`status: "bookkeeping"`, `step` naming exactly one bookkeeping operation,
and `task` naming its task ID (use `"run"` for run-wide operations). The
script calls the orchestrator again with that named `step`, `task`, the run
context, `bootstrap`, and `previous`. This call performs only that operation,
serially, and returns `status: "complete"` and `previous`, containing any
wave results still requiring recording. The script passes that array to the
next selection call. This continuation permits consecutive bookkeeping calls,
including separate recording and log actions, without dispatching another agent.
Each recording operation consumes only the result it records; preserve other
results until their own named recording calls complete. Before choosing new
work, record each completed result and its log line. A missing or empty wave
result is an incomplete step, never a success: first record all other completed
results, then return an incomplete response identifying the affected task and
step. Resume from the records on a later run.

Return JSON with `status: "dispatch"` and a nonempty `dispatches` array.
Each entry has `task`, `step`, `prompt`, and `options` with `agentType` and
any model and effort required by the profile. Supply each agent's required
inputs. The workflow starts all entries of the wave together and waits for
all of them before returning their results. Choose waves according to "Order
within a round": dispatch all workers together in a parallel round, after
all their packets are rendered, and do not start checks until every worker
has returned. Preserve every other stage barrier and each task's bookkeeping;
a wave may contain a single dispatch. Dispatch checks, reruns and final
checks to the check-runner. All bookkeeping, including round formation,
packet rendering, recording, limits, approval, tick, commit and replication,
is performed by an orchestrator call regardless of the profile's driver.
Each named bookkeeping call performs one step for its named task; serialize
bookkeeping that shares state. Do not put bookkeeping in a dispatch wave.

For completion return `status: "done"` and the run outcome. For an operator
question return `status: "question"`, the task ID and the full question or
blocked reason, after the other tasks of the round finish. Return a refusal
or incomplete step explicitly. A call returning nothing is incomplete.
Every workflow log line uses mode `workflow`.

## Parity between the modes

Skill mode and workflow mode both follow this file. They produce the same
files with the same names and append the same log lines, apart from:

- the time field and the mode field;
- the actor of a bookkeeping step (written by the driver of that mode) and
  of a check step (the driver in skill mode, the check-runner agent in
  workflow mode);
- the commit hash in an `approved` line.

Both run the same profile with the same packets and agents. A task begun in
one mode is continued in the other by resuming from its record
(`${CLAUDE_PLUGIN_ROOT}/references/task-record.md`, "Resume"). Do nothing
in either mode that the other could not continue from the records alone.
