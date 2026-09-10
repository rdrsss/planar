---
description: Propagate a feature (anchor plan + descendants) to a registered external operational system (Jira or GitHub Issues).
origin: agents/ext-sync.md
shared_notes:
    - Feature propagation runs through the registered external-system adapter; no direct remote writes happen outside the CLI contract.
slug: pl-ext-propagate
vendor:
    claude:
        argument_hint: <plan> [--system <slug>] [--dry-run]
        invocation_examples: |
            /pl-ext-propagate checkout-rewrite
            /pl-ext-propagate 42 --system my-jira
            /pl-ext-propagate checkout-rewrite --dry-run
---

# Ext-sync Propagate ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `ext-sync` agent. See [`agents/ext-sync.md`](../../agents/ext-sync.md) for the full role spec, strategy-selection contract, and idempotency invariant.

> **Implementation status (plan 996).** The whole-feature walk this skill
> describes — `ext propagate <plan>` — is **not yet implemented on either
> binary**. `ext`/`sync` moved from `planar` to `planar-ext` at task 6419;
> the tree-walking `propagate` verb itself is tracked separately (task 6421,
> in progress). What IS live today is the entity-level
> `planar-ext ext propagate-one <system> --from <kind:id>` — one counterpart
> per call, no tree walk, no strategy cache. Use it directly for a single
> plan or task until whole-feature propagation lands.

## What propagation does

Propagation walks the feature tree anchored at the given plan — top-down — and creates external counterparts for every entity that is not yet linked:

- **Jira:** anchor plan → Epic, child plans → Stories (with Epic Link), tasks → Sub-tasks, decisions → comments on the Epic.
- **GitHub Issues (single-repo):** anchor plan → parent issue, child plans and tasks → sub-issues.

Multi-repo GitHub propagation (the `projects-v2` strategy, a GitHub Projects
board mirroring the feature tree) is permanently cut — decision 1001. It is
not deferred work; it will not exist. Do not describe it as a future
capability.

One `external_links(link_role='mirror')` row is recorded per created entity. One `sync_events(outcome='ok')` row is recorded per Create call.

Before remote writes begin, propagation checks claim-aware plan state. If another live claim owns the anchor plan or a descendant being propagated, the run stops and reports the conflict. When the agent activity claim surface is available, propagation acquires a plan-level propagation claim, heartbeats it during long remote calls, and releases it as `completed` or `aborted` at the end.

## Per-feature strategy selection (ADR-0006)

The strategy is selected once at first propagation and cached on `external_links.config_json` of the anchor plan.

| System | Strategy |
|--------|---------|
| Jira | `jira-epic` (always) |
| GitHub Issues | `parent-issue` (always) |

There is no repo-count-based strategy selection: GitHub systems always use
the single-repo `parent-issue` strategy. The multi-repo `projects-v2`
strategy named in earlier revisions of this skill does not exist — see the
status note above.

See `docs/architecture.md` for the strategy-selection contract.

## Idempotent rerun guarantee

Entities that already have an `external_links(link_role='mirror')` row for the target system are skipped without error. Running propagation twice produces no duplicate external counterparts and no duplicate DB rows.

**Strategy stickiness:** the chosen strategy is cached on `external_links.config_json` of the anchor plan at first propagation. Subsequent reruns honor the cached strategy. To force fresh selection, use `--restrategize`.

**Partial-failure resumability:** mid-tree failures write `sync_events(outcome='partial')` and propagation continues. A rerun skips already-linked entities and resumes from the failure point.

## Flags

| Flag | Effect |
|------|--------|
| `--system <slug>` | Target a specific external system by slug (defaults to first registered system). |
| `--dry-run` | Print what would be created without contacting the remote. |
| `--restrategize` | Force fresh strategy detection. Prompts for confirmation when strategy changes; abandoned counterparts write `sync_events(outcome='strategy-abandoned')`. |
| `--yes` | Auto-confirm the `--restrategize` prompt (no interactive input). |
| `--verify-counterparts` | Probe the remote to confirm existing counterparts still exist. Missing ones write `sync_events(outcome='counterpart-missing')` and are reported as `Missing`. |
| `--unlink` | Remove `external_links` rows for missing counterparts (requires `--verify-counterparts`). |
| `--recreate` | Remove link and immediately re-create missing counterparts (requires `--verify-counterparts`). |
| `--sync <direction>` | Sync direction applied to every `external_links` row created by this propagation. Accepted values: `read-only`, `write-back`, `two-way`. Default: `read-only`. **Behavior change from prior versions:** the propagate flow previously defaulted to `two-way`; users with downstream tooling that depended on the implicit two-way write must pass `--sync two-way` explicitly going forward. |

## What it does not do

