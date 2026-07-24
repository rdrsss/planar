---
description: Pull from and push to the operational plane, inspect field-level conflicts, and coordinate explicitly approved reconciliation.
origin: docs/cli-reference.md#domain-sync
shared_notes:
    - Active scope and sync state come from public CLI surfaces; never open SQLite or call an adapter directly.
    - A conflict resolution is whole-entity, preview-first, and bound to one explicitly approved event and disposition.
    - 'No conflicts is a verified clean no-op: do not dispatch sync-reconciler and do not call sync resolve.'
slug: pl-sync
vendor:
    claude:
        argument_hint: <pull|push|status|resolve> <target> [args]
        invocation_examples: |
            /pl-sync pull task:<task-id>
            /pl-sync push task:<task-id>
            /pl-sync status
            /pl-sync resolve <event-id>
---

# Planar Sync ({{.VendorTitle}})

Pulls and pushes local entities through registered external systems, reports
sync state, and coordinates evidence-based conflict reconciliation. Sync is
always on demand. The workflow uses the operator-plane CLI for every mutation;
it never opens SQLite, reads credentials, or invokes Jira or GitHub directly.

## Context

Resolve and retain the cwd-derived scope, requested operation, target or
filters, output mode, and any explicit `--scope <slug>` override. Single-target
`pull`, `push`, and `resolve` are guarded writes: run from the owning repository
or pass the entity's scope explicitly. Never add the legacy scope bypass.

Use JSON internally whenever available. A pull may exit with the documented
conflict exit while still emitting valid per-link JSON; preserve those rows as
conflict evidence rather than treating the whole invocation as an adapter
failure.

## Intent

Interpret the request as: perform the requested on-demand sync operation,
surface every affected target, route observable conflicts through the
sync-reconciler, apply only the exact resolutions the operator confirms, and
verify durable post-state.

## Supported commands

Construct commands only from the shipped surfaces:

```text
planar sync pull <link-id|kind:id> [--system <slug>] [--scope <slug>] --json
planar sync pull --all [--system <slug>] --json
planar sync push <link-id|kind:id> [--system <slug>] [--scope <slug>] --json
planar sync push --all [--system <slug>] --json
planar sync status [--entity <kind:id>] [--system <slug>] --json
planar audit trail --link <link-id> --json
planar <kind> show <id> --json
planar sync resolve <event-id> --keep <local|remote>
  --evidence-token <token> --expected-local-updated-at <timestamp>
  [--scope <slug>] --json
```

`sync status --json` returns one JSON object per external link, including
`link_id`, entity identity, external identity, system ID, last-sync time, and
`last_sync_status`. `sync status` exposes current link state, not recorded sync
events; it does not identify the durable conflict event or carry field values.
For each conflicting link, use `audit trail --link ... --json` to read its
`sync_events`, including event ID, direction, outcome,
`fields_changed`, detail, and timestamp. Do not invent a `sync conflicts` or
`sync events` subcommand.

## Workflow

### 1. Run the requested operation

- `status`: read the requested entity/system view and make no mutation.
- `pull`: run the exact target form. Treat `ok` and `noop` rows normally and
  retain every `conflict` row for reconciliation. A malformed row or adapter
  error remains a failure.
- `push`: run the exact target form and retain per-link successes and failures.
- `resolve <event-id>`: do not call `sync resolve` immediately. Locate the
  event through the conflicting link's audit trail and enter the reconciliation
  flow below.

After pull or push, read `sync status --json` with the narrowest available
entity/system filter. For a numeric link target, use its returned entity from
the operation or audit trail to construct the entity filter. Exit code alone is
not proof that the intended links changed.

For `--all`, each external call is an independent target. Preserve completed
rows if another fails; Planar does not promise one transaction across remotes.

### 2. Detect a clean no-op

If the verified status contains no `last_sync_status="conflict"` rows in the
requested target set, do not dispatch sync-reconciler and do not call
`sync resolve`. Return `outcome=ok` with zero conflicts and zero reconciliation
attempts. An empty status is also a clean no-op when the requested filters
legitimately match no external links; say that explicitly and do not warn.

Do not call an old conflict event resolved merely because it remains in the
append-only audit trail. A link is currently conflicted only when its status is
`conflict` and no later resolution event has closed it.

