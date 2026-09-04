---
description: Propagates a feature (anchor plan + descendants) to a registered operational system (Jira or GitHub Issues). Triggered explicitly — never on a watcher.
kind: agent
slug: ext-sync
---

# Ext-sync

Given a feature anchor plan id, pushes the feature tree to a registered external operational system by creating external counterparts (Epic/Story/Sub-task on Jira; parent issue/sub-issues on GitHub Issues) and recording `external_links` rows for each created entity.

**Current implementation status (plan 996):** the `ext`/`sync` verb family lives on the `planar-ext` binary (moved from `planar` at task 6419). The whole-feature walk this role describes — `ext propagate <plan>` — is **not yet implemented on either binary**; it is tracked separately (task 6421, in progress at time of writing). The entity-level equivalent that IS live today is `planar-ext ext propagate-one <system> --from <kind:id>`, which creates one external counterpart per call and is not tree-aware. Treat the `ext propagate <plan>` command forms below as the intended future surface on `planar-ext`, not a currently invokable command.

Vendor-neutral. Vendor-specific surfaces are under `commands/claude/pl-ext-propagate.md`, `skills/codex/pl-ext-propagate.md`, and `skills/copilot/pl-ext-propagate.md`.

## Tier

`large`. Resolved to a concrete model per the Tier Table in `agents/models.md`. Top-down tree traversal with per-entity decisions (skip vs create) and per-system strategy selection requires the same level of judgment as orchestration.

## When to use

- After `pl-spec-ingest` has decomposed a feature's documents into child plans and tasks and the user wants to propagate the feature to an external system.
- When the user explicitly invokes `/pl-ext-propagate <plan>`.
- When the user runs `planar link --propagate` and the linked entity belongs to a top-level plan.
- Never invoked automatically on a watcher or cron trigger.

## Inputs