- Does **not** modify the FS workbench. After propagation, run `planar workbench push <plan>` to update front matter with external keys.
- Does **not** run automatically on a watcher or cron.
- Does **not** re-evaluate strategy automatically if the repo count changes (use `--restrategize`).
- Does **not** delete remote counterparts during `--restrategize` — Planar stops tracking them; the user cleans them up manually.

## Underlying CLI verbs

There is no lossless current command for changing an existing external link's
direction. The `links update` subcommand is deferred, and `links add` /
`links remove` manage internal `entity_links`, not external bindings. Top-level
`unlink` / `link` is a destructive recovery: it deletes the old `external_url`,
`config_json` (including cached propagation strategy), last-sync state, and
association with its sync-event history. The old events remain detached with
`link_id=null`; the replacement gets a new row id, null URL/config/last-sync,
status `never`, and no attached history.

Before any unlink, save the CLI-visible evidence:

```sh
planar audit trail --link <link-id> --json > external-link-<link-id>-audit.json
planar-ext sync status --entity <kind:id> --system <system-slug> --json > external-link-<link-id>-status.json
```

Those reads capture identity, event history, and last-sync state, but the
public CLI does not expose the exact old URL, config, role, or direction. Stop
unless the destructive loss is acceptable and the intended role/direction are
known independently. For a record-only binding, review the full replacement
command first, then run top-level `planar unlink <link-id>` and `planar link
<kind:id> --to <system-slug>:<external-id> --role <role> --sync <direction>`.
This retains the same remote id but does not restore the omitted fields.

For a propagation-owned mirror, validate the plan/system with a dry run before
unlinking. After unlink, dry-run again to preview fresh creation, then propagate
with an explicit sync direction. This creates a new remote counterpart and new
state; it does not restore the deleted row. See the complete recovery sequence in
[`docs/cli-reference.md`](../../docs/cli-reference.md#planar-links-update-link-id).

> **Cross-scope guard.** This verb refuses with exit 5 when the
> operator's resolved write scope disagrees with the target entity's
> stored scope. Run from inside the entity's owning repo, pass
> `--scope <slug>` explicitly, or `cd` into that repo — there is no
> flag that downgrades the refusal to a warning; a genuine mismatch fails
> outright. See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar-ext ext propagate <plan>              # NOT YET IMPLEMENTED — see status note above
planar-ext ext propagate <plan> --system <slug>
planar-ext ext propagate <plan> --dry-run
planar-ext ext propagate <plan> --restrategize [--yes]
planar-ext ext propagate <plan> --sync read-only|write-back|two-way
planar-ext ext propagate <plan> --verify-counterparts [--unlink | --recreate]
planar-ext ext propagate-one <system> --from <kind:id>   # live today
planar link <kind:id> --to <system-slug>:<external-id> --propagate
planar unlink <link-id>
planar link <kind:id> --to <system-slug>:<external-id> --role <role> --sync read-only|write-back|two-way
```

See [`docs/cli-reference.md`](../../docs/cli-reference.md) for the full command grammar.

## Context

Report the resolved scope, anchor plan, external system, selected or cached
strategy, sync direction, dry-run or apply mode, and counterpart-verification
or restrategize options.

## Intent

State in one sentence which feature tree will be previewed, propagated,
verified, restrategized, unlinked, or recreated.

## Actions

Report `attempted`, `applied` (the succeeded count), `skipped`, and `failed` for every entity target.
Retain the propagation result's created, existing, missing, unlinked, and
recreated distinctions, and list every failed local identity with system slug
and remote failure evidence. A dry run applies zero; already-linked entities
are skips.

## Result

Always report `outcome=ok|partial|error`. Return the anchor plan, strategy,
system, completed entity-to-link/URL mappings, missing counterparts, and the
latest sync-event evidence. Verify persisted links with `planar-ext sync status
--entity <kind:id> --system <system-slug> --json` where supported. A fully
idempotent rerun is `outcome=ok` with zero applied and the existing links.

## Warnings

Preserve the `--restrategize` confirmation gate and require the existing
confirmation semantics before changing strategy. Name claim conflicts,
missing counterparts, destructive unlink/recreate implications, unavailable
post-state, and partial remote results. Never imply atomicity or rollback
across independent remote calls.

## Next actions

Give zero to three executable recommendations. A dry run leads with the exact
approved apply command; missing counterparts lead with their audit/status read
before any separately confirmed `--unlink` or `--recreate` action.

## Recovery

For each failed entity, provide its `planar-ext sync status --entity <kind:id>
--system <system-slug> --json` inspection and the exact idempotent `planar-ext ext
propagate <plan> --system <slug> ...` retry preserving strategy, sync, scope,
and verification flags. Successful targets remain linked and the retry skips
them; do not prescribe a cross-target undo.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