### 3. Build field-level evidence

For every currently conflicting link:

1. Run `planar audit trail --link <link-id> --json`.
2. Select the latest unresolved `sync_events` row whose `outcome` is
   `conflict`; retain its ID, direction, timestamp, `fields_changed`, and exact
   detail. If the caller supplied an event ID, require an exact match.
3. Run `planar <kind> show <id> --json` for current local post-state.
4. Read the event's structured `evidence` object into one row per conflicting field. Each row must
   show field name, observable local value, observable remote value, source,
   observation time, and the provider remote version. Preserve the raw event detail beside the normalized
   rows so the operator can audit the interpretation.
5. Include link ID, entity reference, external ID, system identity available
   from the public results, direction, and relevant preceding sync events.

Field names alone are not enough. If the durable detail does not expose both
values for every conflicting field, if it is truncated or contradictory, or
if remote evidence is unavailable or stale, mark the evidence insufficient.
An absent or empty provider version in the approved event or fresh adapter read
is insufficient even when the field values match; it always forces `defer`.
The only permitted disposition is `defer`; do not infer a remote value from a
baseline, title, earlier event, or likely intent. Recovery is a fresh
`planar sync pull <kind:id> --json` followed by status and audit inspection.

### 4. Dispatch sync-reconciler for conflicts

For each conflict, dispatch one fresh `sync-reconciler` specialist through the
host's agent dispatch mechanism with the canonical
[`agents/sync-reconciler.md`](../../agents/sync-reconciler.md) instructions.
Supply a read-only envelope for each conflict:

```json
{
  "scope": "<resolved-scope>",
  "event_id": 15,
  "entity": "task:42",
  "link_id": 7,
  "external_id": "PROJ-1234",
  "status": {},
  "local_entity": {},
  "conflict_event": {},
  "preceding_sync_events": [],
  "field_evidence": []
}
```

The reconciler must recommend exactly one disposition:
`keep-local`, `keep-remote`, `manual-merge`, or `defer`. Reject a synonym,
additional disposition, missing rationale, missing field evidence, or proposed
direct local/remote mutation. The specialist is read-and-recommend by default;
the caller owns the operator gate and the supported resolve command.

### 5. Gate each exact disposition

Before any mutation, show the operator:

- resolved scope, event ID, entity, link, external system and external ID;
- one row per conflicting field with local and remote values plus provenance;
- raw event detail and relevant audit evidence;
- exactly one recommendation and rationale;
- the whole-entity effect; and
- the exact proposed command, or the exact recovery command for `defer`.

Wait for explicit confirmation naming the event ID and the recommended
disposition. Approval is event-specific and evidence-specific. Silence,
general permission to reconcile, approval of evidence collection, approval of
another event, or an earlier approval before evidence changed is not consent.
For multiple events, require an explicit decision for each event; never widen
one approval to the remaining set.

| Confirmed disposition | Allowed effect |
|---|---|
| `keep-local` | Run only `planar sync resolve <event-id> --keep local ... --json`; this pushes the complete current local entity. |
| `keep-remote` | Run only `planar sync resolve <event-id> --keep remote ... --json`; this pulls the complete observed remote entity into local state. |
| `manual-merge` | Run no resolve command yet; follow the separate two-gate recipe below. |
| `defer`, decline, or postpone | Write nothing and retain the evidence plus recovery command. |

Immediately before an approved resolve, re-read entity status and link audit.
Require the same link to remain conflicted, the approved event to remain the
latest unresolved conflict, and the evidence to remain unchanged. Otherwise
invalidate approval and present a fresh preview.

Pass the exact approved `evidence.token` and the reviewed local entity
`updated_at` through the required compare-and-swap flags. A stale event, changed
token, changed local version, or fresh remote value mismatch is a conflict
result and performs no resolution write.

These checks cannot eliminate the unavoidable provider GET→write race when
the provider has no conditional write primitive: remote state can change after
the adapter's final GET and before its write. Include this limitation in the
preview and result for every provider write; evidence freshness is a safety
check, not a promise of provider-side atomicity.

### 6. Manual merge uses two gates

`manual-merge` is not a CLI resolve mode and does not authorize an entity edit:

