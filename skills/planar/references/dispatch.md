# Dispatching work with Planar

This page covers how to hand Planar work to another agent, a worker, and how Planar records
it. The dispatcher is whatever session assigns the work: an orchestrator, a script, or an
operator at a terminal. Planar records claims, reviews and policy facts; it does not decide
how many workers to run, how to isolate them, or what each one reports. Those choices stay
with the operator's workflow.

All verbs below are `planar-agent` verbs. Flags: `planar-agent <verb> --help`.

## Read the brief

`planar-agent brief <plan-id>` is one read-only call that shows the plan's state, live and
stale claims, the parallel wave, readiness for each todo task, the effective policy and the
command templates for the claim protocol. Add `--json` for programs.

- **Wave.** The wave lists the tasks that can run in parallel now. The serialized
  remainder lists the other todo tasks, each with the reason it is held back (for example
  an unfinished `depends-on` target, a linked open question, or an overlap with another
  task on touched paths). Take the next batch of workers from the wave.
- **Policy.** The brief prints the effective policy and where each value came from. Keys
  that change what a verb accepts are `review_required`, `task_gate` and `require_model`;
  `confirm_before` is only displayed, as a list of verbs the operator wants confirmed
  first.
- **Templates.** The claim, heartbeat, terminal-verb and review commands appear with the
  heartbeat interval derived from the lease length.

`planar-agent peek <plan-id>` shows the next eligible task without writing anything.

## Claim on a worker's behalf

A dispatcher takes the claim and gives the worker its token. Two verbs take it:

```sh
planar-agent claim --entity task:<id> --role coder --model <model> --vendor <vendor> \
  --parent-action <action-id> --worktree <path>
planar-agent pull <plan-id> --role coder --model <model> --vendor <vendor> \
  --parent-action <action-id> --worktree <path>
```

`claim` takes a task you chose; `pull` takes the next eligible ready task on a plan and
reports `no_work` when there is none. Both flip a `todo` task to `doing`.

What each flag records:

| Flag | Recorded as |
|------|-------------|
| `--vendor` | The vendor tag of the worker, such as `claude` or `codex`. |
| `--vendor-session` | The worker's session id. Reviews compare sessions (see below), so set it when a review will follow. |
| `--role` | The role name, for example `coder`. |
| `--model` | The model the worker actually runs, stored verbatim and never validated. When the policy sets `require_model = true`, a coder claim without it is refused at exit 1 and the error names the flag. |
| `--parent-action` | The id of an existing action, usually the dispatcher's own, so `planar-watch tree` shows the worker under it. Without it the worker's action is a root. An id that names no action is refused and nothing is written. |
| `--worktree`, `--repo-root` | Where the worker runs. A handoff copies these from the claim. |
| `--ttl` | The lease length. The default is two hours; `--ttl 4h` or `--ttl 90m` set another. |

A dispatcher that coordinates a plan can hold its own claim with
`planar-agent claim --entity plan:<id>`; `pull` is for workers because it consumes a task.
The `action_id` in that claim's `--json` output is the value to pass as `--parent-action` to each worker's claim.

`claim` refuses a task that is not ready (exit 1, nothing written), names the reasons and
prints the command that would succeed. `--allow-unready` claims it anyway and records the
overridden reasons. `pull` skips unready tasks; `--json` lists them as `skipped_unready`.
`planar task packet <task-id> --json` shows readiness ahead of time. See
[claim-ritual.md](claim-ritual.md).

## Pass the token to the worker

The claim output carries the claim token. A worker needs only the token to heartbeat and
to be the subject of a review, so put the token in the worker's prompt along with the task
and worktree. Run `claim` or `pull` with `--json` when a script reads it; the field is `claim_token`.

## Heartbeats

The worker heartbeats while it works:

```sh
planar-agent heartbeat --claim <token>
```

Send one at least every half lease. Without `--ttl`, a heartbeat renews the lease length the
claim already holds; `--ttl` sets a new absolute length in either direction. The output
carries `expires_at`, which shows whether the lease moved. A claim whose lease lapses
becomes stale; [claim-ritual.md](claim-ritual.md) and [recovery.md](recovery.md) cover
recovery.

### Optional: heartbeat status strings

`--status "<text>"` stores free text, up to 256 bytes, on the heartbeat. It is a display
field, and Planar attaches no meaning to the words except one: `planar-watch` shows the
latest status beside each claim, and a status that begins with `awaiting:` (for example
`awaiting:reviewer`) shows the claim as waiting instead of working. A prefix such as
`validating: <gate>` has no special display; it is just text in the status column.

## Who runs the terminal verb

A claim ends with exactly one of `complete`, `fail`, `release` or `block`. Each flips the
claim and the task status in one transaction. Who runs it depends on how the work was
dispatched:

- **Orchestrator dispatch.** The dispatcher took the claim, so the dispatcher runs the
  terminal verb. The worker heartbeats and returns; it does not end the claim.
- **Direct-claim dispatch.** The agent that took the claim runs the terminal verb itself.

Splitting the ending into `planar task done` plus `planar-agent release` can leave a claim
stranded if the process dies between the two. The terminal verbs avoid that. `planar task
done` and `task update --status` on a live-claimed task refuse and print the terminal
command that would succeed.

## Complete once the work is integrated

`planar-agent complete --claim <token> --summary <text>` marks the task `done`. It does not
look at branches, commits or worktrees, so `done` means whatever the operator's workflow
says it means. Run it when the work has reached the place that workflow calls integrated:
merged to an integration branch, pushed, a pull request opened, or simply written in the
working tree.

One example, for workflows that give each worker its own branch and merge into the
dispatcher's checkout: confirm the worker's commit is an ancestor of the integration head
before `complete`.

```sh
git merge-base --is-ancestor <worker-sha> HEAD && echo integrated
```

A `git merge <branch>` of a misspelt or nonexistent branch name can print "Already up to
date" and exit 0, so the merge's own exit status does not show that the worker's commit
arrived. The ancestry check does.

## Reviews by a separate actor

When the scope's policy sets `review_required = true`, `complete` refuses (exit 1, the claim
stays active) until the claim has an approving review recorded by a different session. The
refusal prints the command that would succeed. A task gate (`task_gate`) additionally needs
a passing queued run recorded on the claim; see [queue.md](queue.md).

The reviewer, a separate actor with its own claim or session, records the verdict:

```sh
planar-agent review --claim <reviewed-token> --verdict approve|request-changes \
  --summary <text> --reviewer-claim <reviewer-token>
```

- `--reviewer-claim <token>` identifies the reviewer by its own claim. `--vendor-session
  <id>` identifies it by session when it holds no claim. One of the two is required.
- The review's session must differ from the reviewed claim's session, or the verb refuses
  with `ReviewerNotIndependent`. When the reviewed claim has no `--vendor-session`, the two
  sessions cannot be compared; pass a `--reviewer-claim` whose claim has one.
- The verdict is stored as one `review` action under the reviewed claim. It changes no task
  status. The terminal verb still goes to whoever owns it (previous section).
- A `request-changes` verdict is a record. What happens next, another round with the same
  worker or a new one, is the dispatcher's call; the claim stays active until a terminal
  verb ends it.

## Handing a worker off

A worker that stops before finishing can leave a handoff with `planar handoff <task-id>`
while it still holds the claim. See [resume-handoff.md](resume-handoff.md).
