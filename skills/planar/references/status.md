# Status: what needs attention

Two read-only views. **Scope status** answers "what needs attention now, and what can I
safely pick up?" for the cwd-derived scope. **Plan activity** answers "what has been
happening on this plan?" from timestamped evidence. Neither writes anything; route any
requested change to the reference that owns it.

## Scope status: the read sequence

Use `--json` internally and render concise prose. Count every read under Actions.

1. `planar scope show --json`. If resolution fails, stop with `outcome=error` and the
   exact retry. Never fall back to reading the database or workspace files directly.
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
   the list. Report open handoffs as a global count with id, status and age, flag any
   older than 24 hours as stale, and say in Warnings that they are not scope-filtered.
5. `planar-ext sync status --json`; keep links whose `last_sync_status` is `conflict` or
   `error`. Inspect one with
   `planar-watch sync-events --entity <kind:id> --outcome conflict --json`. Resolution is
   an operator choice; see [external-sync.md](external-sync.md).
6. For each in-flight plan, `planar plan next <plan-id> --json` when the dashboard does not
   explain why a task is or is not available.

## Attention order

Render only non-empty groups, in this order:

1. Sync conflicts and errors.
2. Stale claims and stale handoffs.
3. Blocked tasks and open questions.
4. Active claims: token prefix, task or plan, vendor and role, lease expiry, latest action.
5. Claim-aware next work, by plan priority then task priority.

No empty headings, no `none` placeholders, no completed work. A quiet scope returns its
identity, one result line and useful next actions. Keep the response near 60 lines; when
it would run longer, keep the order, show counts plus the highest-priority rows, and name
the drill-down command for what was omitted.

## Next work is claim-aware

Recommend work only from `plan next` or `planar-agent peek`. Never recommend claiming a
task that the claim-aware result marks claimed, stale or blocked, and never derive next
work from a `todo` list: a `todo` task can already be claimed by another vendor.
Before a claim, the task's packet must be ready; see [claim-ritual.md](claim-ritual.md).

Useful next actions, picked from the actual top items:

- `planar-agent reconcile --dry-run --plan <plan-id> --json` for stale claims.
- `planar resume validate <task-id> --json` for an interrupted task.
- `planar question show <question-id> --json` for an open decision point.
- `planar plan next <plan-id> --json` for available work.

## Plan activity: the live views

For one plan, verify the target first with `planar plan show <plan-id> --json` and
compare its scope with `planar scope show --json`; on a mismatch, stop and ask the
operator to run from the owning project. A draft or finished plan may be absent from the
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
  stream is an empty result, not a parse error. `sync-events --json` emits one envelope
  with a `sync_events` array.
- `--plan` on `actions`, `ps` and `feed` covers the plan, its tasks and its steps, but not
  child plans. On `sync-events` it covers only links attached to the plan itself, not to
  its tasks; use `--entity task:<id>` for those.
- `handoff list` accepts `--vendor` but does not filter by it.
- These views are snapshots. For continuous watching, hand the operator a following
  command such as `planar-watch feed --plan <id> --follow` instead of following inside a
  report. `log` and `sync-events` have no `--follow`.
- Always name a verb. A bare `planar-watch` on a terminal opens the operator's
  interactive agent view; through a pipe it falls back to `feed`, but scripted reads
  should not rely on that. To point an operator at the live view, hand them the bare
  `planar-watch` command.

## Empty versus unavailable

Say "observed empty" only for a successful read that is authoritative for the set, such as
the current claim set from `ps`. A row-capped read (`--limit`, default 100) that returns
nothing matching is "no matching rows in the sample", not "no activity". A failed or
unparseable read is "unavailable": name the source, give its retry, and never report it as
zero.

## What these views do not do

- No writes: no claims, reconciliation, handoff transitions or sync resolution.
- No calls to Jira or GitHub; `sync status` and `sync-events` read recorded local state.
- No audit history beyond the live tables; use `planar audit trail` for that.

Report results with the envelope in [feedback-contract.md](feedback-contract.md).
