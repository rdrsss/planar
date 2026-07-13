---
slug: pl-observe
description: "Observe one plan's live operational activity: action topology, claims, recent failures, sync events, and handoffs."
source: docs/cli-reference.md#binary-planar-watch
vendor:
  claude:
    argument_hint: "--plan <id> [--since <RFC3339>] [--limit <n>] [--vendor <vendor>] [--system <slug>]"
    invocation_examples: |
      /pl-observe --plan 808
      /pl-observe --plan 808 --since 2026-07-13T00:00:00Z --limit 50
      /pl-observe --plan 808 --vendor codex --system github
shared_notes:
  - "This workflow is strictly read-only: use planar and planar-watch read surfaces only; never mutate planning, claim, handoff, or sync state."
  - "An observed empty collection is different from unavailable telemetry; preserve that distinction in every result."
---

# Planar Observe ({{.VendorTitle}})

Answer “what has been happening on this plan?” from timestamped operational
evidence. This is a plan-focused activity view, not the scope and next-work
summary provided by `pl-status`.

## Context

Require `--plan <id>`. Accept these observation filters:

| Filter | Meaning | Default |
|---|---|---|
| `--since <RFC3339>` | Inclusive lower timestamp for recent feed and sync events; also apply it locally to handoffs. | 24 hours before observation time |
| `--limit <n>` | Positive source-query cap. For actions, feed, and sync events this bounds global rows before all requested filtering is complete. | 100 |
| `--vendor <vendor>` | Narrow actions, claims, failures, and handoffs to one vendor. | all vendors |
| `--system <slug>` | Narrow sync events to one registered external system. | all systems |

Reject a missing/invalid plan id, malformed RFC3339 timestamp, or non-positive
limit before reading the activity ledgers. Record the observation timestamp in
UTC. Parse the validated effective `since` value as an instant and derive a
canonical UTC comparison token with exactly three fractional digits, matching
the millisecond-width SQLite `%f` timestamps stored by `sync_events` (for
example, `2026-07-13T02:00:00-04:00` becomes
`2026-07-13T06:00:00.000Z`). If the input instant has non-zero precision below
a millisecond, round the comparison token up to the next millisecond so the
inclusive lower bound cannot admit an older stored timestamp. Echo this
canonical value as the effective `since` filter. Filters are skill inputs:
pass them only to underlying verbs whose schema exposes the corresponding
flag.

Resolve cwd scope with `planar scope show --json`, verify the target with
`planar plan show <plan-id> --json`, then read
`planar dashboard --agents --json` and select that plan's roll-up, claims, and
next-available count. The dashboard has no `--plan` flag; do not invent one.
Compare the plan's `scope_kind`/`scope_id` with the resolved scope. On mismatch,
stop with `outcome=error` and ask the operator to run from that project. A
verified draft, completed, or otherwise quiet plan may be absent from the
in-flight dashboard; treat that as an observed zero in-flight roll-up, not as
missing telemetry or a scope mismatch.

## Intent

State in one sentence that the workflow will observe the selected plan and
time window without changing planning or coordination state.

## Actions

Use JSON internally and issue only these read commands, adding the effective
filters where shown:

```text
planar scope show --json
planar plan show <plan-id> --json
planar dashboard --agents --json
planar-watch actions --plan <plan-id> [--vendor <vendor>] --limit <n> --json
planar-watch ps --plan <plan-id> [--vendor <vendor>] --stale --json
planar-watch feed --plan <plan-id> [--vendor <vendor>] --since <since-utc> --limit <n> --json
planar-watch sync-events --plan <plan-id> [--system <slug>] --since <since-utc> --limit <n> --json
planar handoff list --status pending,validated,consumed,abandoned --json
```

Pass the same canonical UTC `since-utc` value to both `feed` and
`sync-events`. Do not pass through the caller's original numeric-offset text:
`sync-events` currently compares its stored UTC timestamps lexically, so the
normalized fixed-width millisecond `Z` representation is required to preserve
instant ordering and include a row exactly on the requested boundary.

Plan matching differs by source. For `actions`, `ps`, and `feed`, `--plan`
matches rows attached to the plan itself, a task whose `plan_id` is the target,
or a plan step whose `plan_id` is the target. It does not traverse child plans
or infer ownership for actions on questions, scenarios, artifacts, or
decisions. For `sync-events`, `--plan` is narrower: it matches only an event
whose `link_id` resolves to an `external_links` row directly attached to that
plan (`entity_kind=plan`, `entity_id=<plan-id>`). A sync event linked to a task
or plan step on the plan is therefore outside this command's `--plan` result.
Handoffs have no command-level plan filter and use the separate attribution
rules below.