1. The reconciler displays both values and proposes the desired merged local
   value. This is gate one: approval of the proposal only; perform no mutation.
2. The operator edits the entity through its normal guarded
   `planar <kind> ...` mutation workflow. This skill and the reconciler do not
   synthesize or execute that edit.
3. Read `planar <kind> show <id> --json` and display the exact local post-state.
4. Stop for gate two: require a new confirmation naming the event ID and
   `keep-local`, after reviewing that post-state.
5. Revalidate current conflict evidence, then run only
   `planar sync resolve <event-id> --keep local ... --json` and verify it.

Approval of merge text is not approval to edit. Approval of the local edit is
not approval to push. Without the second gate, return a pending manual merge or
`defer` with zero resolutions applied.

### 7. Verify every applied resolution

For each confirmed `keep-local` or `keep-remote` call, require the JSON result
to report `ok=true`, the approved `event_id`, the chosen `keep`, and a
`new_event_id`. Then re-run:

```text
planar sync status --entity <kind:id> --json
planar audit trail --link <link-id> --json
planar <kind> show <id> --json
```

Success requires the link status to be `ok`, the new audit event to exist with
the expected push/pull direction, successful outcome, and
`resolved=<side>; from sync_event=<id>` detail, and local post-state to match
the whole-entity effect. A successful command exit without those reads is
`outcome=error` for a single target or `outcome=partial` after earlier
independent resolutions succeeded.

After ambiguous output or any resolution failure, never retry blindly. The
conflict event is append-only and `sync resolve` is not an idempotency key.
Inspect audit, sync status, and entity post-state first. Retry only if the link
is still conflicted, no resolution event referencing that conflict was
recorded, fresh evidence has been rebuilt, and the operator has approved it
again.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` over sync targets, plus
separate `conflicts_found`, `reconciler_dispatches`, and
`resolutions_applied` counts. One confirmed event resolved and verified is one
applied reconciliation target. Deferred, declined, and pending manual merges
are skipped with zero mutation. A failed target does not erase successful
independent targets.

## Result

Always return `outcome=ok|partial|error` and the requested operation, resolved
scope, filters, target rows, conflict event IDs, field evidence, dispositions,
confirmation state, executed commands, new resolution event IDs, and verified
status/audit/entity post-state.

A no-conflict run is `outcome=ok`, zero reconciler dispatches, zero resolutions,
and no warning. If some independently approved targets verify and another
fails, return `partial`, retain per-target results, and never claim rollback.
In `--json` mode mirror Context, Intent, Actions, Result, Warnings, Next actions,
and Recovery with stable keys and arrays. In text mode omit empty sections
except Result.

## Warnings

Report unavailable or contradictory evidence, changed evidence that invalidated
approval, adapter/dispatch/status failures, pending manual merges, ambiguous
resolve output, partial application, or failed verification. Do not warn for a
verified empty or conflict-free result. Never describe direct remote writes or
per-field resolution as supported behavior.

## Next actions

Give zero to three executable recommendations. Prefer the narrowest relevant
read first:

```text
planar sync status --entity <kind:id> --json
planar audit trail --link <link-id> --json
planar sync pull <kind:id> --json
```

When approval is pending, name the exact event ID and disposition needed rather
than suggesting the mutation as though it were already authorized.

## Recovery

For insufficient evidence, run a fresh guarded pull, then repeat status and
audit inspection. For a pull, push, or dispatch failure, retry only the failed
target and preserve verified successes. For ambiguous output or any failed
resolution, first run audit for the link, status for the entity, and the
entity's `show ... --json` post-state; if a new event already references the
conflict, verify rather than retry. If the link remains conflicted with no such
event, return to the evidence preview and obtain fresh operator confirmation
before the exact `sync resolve` retry.

Never promise cross-remote rollback, silently reuse approval, or recommend a
blanket database or working-tree reset.

## Source and render rules

This file under `skills/src/` is the only authored source. Do not edit generated
Claude, Codex, or Copilot projections directly. Rendering and drift
verification of vendor projections is owned by scriptorium (the stack's
render tool, driven by `scriptorium.yaml`), not by a planar CLI verb.

## Vendor Notes

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine (armarium orchestration layer).
