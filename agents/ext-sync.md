---
name: ext-sync
description: Propagates a feature (anchor plan + descendants) to a registered operational system (Jira or GitHub Issues). Triggered explicitly — never on a watcher.
tier: large
role: ext-sync
capability: coordinate
---

# Ext-sync

Given a feature anchor plan id, pushes the feature tree to a registered external operational system by creating external counterparts (Epic/Story/Sub-task on Jira; parent issue/sub-issues on GitHub Issues) and recording `external_links` rows for each created entity.

Vendor-neutral. Vendor-specific surfaces are under `commands/claude/pl-ext-propagate.md`, `skills/codex/pl-ext-propagate.md`, and `skills/copilot/pl-ext-propagate.md`.

## Tier

`large`. Resolved to a concrete model per [`agents/models.md`](models.md). Top-down tree traversal with per-entity decisions (skip vs create) and per-system strategy selection requires the same level of judgment as orchestration.

## When to use

- After `pl-spec-ingest` has decomposed a feature's documents into child plans and tasks and the user wants to propagate the feature to an external system.
- When the user explicitly invokes `/pl-ext-propagate <plan>`.
- When the user runs `planar link --propagate` and the linked entity belongs to a top-level plan.
- Never invoked automatically on a watcher or cron trigger.

## Inputs

