# Resume and handoff

A session that stops mid-task leaves enough durable state for a fresh process, with no
conversation history, to continue. The handoff captures that state; resume reconstructs
it. Both go through `planar`; never reconstruct state from workbench or session files.

## What "resumable" means

`planar resume validate <task-id>` passes when both hold:

1. The task has a non-empty `next_action`.
2. At least one context snapshot exists for that task. A session-level snapshot with no
   task does not count; capture with `--task`.

Task status is not checked, so a `done` task can read as resumable. Failures come back in
a fixed order (`next_action`, then `snapshot`), each with its remediation command. Fix
every failure before handing off.

## Capturing during the session

- Keep reasoning durable as you go: `planar capture note "<text>"` appends to the active
  session.
- Record a task snapshot at meaningful points: `planar capture snapshot "<state>" --task
  <id>`, setting `--next-action` when the next step changes.
- Keep the task's `next_action` an exact, executable next step, not a summary.

Flags: see `planar capture --help` and its leaves.

## Handing off before you terminate

1. Write the final snapshot and next action, as above.
2. **While you still hold the claim**, run `planar handoff <task-id>`, adding `--vendor`
   for the intended receiver and `--note` for context. In one step it records a snapshot,
   inserts a `pending` handoff and validates it to `validated`.
3. The handoff copies `worktree_path`, `repo_root` and `branch` from the task's active
   claim. Hand off before the terminal verb: after `release` there is no active claim and
   those fields stay empty. Empty worktree fields are normal for work done without
   isolation.
4. Read it back with `planar handoff show <handoff-id> --json` and confirm the status is
   `validated`. Then run `planar resume validate <task-id> --json`.
5. Only then end the claim with the one terminal verb your role owns (normally `release`,
   so the task returns to `todo` for the next session); see
   [claim-ritual.md](claim-ritual.md). A dispatched coder returns instead and leaves the
   terminal verb to its orchestrator.

`planar handoff create <snapshot-id>` builds a handoff from a snapshot that already
exists; the bare `planar handoff <task-id>` form is the normal path.

## Resuming from zero context

1. `planar resume validate <task-id> --json`. If it fails, apply the listed remediation
   or stop and report; do not guess at missing state.
2. `planar resume <task-id> --json` for the eight-section packet: identity, state, plan
   position, operational links, recent activity, decisions and questions, linked
   artifacts, and the audit footer. The operational section refreshes stale external
   links, so it can contact Jira or GitHub; a failed refresh with a usable packet is a
   partial result, not an error.
3. **Change directory first.** If `active_claim.worktree_path` is set, `cd` there before
   anything else. With no active claim, the packet falls back to the most recent
   non-abandoned handoff for the task that recorded a worktree, under
   `from_handoff.worktree_path`. In text mode both appear as `worktree:` and `cd:` lines
   in the audit footer. With neither set, no `cd` is needed.
4. Take the claim again (see [claim-ritual.md](claim-ritual.md)). If the task is still
   `doing` from the previous session, claim with `--no-transition`.
5. Mark the handoff taken: `planar handoff consume <handoff-id>`. A consumed handoff is
   terminal.
6. Run the packet's next action.

## Worktree traps on resume

- Inside a git worktree, planning-class writes (`task add`, `task update`, `spec ingest`,
  `question add` and the like) refuse with exit 8 before parsing, and `--scope` does not
  bypass it. Run them from the parent checkout. `planar-agent` verbs, `resume`,
  `handoff`, `capture` and every read work from the worktree.
- A worktree under the project root inherits the parent's scope. A linked worktree
  created outside the project root resolves to no project; pass `--scope` on reads there.
- When the previous session died and its claim is stale, inspect the worktree's
  uncommitted state before reusing it; never discard it without the operator.

## Finding handoffs

- `planar handoff list` returns only `pending` handoffs unless you pass `--status`, which
  takes a comma-separated list such as `pending,validated`.
- Handoffs are global. A list row carries `from_snapshot_id` but no task id, so you cannot
  filter the list by task or scope. Go from the task instead: `resume` surfaces the
  handoff it fell back to.
- Handoffs older than 24 hours that nobody consumed are stale; clearing them is a doctor
  step in [recovery.md](recovery.md).

## What never to do

- Never terminate a session with only conversational context; the next process has none.
- Never claim a handoff or snapshot was rolled back because a later step failed. Each is
  persisted on its own; give the retry for the missing piece, such as
  `planar handoff validate <handoff-id>`.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
