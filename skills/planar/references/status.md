# Status: what needs attention

Two read-only views. **Scope status** answers "what needs attention now, and what can I
safely pick up?" for the cwd-derived scope. **Plan activity** answers "what has been
happening on this plan?" from timestamped evidence. Neither writes anything; route any
requested change to the reference that owns it.

## Scope status: the read sequence

Every read below takes `--json`.

1. `planar scope show --json` resolves the cwd's scope. Read state through the CLI; the
   database and workspace files are not a substitute.
2. `planar dashboard --agents --json`: in-flight plans, active and stale claims, each
   claim's latest action, and next available work per plan. This is the canonical
   roll-up. It has no `--plan` filter; select a plan from its output.
3. `planar task list --status blocked --json` and
   `planar question list --status open --json`. `task list --status` takes one value;
   a comma list such as `doing,blocked` is refused, so run one call per status.
4. `planar handoff list --status pending,validated --json`. The `--status` filter takes a
   comma-separated list; with no `--status` it returns only `pending`, so an omitted
   filter hides validated handoffs. Handoffs are global, and a list row carries
   `from_snapshot_id` but no task id, so a row cannot be attributed to this scope from
   the list. Open handoffs are therefore a global count, and any older than 24 hours is
   stale.
5. `planar-ext sync status --json`; keep links whose `last_sync_status` is `conflict` or
   `error`. Inspect one with
   `planar-watch sync-events --entity <kind:id> --outcome conflict --json`. Resolution is
   an operator choice; see [external-sync.md](external-sync.md).
6. For each in-flight plan, `planar plan next <plan-id> --json` when the dashboard does not
   explain why a task is or is not available.

## What the reads surface

- Sync conflicts and errors.
- Stale claims and stale handoffs.
- Blocked tasks and open questions.
- Active claims: token, task or plan, vendor and role, lease expiry, latest action.
- Claim-aware next work, by plan priority then task priority.

## Next work is claim-aware

`plan next` and `planar-agent peek` bucket a plan's tasks as available, claimed, stale
or blocked. A `todo` list does not: a `todo` task can already be claimed by another
vendor. A claim refuses a task whose packet is not ready; see
[claim-ritual.md](claim-ritual.md).

Related verbs:

- `planar-agent reconcile --dry-run --plan <plan-id> --json` for stale claims.
- `planar resume validate <task-id> --json` for an interrupted task.
- `planar question show <question-id> --json` for an open decision point.
- `planar plan next <plan-id> --json` for available work.

## Plan activity: the live views

For one plan, `planar plan show <plan-id> --json` gives its scope, which can be compared
with `planar scope show --json`. A draft or finished plan may be absent from the
dashboard; that is a quiet plan, not missing telemetry.

| Question | Read |
|----------|------|
| What actions ran, and under what parent? | `planar-watch actions --plan <id> --json` |
| Who holds or held claims? | `planar-watch ps --plan <id> --stale --json` |
| What happened recently? | `planar-watch feed --plan <id> --since <RFC3339> --json` |
| What did sync do? | `planar-watch sync-events --plan <id> --since <RFC3339> --json` |
| Everything for one claim or task | `planar-watch log --claim <token> --json`, or `--task <id>` |
| Full claim ledger | `planar-watch claims --plan <id> --status all --json` |

How to read them:

- Build the action tree from `parent_action_id`. A parent missing from a capped result is
  a truncated root, not a real root. `planar-watch tree` has no plan filter.
- `ps --stale` returns active and stale claims together, and an expired active claim can
  appear in both lists. De-duplicate by claim id, and count an expired lease as stale,
  never active.
- A recent failure is a `failed` event or an `action_ended` event whose outcome is
  `error`, `aborted` or `timeout`. A release, a completion or a blocked task is not a
  failure.
- `feed --json` and `handoff list --json` emit NDJSON, one object per line; an empty
  stream is an empty result, not a parse error. `sync-events --json` emits one JSON
  object with a `sync_events` array.
- `--plan` on `actions`, `ps` and `feed` covers the plan, its tasks and its steps, but not
  child plans. On `sync-events` it covers only links attached to the plan itself, not to
  its tasks; use `--entity task:<id>` for those.
- `handoff list` accepts `--vendor` but does not filter by it.
- These views are snapshots. `planar-watch feed --plan <id> --follow` follows
  continuously; `log` and `sync-events` have no `--follow`.
- Name a verb in scripted reads. A bare `planar-watch` on a terminal opens the interactive
  agent view; through a pipe it falls back to `feed`.

## Empty versus unavailable

A successful read that is authoritative for its set, such as the current claim set from
`ps`, is empty when it returns nothing. A row-capped read (`--limit`, default 100) that
returns nothing matching means no matching rows in the sample, not no activity. A failed
or unparseable read is unavailable, not zero.

## What these views do not do

- No writes: no claims, reconciliation, handoff transitions or sync resolution.
- No calls to Jira or GitHub; `sync status` and `sync-events` read recorded local state.
- No audit history beyond the live tables; use `planar audit trail` for that.