`feed --json` and `handoff list --json` emit NDJSON; parse each non-empty line
as one object and do not interpret an empty stream as a parse failure.
`sync-events --json` instead emits one JSON envelope with `generated_at` and a
`sync_events` array; parse the complete output once and read event rows from
that array. A successfully parsed envelope with `sync_events: []` is an empty
bounded sample, not a parse failure. The other commands return JSON objects or
arrays as documented. After parsing the feed, enforce the requested inclusive
lower bound locally: retain only rows whose top-level `at` is greater than or
equal to the effective `--since` instant, and count older rows as skipped.
Parse both RFC3339 values as instants rather than comparing their text, so an
input with a numeric UTC offset is handled correctly. This instant-aware local
filter remains required even though the command receives normalized UTC text,
because the feed's initial snapshot currently uses the epoch as its query
watermark when `--since` is supplied.

The actions, feed, and sync-events handlers apply `--limit` to their global
source rows before all requested filtering is complete. Treat their outputs as
bounded samples, not complete filtered result sets, even when fewer than `<n>`
matching rows or zero matching rows are returned. If a sampled command returns
exactly `<n>` rows, report that its output cap was reached. Fewer than `<n>`
rows does not prove the global source was unsaturated, because plan/vendor,
time, or system filtering may occur after a source query has already reached
its cap.

Build the snapshot as follows:

1. **Action tree.** Use the plan-matching `actions` rows returned from the
   bounded recent global sample. Join each row to its parent through
   `parent_action_id`, render roots oldest-first and children beneath them, and
   retain action id, kind, entity, vendor/role, `started_at`, `ended_at`,
   `outcome`, and summary. A parent absent from the sample is an explicit
   truncated root, not evidence that the action was originally root.
   `planar-watch tree` is not a substitute because its schema has no plan
   filter.
2. **Claims.** Normalize `ps --stale` by claim identity (`id`) before
   rendering. The handler's `active` query includes every row whose stored
   status is active, while its `stale` query also includes lease-expired active
   rows, so the same claim can appear in both arrays. De-duplicate the union by
   id and classify each unique claim exactly once at the observation timestamp:
   explicit `status=stale` or `lease_expires_at < observed_at` belongs only in
   stale; an unexpired `status=active` row belongs only in active. Render entity,
   vendor/role, claim-token prefix, latest action summary/time, claimed time,
   last heartbeat, and lease expiry. Report distinct unique active and stale
   counts; never count or render an expired active row as active.
3. **Recent failures.** First apply the effective inclusive `--since` bound
   locally to every plan-matching feed row using its top-level `at`. From the
   surviving rows in the bounded recent global sample, retain terminal
   `failed` events and `action_ended` events whose action outcome is `error`,
   `aborted`, or `timeout`. Show the event timestamp, entity, action/claim
   identity, outcome, and available summary or release reason. Do not relabel
   ordinary releases, successful completions, or blocked tasks as failures,
   and do not claim the sample contains every failure in the requested
   interval.
4. **Sync events.** Render the matching rows from the bounded recent global
   sample with event id, `at`, link id, direction, outcome, changed fields, and
   detail. Preserve `conflict`, `error`, `noop`, and successful outcomes rather
   than flattening them; do not claim the sample is the complete filtered
   interval.
5. **Handoffs.** `handoff list` is global and defaults to `pending`; always pass
   the explicit comma-separated status filter
   `--status pending,validated,consumed,abandoned` so the observation covers
   every supported handoff status. Although `--vendor` appears as an inherited
   flag, the list handler does not apply it. Never pass it for filtering.
   Instead, when `--vendor <vendor>` was requested, retain a row locally only
   when `from_vendor == <vendor>` or non-null `to_vendor == <vendor>`; this is
   endpoint participation semantics, not source-vendor-only semantics. Then
   apply `--since` locally to `created_at` (the one timestamp present for every
   status) and apply the limit after vendor/time/plan attribution filtering.
   The current handoff-list JSON exposes `from_snapshot_id` but no task id, and
   no current public read resolves a snapshot to its task. Consequently every
   returned row that survives local vendor/time filtering is `unavailable for
   plan attribution` in this cycle: do not include it in the plan's observed
   handoff count or guess from worktree/branch text. Still report its handoff
   id, status, both vendor endpoints, timestamps, and the explicit attribution
   limitation in the unavailable-row detail.

