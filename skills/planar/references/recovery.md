# Recovery: health, doctor and stale state

Diagnose first, read-only. Repair second, one operator-confirmed write at a time. The
goal is to clear cruft (expired claims, stale handoffs, dead tasks in finished plans),
not to force a green report: legitimate in-flight work keeps health `degraded` by design.

## Read the health verdict

Run `planar health --json` and read the verdict from the `overall` field (`ok` or
`degraded`), never from the exit code. Exit 1 means degraded, but exit 2 is any usage
error (a typo, an unknown flag), so mapping exit codes to health reports a typo as a
critical system. If the output does not parse, that is a command failure: report the exit
code and stderr and stop.

Health is global. It counts in-flight tasks and handoffs across the whole database,
whatever the cwd. Many task reads are scope-filtered, so a quiet current scope does not
explain a degraded global count; say so instead of implying it does.

## Contributors and their routes

Explain only contributors that are unhealthy or useful for orientation. Do not dump the
JSON or print empty trees.

| Contributor | Meaning | Route |
|-------------|---------|-------|
| `db_ok` false | Critical: database unreachable. | `planar config path`, `planar config validate`; fix the reported problem; recheck health. |
| `integrity_ok` false | Critical: SQLite integrity failure. | Stop and escalate. Reconcile nothing on a corrupt database. |
| `schema_current` false | Database and binary disagree on schema version. | Report it. Never write migration SQL. Use a binary that matches the database. |
| `not_resumable_tasks` > 0 | `doing` or `blocked` tasks lacking a `next_action` or a context snapshot. | `planar audit handoff-readiness --json`, then `planar resume validate <task-id> --json` per task; triage below. |
| `stale_handoffs` > 0 | `pending` or `validated` handoffs older than 24 hours. | `planar handoff list --status pending,validated --json`, then `planar handoff show <id> --json`; triage below. |
| expired claims | Interrupted work holding a lapsed lease. | `planar-agent reconcile --dry-run --json`; triage below. |
| `projection_freshness` `stale` or `missing` | Installed skill or agent files differ from or lack their managed source. | Report the health output's `repair_command` (a full reinstall). Repair is a separate, explicit action. |
| `projection_freshness` `unmanaged` | Operator-authored files. | Informational. Usable and never degraded. |
| configuration error | A config file is named by an error. | `planar config path`, `planar config validate`, `planar config show --effective`; suggest `planar config edit` only after validation names a change. |

## The doctor flow

Every write below needs an explicit operator confirmation for that specific target. Never
batch approvals across targets.

1. **Diagnose.** `planar health --json`. If `overall` is `ok`, stop and report. If
   `integrity_ok` is false, stop and escalate.
2. **Expired claims.** Preview with `planar-agent reconcile --dry-run --json`; its
   `candidates` are claims whose lease has already expired. On approval, run the same
   command without `--dry-run`: it marks them stale and closes orphaned action rows. It
   never touches a live, heartbeating claim. The preview can list another session's
   claim; confirm with the operator before sweeping work that is not yours, or narrow
   with `--plan`.
3. **Stale handoffs.** For each handoff older than 24 hours, inspect it with
   `planar handoff show <id> --json`, then, on approval, abandon it:

   ```sh
   planar handoff abandon <id> --reason "stale: no resumer after 24h"
   ```

   Abandon is terminal, and a consumed handoff cannot be abandoned.
4. **Non-resumable tasks.** Never auto-resolve these; each needs operator judgement.
   Gather `planar task show <task-id> --json` and `planar plan show <plan-id> --json`,
   then classify (next section).
5. **Recheck.** `planar health --json`. Report what remains honestly; remaining active
   work is an explained state, not a failure.

## Triage: the 48-hour rule

| Situation | Recommendation |
|-----------|----------------|
| The task's plan is `done` or `abandoned`. | Dead work: cancel with `planar task cancel <task-id>`. |
| The plan is active and the task has been idle for weeks. | Keep the work item: reset with `planar task update <task-id> --status todo`. |
| The task moved in the last 48 hours. | Leave it. It is probably live work. |

Before cancelling or resetting a task that is still `doing`, check that no live claim
holds it (`planar-watch ps --stale --json`). A task with a stale claim but finished work
is a lapsed claim, not dead work; recover it as in [claim-ritual.md](claim-ritual.md).

## Scope traps

- `planar task list` is scope-filtered to the cwd. To enumerate in-flight tasks across
  scopes, run it from each project directory, or pass `--scope <slug>` per association
  (slugs from `planar assoc list`). `--scope global` returns only globally scoped tasks;
  it does not span every scope.
- `planar task list --status` takes one value. List `doing` and `blocked` in two calls.
- `planar health` and `planar handoff list` are global; for a cross-scope count, health is
  the source of truth.
- `planar task update` is cross-scope guarded: a task in another scope refuses with exit
  5. Pass the task's own scope with `--scope <slug>` or run from its project. There is no
  bypass flag. `planar task cancel` is not guarded: it writes by id from any scope, so
  confirm the task's scope from `task show` before cancelling.
- `task update` applies its changes from flags; it does not open an editor.
- Planning writes, `task cancel` and `task update` among them, refuse with exit 8 inside a
  git worktree. Run the doctor's writes from the parent checkout.

## What recovery never does

- No direct database writes, migrations or edits to config files.
- No claim on a repair, reconciliation or resume that a separately confirmed command did
  not perform and verify.
- No reconcile of a live claim, and no touching of work that moved in the last 48 hours.

Verify every write by reading it back: `planar handoff show <id> --json`,
`planar task show <task-id> --json`, or a fresh `planar-agent reconcile --dry-run --json`.
Report results with the envelope in [feedback-contract.md](feedback-contract.md).