- A feature anchor plan id (or slug). Required.
- The system slug to push to (defaults to the association's primary registered system, i.e. the first registered system in the database).
- The strategy override (defaults to ADR-0006 per-feature detection: single-repo → parent-issue + sub-issues on GitHub, multi-repo → Projects v2, zero-repo → parent-issue in `github_lead_repo`, Jira → always epic-hierarchy).

## Outputs

- External counterparts in the target system per the operational-plane adapter shape described in [`docs/architecture.md`](../docs/architecture.md):
  - **Jira:** anchor plan → Epic, child plans → Stories (with Epic Link set), tasks → Sub-tasks, decisions → comments on the Epic.
  - **GitHub Issues (single-repo):** anchor plan → parent issue, child plans → sub-issues, tasks → sub-issues. (Phase B)
  - **GitHub Projects v2 (multi-repo):** anchor plan → Project, child plans / tasks → issues in their respective repos attached to the Project. (Phase B)
- One `external_links(link_role='mirror')` row per local entity per target system.
- One `sync_events(outcome='ok')` row per successful Create call.
- A summary report listing created, skipped, and failed entities.

## Boundaries

- **Task→plan and plan→plan attachments:** the engine walks descendant tasks via both `tasks.plan_id` (the canonical FK, set by `planar task add --plan <id>`) and `entity_links(task→plan, derives-from)` (written by the ingestor alongside the FK). Child plans are walked via `plans.parent_plan_id` alone. A task with both attachment paths is counted once; a child plan with `parent_plan_id` but no entity_link still surfaces. The consolidated walker lives in `src/internal/extsync/tree.go`; new propagation code should reuse those helpers rather than embed its own CTE.
- **Local-project → GitHub-owner/repo resolution:** the propagate flow reads `projects.git_remote` first (parsed by `parseGitHubRepo` in `src/internal/extsync/github_remote.go`) and falls back to splitting `projects.slug` as `owner/repo` for fixtures and manually-registered projects without a remote URL. Real `planar init`-registered projects always have `git_remote` populated.
- **Sync-direction control:** the propagate flow defaults to `external_links.sync_direction='read-only'`. This is a behavior change from prior versions where the propagate flow defaulted to `'two-way'`; users with downstream tooling that depended on the implicit two-way write must pass `--sync two-way` explicitly going forward. Users opt into write-back or two-way at creation via `planar ext propagate --sync <direction>` (the flag in `src/cmd/planar/cmd_ext.go` plumbs through `PropagateOpts.SyncDirection` to every row insertion; see `resolveSyncDirection` in `src/internal/extsync/engine.go`). Users who already propagated can change `sync_direction` on an existing row post-hoc via `planar links update <link-id> --sync <direction>` (calls `external.UpdateLinkSyncDirection` in `src/internal/external/external.go`, which atomically updates the column and writes a `sync_events` audit row). No auto-push behavior on local task/plan updates — `planar sync push` remains a manual command.
- **Top-down creation order:** anchor plan → child plans → tasks → test scenarios. Each level is skipped if already linked (idempotent).
- **Idempotent on rerun:** entities that already have a `external_links(link_role='mirror')` row for the target system are skipped without error.
- **Strategy stickiness:** strategy is selected at first propagation and cached on `external_links.config_json` of the anchor plan under the key `"strategy"`. Rerun honors the cached strategy even if the repo count later changes. This preserves stable external URLs the team has already shared. Use `planar ext propagate --restrategize` to force fresh selection.
- **First-propagation override (ADR-0006):** `--github-strategy <value>` overrides ADR-0006 auto-detection at first propagation for GitHub systems. Accepted values are `parent-issue`, `projects-v2`, and `tracking-issue` (the three GitHub strategy kinds). The override is cached on `external_links.config_json` identically to auto-detected strategies; subsequent propagations honor the cached value unless `--restrategize` is set. Mutually exclusive with `--restrategize`; rejected for Jira systems. The selector (`SelectStrategy` in `src/internal/extsync/selector.go`) is bypassed when the override is in effect; the per-strategy handler dispatch is unchanged.
- **On failure mid-tree:** the failed entity is recorded with `Op="failed"` and a `sync_events(outcome='partial')` row is written for audit. Propagation continues to the next entity. Idempotent skip-if-already-linked means rerun resumes from the failure point (partial-failure resumability).
- **--restrategize abandonment:** when the fresh strategy differs from the cache, the user is prompted to confirm (or `--yes` bypasses). On confirmation, prior counterparts are NOT deleted from the remote — Planar abandons tracking them and writes `sync_events(outcome='strategy-abandoned')` per counterpart for audit. Fresh propagation then proceeds under the new strategy.
- **--verify-counterparts probe:** when set, the engine probes the remote for every already-linked entity. Entities confirmed present are "verified". Entities returning 404 are reported as "missing" with `sync_events(outcome='counterpart-missing')`. Combine with `--unlink` (remove tracking link) or `--recreate` (remove link and re-create counterpart). Off by default — probing on every run is expensive on large features.
- **Never modifies the FS workbench directly.** After propagation, the user (or `pl-orchestrator`) runs `planar workbench push <plan>` separately to update front matter with external keys.

## Strategy selection (ADR-0006)

The ext-sync engine selects a propagation strategy per feature at first propagation by counting distinct repos touched by the feature's descendant tasks:

| System | Repo count | Strategy |
|--------|-----------|---------|
| Jira | any | `jira-epic` (always) |
| GitHub Issues | 1 | `github-parent-issue` (Phase B) |
| GitHub Issues | ≥ 2 | `github-projects-v2` (Phase B) |
| GitHub Issues | 0 | `github-zero-repo` (Phase B; requires `github_lead_repo` in config) |

Detection is read-only against the local DB; no remote calls are made to determine strategy.

## Behavior

1. Resolve the anchor plan via `planar ext propagate <plan>`.
2. Check claim-aware plan state before propagation. If another vendor owns an active claim on the anchor or a descendant currently being propagated, stop and surface the conflict rather than creating remote counterparts from stale local assumptions.
3. Acquire a propagation claim on the anchor plan for the duration of the run when the agent activity claim surface is available. Heartbeat during long remote calls and release as `completed` or `aborted` at the end.
4. Resolve the target external system (from `--system` flag or first registered system).
5. Select the propagation strategy (ADR-0006 detection or cached value).
6. For each entity in top-down order, check for an existing `external_links(link_role='mirror')` row for the target system.
   - If found: record as `skipped`.
   - If not found: render the template payload, call `adapter.Create`, record `external_links` + `sync_events` in a single transaction, record as `created`.
7. For Jira: post anchor-level decisions as comments on the Epic.
8. Print the propagation summary.

## Non-trivial task scenarios

Test scenarios that derive from tasks are propagated as Stories with a `test-scenario` label (Jira) or as issues with the `test-scenario` label (GitHub). This is the only label-based semantic in any default strategy.

## CLI commands composed

```
planar ext propagate <plan>
planar ext propagate <plan> --system <slug>
planar ext propagate <plan> --dry-run
planar ext propagate <plan> --restrategize [--yes]
planar ext propagate <plan> --sync <read-only|write-back|two-way>
planar ext propagate <plan> --verify-counterparts [--unlink | --recreate]
planar link <kind:id> --to <system-slug>:<external-id> --propagate
planar links update <link-id> --sync <read-only|write-back|two-way>
```
