# Recovery: health, doctor and stale state

Diagnosis is read-only; each repair is a separate write. The goal is to clear cruft (expired claims, stale handoffs, dead tasks in finished plans),
not to force a green report: legitimate in-flight work keeps health `degraded` by design.

## Read the health verdict

Run `planar health --json` and read the verdict from the `overall` field (`ok` or
`degraded`), never from the exit code. Exit 1 means degraded, but exit 2 is any usage
error (a typo, an unknown flag), so mapping exit codes to health reports a typo as a
critical system. Output that does not parse is a command failure.

Health is global. It counts in-flight tasks and handoffs across the whole database,
whatever the cwd. Many task reads are scope-filtered, so a quiet current scope does not
explain a degraded global count.

## Contributors and their routes

Routes for the contributors that read as unhealthy or useful for orientation:

| Contributor | Meaning | Route |
|-------------|---------|-------|
| `db_ok` false | Critical: database unreachable. | `planar config path`, `planar config validate`; fix the reported problem; recheck health. |
| `integrity_ok` false | Critical: SQLite integrity failure. | Stop. Reconciling a corrupt database is unsafe. |
| `schema_current` false | Database and binary disagree on schema version. | Migration SQL is never written by hand; use a binary that matches the database. |
| `not_resumable_tasks` > 0 | `doing` or `blocked` tasks lacking a `next_action` or a context snapshot. | `planar audit handoff-readiness --json`, then `planar resume validate <task-id> --json` for each task; triage below. |
| `stale_handoffs` > 0 | `pending` or `validated` handoffs older than 24 hours. | `planar handoff list --status pending,validated --json`, then `planar handoff show <id> --json`; triage below. |
| expired claims | Interrupted work holding a lapsed lease. | `planar-agent reconcile --dry-run --json`; triage below. |
| `projection_freshness` `stale` or `missing` | Installed skill or agent files differ from or lack their managed source. | The health output's `repair_command` (a full reinstall) repairs it, as a separate action. |
| `projection_freshness` `unmanaged` | Operator-authored files. | Informational. Usable and never degraded. |
| configuration error | A config file is named by an error. | `planar config path`, `planar config validate`, `planar config show --effective`; suggest `planar config edit` only after validation names a change. |

## The doctor flow

1. **Diagnose.** `planar health --json`. If `overall` is `ok`, there is nothing to
   repair. If `integrity_ok` is false, stop.
2. **Expired claims.** Preview with `planar-agent reconcile --dry-run --json`; its
   `candidates` are claims whose lease has already expired. Running the same
   command without `--dry-run` marks them stale and closes orphaned action rows. It
   never touches a live, heartbeating claim. The preview can list another session's
   claim; narrow the sweep with `--plan`.
3. **Stale handoffs.** For each handoff older than 24 hours, inspect it with
   `planar handoff show <id> --json`, then abandon it:

   ```sh
   planar handoff abandon <id> --reason "stale: no resumer after 24h"
   ```

   Abandon is terminal, and a consumed handoff cannot be abandoned.
4. **Non-resumable tasks.** Nothing repairs these automatically; each needs a judgement.
   Gather `planar task show <task-id> --json` and `planar plan show <plan-id> --json`,
   then classify (next section).
5. **Recheck.** `planar health --json`. Remaining active work keeps health `degraded`
   by design.

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
  check the task's scope with `task show` first.
- `task update` applies its changes from flags; it does not open an editor.
- Planning writes, `task cancel` and `task update` among them, refuse with exit 8 inside a
  git worktree. Run the doctor's writes from the parent checkout.

## Access failures are not schema failures

- `DatabaseUnreadable`, or a `QueryFailed` followed by "the database at <path> is
  read-only to this process" or "refused the operation", means this process may not
  open or write the database: a sandbox, or file or folder permissions. Planar needs
  read and write access to the database file and its folder, because SQLite keeps its
  `-wal` and `-shm` files beside it.
- The database is fine, so `planar init` and `planar-agent reconcile` do not help. The
  harness needs write access under `~/.planar/` (or the folder of `$PLANAR_DB`); retry
  once it has it. A claim whose lease lapsed meanwhile is recovered as in
  [claim-ritual.md](claim-ritual.md#lapsed-claim-recovery).
- `SchemaVersionBehind` now always means the database really is older than the binary.

## What recovery never does

- No direct database writes, migrations or edits to config files.
- No reconcile of a live claim, and no touching of work that moved in the last 48 hours.

Verify every write by reading it back: `planar handoff show <id> --json`,
`planar task show <task-id> --json`, or a fresh `planar-agent reconcile --dry-run --json`.