- A feature anchor plan id (or slug). Required.
- The system slug to push to (defaults to the association's primary registered system, i.e. the first registered system in the database).
- The strategy override (defaults to ADR-0006 per-feature detection: GitHub Issues → `parent-issue` + sub-issues; Jira → always epic-hierarchy. The multi-repo `projects-v2` strategy is permanently cut — decision 1001 — and is never selected).

## Outputs

- External counterparts in the target system per the operational-plane adapter shape described in [`docs/architecture.md`](../docs/architecture.md):
  - **Jira:** anchor plan → Epic, child plans → Stories (with Epic Link set), tasks → Sub-tasks, decisions → comments on the Epic.
  - **GitHub Issues (single-repo):** anchor plan → parent issue, child plans → sub-issues, tasks → sub-issues.
  - Multi-repo GitHub propagation (`projects-v2`) is cut (decision 1001) and does not exist as a strategy.
- One `external_links(link_role='mirror')` row per local entity per target system.
- One `sync_events(outcome='ok')` row per successful Create call.
- A summary report listing created, skipped, and failed entities.

## Boundaries

- **Task→plan and plan→plan attachments:** the engine walks descendant tasks via both `tasks.plan_id` (the canonical FK, set by `planar task add --plan <id>`) and `entity_links(task→plan, derives-from)` (written by the ingestor alongside the FK). Child plans are walked via `plans.parent_plan_id` alone. A task with both attachment paths is counted once; a child plan with `parent_plan_id` but no entity_link still surfaces. The current traversal contract lives in [`zig/src/engine/extsync/propagate.zig`](../zig/src/engine/extsync/propagate.zig) and [`zig/src/engine/extsync/parent_issue.zig`](../zig/src/engine/extsync/parent_issue.zig).
- **Local-project → GitHub-owner/repo resolution:** the propagate flow reads `projects.git_remote` first (parsed by `parseGitHubRepo` in [`zig/src/engine/extsync/parent_issue.zig`](../zig/src/engine/extsync/parent_issue.zig)) and falls back to splitting `projects.slug` as `owner/repo` for fixtures and manually-registered projects without a remote URL. Real `planar init`-registered projects always have `git_remote` populated.
- **Sync-direction control:** the propagate flow defaults to `external_links.sync_direction='read-only'`. This is a behavior change from prior versions where the propagate flow defaulted to `'two-way'`; users with downstream tooling that depended on the implicit two-way write must pass `--sync two-way` explicitly going forward. Users opt into write-back or two-way at creation via `planar-ext ext propagate --sync <direction>`, which carries the selected direction through every row insertion. There is no lossless direction update for an existing row: the `links update` subcommand is deferred, while `links add` / `links remove` operate on internal `entity_links`. Top-level `planar unlink` followed by `planar link` is destructive: it loses the old row's `external_url`, `config_json` (including propagation strategy), last-sync timestamp/status, and association with its sync-event history, and the replacement starts with null URL/config/last-sync plus status `never`. The old event rows remain with `link_id=null`, but the replacement cannot query or adopt them. The public CLI can capture identity/history with `planar audit trail --link <link-id> --json` and last-sync state with `planar-ext sync status --entity <kind:id> --system <slug> --json`, but it cannot export or restore the exact old URL, config, role, or direction. Stop unless the losses are acceptable and the intended role/direction are independently known; then use the destructive recovery recipe in [`docs/cli-reference.md`](../docs/cli-reference.md#planar-links-update-link-id). A propagation-owned mirror may instead be unlinked, previewed with `planar-ext ext propagate ... --dry-run`, and freshly propagated, which creates a new counterpart rather than restoring the old one. No auto-push behavior on local task/plan updates — `planar-ext sync push` remains a manual command.
- **Top-down creation order:** anchor plan → child plans → tasks → test scenarios. Each level is skipped if already linked (idempotent).
- **Idempotent on rerun:** entities that already have a `external_links(link_role='mirror')` row for the target system are skipped without error.
- **Strategy stickiness:** strategy is selected at first propagation and cached on `external_links.config_json` of the anchor plan under the key `"strategy"`. Rerun honors the cached strategy. This preserves stable external URLs the team has already shared. Use `planar-ext ext propagate --restrategize` to force fresh selection.
- **On failure mid-tree:** the failed entity is recorded with `Op="failed"` and a `sync_events(outcome='partial')` row is written for audit. Propagation continues to the next entity. Idempotent skip-if-already-linked means rerun resumes from the failure point (partial-failure resumability).
- **--restrategize abandonment:** when the fresh strategy differs from the cache, the user is prompted to confirm (or `--yes` bypasses). On confirmation, prior counterparts are NOT deleted from the remote — Planar abandons tracking them and writes `sync_events(outcome='strategy-abandoned')` per counterpart for audit. Fresh propagation then proceeds under the new strategy.
- **--verify-counterparts probe:** when set, the engine probes the remote for every already-linked entity. Entities confirmed present are "verified". Entities returning 404 are reported as "missing" with `sync_events(outcome='counterpart-missing')`. Combine with `--unlink` (remove tracking link) or `--recreate` (remove link and re-create counterpart). Off by default — probing on every run is expensive on large features.
- **Never modifies the FS workbench directly.** After propagation, the user (or the orchestrator) runs `planar workbench push <plan>` separately to update front matter with external keys.

## Strategy selection (ADR-0006)

| System | Strategy |
|--------|---------|
| Jira | `jira-epic` (always) |
| GitHub Issues | `parent-issue` (always) |

There is no repo-count-based selection today: the multi-repo `projects-v2` strategy and the zero-repo `github-zero-repo` strategy are not wired to any strategy-selection path (`projects-v2` is permanently cut per decision 1001). Detection is read-only against the local DB; no remote calls are made to determine strategy.

## Behavior

1. Resolve the anchor plan via `planar-ext ext propagate <plan>`.
2. Check claim-aware plan state before propagation. If another vendor owns an active claim on the anchor or a descendant currently being propagated, stop and surface the conflict rather than creating remote counterparts from stale local assumptions.
3. Acquire a propagation claim on the anchor plan for the duration of the run when the agent activity claim surface is available. Heartbeat during long remote calls and release as `completed` or `aborted` at the end.
4. Resolve the target external system (from `--system` flag or first registered system).
5. Select the propagation strategy (ADR-0006 detection or cached value).
6. For each entity in top-down order, check for an existing `external_links(link_role='mirror')` row for the target system.
   - If found: record as `skipped`.
   - If not found: render the template payload, call `adapter.Create`, record `external_links` + `sync_events` in a single transaction, record as `created`.
7. For Jira: post anchor-level decisions as comments on the Epic.
8. Print the propagation summary.

## Status reporting

The ext-sync agent emits a status at each meaningful phase boundary. Use the
following strings for claim-backed runs:

| Phase | Status string |
|-------|---------------|
| Resolving the anchor, descendants, system, and cached strategy | `"resolving propagation tree"` |
| Building payloads for a known entity set | `"building payloads <current>/<total>"` |
| Creating or skipping counterparts in top-down order | `"propagating entities <current>/<total>"` |
| Waiting for one in-flight adapter request to return | `"awaiting:remote-response <current>/<total>"` |
| Posting a known set of anchor decisions | `"posting decisions <current>/<total>"` |
| Probing known linked counterparts under `--verify-counterparts` | `"verifying counterparts <current>/<total>"` |
| Waiting for the `--restrategize` confirmation gate | `"awaiting:operator-confirmation"` |
| Assembling created, skipped, and failed rows | `"summarizing propagation"` |

Entity counters use the deduplicated top-down traversal set; decision counters
use the selected anchor decisions; counterpart counters use the links selected
for probing. Each counter begins at `1/<total>`, is monotonic, and never exceeds
its known total. Omit the counter until the applicable total is known and for
an empty set. Payload building, coordination, and propagation are active work
and therefore use plain statuses. Reserve `awaiting:` for the time actually
blocked on a remote response or explicit operator confirmation. The
propagation summary is the final result; it replaces any terminal heartbeat.

See `agents/methodology.md` § Heartbeat status contract
for the full convention and 256-byte cap.

## Non-trivial task scenarios

Test scenarios that derive from tasks are propagated as Stories with a `test-scenario` label (Jira) or as issues with the `test-scenario` label (GitHub). This is the only label-based semantic in any default strategy.

## CLI commands composed

```
planar-ext ext propagate <plan>              # NOT YET IMPLEMENTED — see status note above
planar-ext ext propagate <plan> --system <slug>
planar-ext ext propagate <plan> --dry-run
planar-ext ext propagate <plan> --restrategize [--yes]
planar-ext ext propagate <plan> --sync <read-only|write-back|two-way>
planar-ext ext propagate <plan> --verify-counterparts [--unlink | --recreate]
planar-ext ext propagate-one <system> --from <kind:id>   # live today
planar link <kind:id> --to <system-slug>:<external-id> --propagate
planar unlink <link-id>
planar link <kind:id> --to <system-slug>:<external-id> --role <role> --sync <read-only|write-back|two-way>
```

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine; when running under Codex, this also covers the Codex enforcement caveat for this role's `coordinate` capability.