Count every attempted read. For this read-only workflow, `applied` means a
successful, parsed observation read; it never means a write. Report
`attempted`, `applied`, `skipped`, and `failed`. Count the single handoff-list
command as one attempted read and, when parsed, one applied observation read.
Count feed rows removed by the local `at >= since` filter, handoff rows removed
by local vendor/time filtering, and rows unavailable for plan attribution as
skipped rows, reported separately; there is no public snapshot-to-task read,
so do not manufacture per-handoff lookup attempts. Do not call
`planar-agent`, `planar task done`, handoff lifecycle verbs, sync mutation
verbs, or any other planning/coordination write.

## Result

Always set `outcome=ok|partial|error` and include:

- plan id/title and observation timestamp;
- effective `since`, limit, vendor, and system filters;
- dashboard in-flight counts for orientation, without reproducing its scope
  summary or recommending next work;
- the action tree, active/stale claims, recent failures, sync events, and
  plan-attributed handoffs, each with source timestamps;
- per-section matching row counts and whether the source is an authoritative
  current-state read, a bounded global sample, or unavailable/partially
  attributable telemetry.

An `ok` result requires every required source to have been read and parsed. A
failed source makes the result `partial` when other evidence is usable, or
`error` when target verification/dashboard failed or no operational evidence
source was available.

### Empty, Sampled, Versus Unavailable Telemetry

Declare `observed empty` only for a successful read whose underlying source is
authoritative for the reported set. In this workflow, the normalized `ps`
union is authoritative for current claims: zero unique active and zero unique
stale claims may be reported as `current claims: observed empty at
<observed_at>`. The dashboard's absence of a verified quiet plan is likewise
authoritative only for its documented current in-flight roll-up.

Actions, recent failures, and sync events are bounded global samples. Zero
matching rows must be reported as `no matching rows in bounded sample`, with
the requested `since`, the source limit, and the returned matching count. It
must not be called `observed empty`, `no activity`, a complete interval, or
evidence that no matching row exists outside the sample. A result with zero
matches in all three sampled sections may be described as quiet in the sampled
window only if that limitation is stated in the same sentence.

For a command failure, invalid JSON/NDJSON, schema mismatch, or handoff row
whose plan ownership cannot be verified, say `unavailable`, name the source,
and do not report zero for that row. A successfully read handoff stream can
therefore report an observed-empty attributable set and a nonzero
unavailable-for-attribution row count at the same time; that is not a fully
observed empty handoff section. A section may report both an observed count and
a separate unavailable-row count. Never infer “no activity” from unavailable
telemetry. Because three activity sections are sampled and non-empty handoff
rows cannot currently be attributed to a plan, never claim that all five
activity sections or the complete requested interval were authoritatively
observed empty.

## Warnings

Report failed or partially attributable sources, stale claims, failure events,
sync conflicts/errors, missing parents caused by sampling, and the bounded
global-sample limitation for actions, failures, and sync events. Do not warn
merely because an authoritative current-state section is empty. When
summarizing plan coverage, preserve the per-source distinction: actions,
claims, and feed failures include the plan's direct tasks and plan steps,
whereas sync events require a direct plan external link. Neither behavior
recursively includes descendant plans.

## Next Actions

Give zero to three read-only drill-down commands selected from the evidence:

```text
planar-watch log --claim <claim-token> --limit <n> --json
planar-watch log --task <task-id> --limit <n> --json
planar-watch sync-events --entity <kind:id> --since <RFC3339> --json
planar handoff show <handoff-id> --json
```

Do not recommend task selection, claiming, reconciliation, handoff transitions,
or sync resolution from this workflow. Route “what should I do next?” to
`pl-status`; route a requested mutation to the dedicated workflow and its
operator gate.

## Recovery

For every unavailable source, provide its exact idempotent read command from
the Actions section. For malformed filter input, provide the corrected
`/pl-observe --plan <id> --since <RFC3339> --limit <n>` invocation. After a
schema-version mismatch, inspect with `planar health --json`, then retry the
same observation command. Omit Recovery when every source read succeeded and
no unavailable telemetry needs a retry; bounded sampling is an explicit
limitation, not a failed read.

## Boundaries

- Read through `planar` and `planar-watch` only; never open SQLite or infer
  operational state from workbench/session files.
- Do not duplicate `pl-status` scope orientation, attention ordering,
  claim-aware next-work selection, or planning recommendations.
- Do not contact Jira or GitHub. Sync-event rows are locally recorded
  telemetry.
- Do not follow streams in the combined snapshot. For continuous low-level
  monitoring, hand the operator an exact `planar-watch ... --follow` command
  supported by that individual verb.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```text
{{.InvocationBlock -}}
```
{{- end}}
