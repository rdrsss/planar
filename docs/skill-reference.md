# Planar Skill Reference

Skills are the primary way users and agents interact with Planar. Each skill composes `planar` binary verbs into a higher-level workflow. Authoring is unified: one source file under `skills/src/` renders to the three vendor surfaces.

This document lists every available skill, grouped by purpose, with a one-line description and an example invocation. For the full behavior spec of each skill, open the source file linked under each entry.

---

## Source And Render Model

Planar authors each shared skill once at `skills/src/<name>.md` and renders vendor outputs with `planar skills render`:

- `commands/claude/<name>.md` — generated, installed to `~/.claude/commands/<name>.md`, invoked as `/<name>`
- `skills/codex/<name>.md` — generated, materialized as `~/.planar/codex-skills/<name>/SKILL.md`, installed into `~/.codex/skills/<name>`
- `skills/copilot/<name>.md` — generated, installed to `~/.copilot/skills/<name>.md`

Vendor profile data and model-tier resolution come from `src/configs/vendors.yaml` (embedded into the `planar` binary). Drift between `skills/src/` and generated vendor trees is gated by `planar skills render --check` against an out-of-tree staging directory.

---

## Three-binary architecture

Planar ships three executables, each with a disjoint capability boundary enforced by its verb set (not by runtime ACLs). Skills and agents reach for the binary that matches the work — and only that binary. The capability boundary is locked by integration tests (`integration_tests/capability_boundary_test.zig`); see [tech spec § Binary boundaries](../../.planar/workbench/project_planar/p85-agent-activity/58-agent-activity-tracking-tech-spec.md#binary-boundaries) for the canonical table and the per-binary capability invariants.

- `planar` — operator binary. Read-write to the full schema; owns every planning-entity verb (`plan`, `task`, `decision`, `question`, `scenario`, `artifact`, `workbench`, `doc`, `spec`, `templates`, `ext`, `sync`, `init`, `dashboard`, `tree`, `audit`, `health`, …). Has **no** `agent` subcommand namespace; agent-table writes live on `planar-agent` and agent-table reads live on `planar-watch`.
- `planar-agent` — agent-callable coordination binary. Read-write **only** to `agent_actions`, `agent_work_claims`, and `tasks.status` (the last only as part of atomic coordinated operations). Verbs: `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`action end`, `ingest`, `reconcile`, `abort`, `version`. **Capability invariant:** a vendor hook configured with only `planar-agent` on its PATH cannot touch any plan / decision / question / scenario / artifact / annotation row.
- `planar-watch` — human-facing read-only viewer. Opens SQLite via `file:?mode=ro` so the driver itself refuses any write SQL. Verbs: `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `version`, `completion`. **Capability invariant:** a watcher process holding the binary on PATH cannot corrupt operator state even under hostile verb invocation — enforced both by the zero-write verb set and the read-only DB handle.

Every skill in this document routes its writes through the binary that owns them. Skills that schedule agent work (`/orchestrator`, `/pl-coder`) drive the `planar-agent pull → heartbeat → complete|fail|release|block` ritual; skills that surface live operator views (status, dashboard, audit trail) read through `planar` and `planar-watch`.

---

## Orchestration

These skills manage the full feature lifecycle and the coder/reviewer execution loop. They are the highest-level entry points.

### `/orchestrator`

Run the orchestrator over a goal, anchor plan, or task list. Manages all five phases (planning → ingestion → execution → propagation → archive) with reviewer iteration cap and user gates at each phase boundary.

**Example:**
```
/orchestrator "add billing export to CSV"
/orchestrator 42                              # resume from plan 42's current status
/orchestrator 17 18 19 --strict               # execute specific tasks, one cycle per task
/orchestrator 42 --barrel-grouped             # one cycle per milestone; reviewer per group
/orchestrator 42 --barrel-deferred            # coder cycles back-to-back; reviewer at milestone boundary
/orchestrator 42 --barrel-bypass              # no reviewer; gates are the entire signal
/orchestrator 42 --propagate --archive        # execute, then propagate and archive
```

The Phase 3 dispatch gate offers six shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`); see [`docs/concepts.md` §Dispatch shapes](concepts.md#dispatch-shapes) for the trade-off matrix. Phase 3.5 (test-coder dispatch) fires across all shapes when uncovered slugs intersect the cycle.

Source: `commands/claude/pl-orchestrator.md` · `skills/codex/pl-orchestrator.md` · `agents/orchestrator.md`

---

### `/coder`

Implement a scoped coding task end-to-end. Called by the orchestrator in Phase 3; can also be invoked directly for single-task work outside the full orchestrator flow.

**Example:**
```
/coder 37
```

Source: `commands/claude/pl-coder.md` · `skills/codex/pl-coder.md` · `agents/coder.md`

---

### `/reviewer`

Review a coder change set. Returns one of `approve`, `request-changes`, `open-question`, or `abort`. Called by the orchestrator after each coder iteration; also invokable directly.

**Example:**
```
/reviewer 37 3          # review task 37, iteration 3
```

Source: `commands/claude/pl-reviewer.md` · `skills/codex/pl-reviewer.md` · `agents/reviewer.md`

### `/pl-test-coder`

Adversarial test-author dispatched between the coder and the reviewer (Phase 3.5) when `planar test-spec status` reports uncovered slugs intersecting the dispatched cycle. Reads the test-spec and the coder's diff; produces a test-only diff that closes uncovered slugs. When a new test fails on first run, the test-coder surfaces the failure with a classification (`test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`) — it never modifies the test to make it pass. Also invokable directly to backfill coverage on an already-committed change set.

**Example:**
```
/pl-test-coder 1925              # run against one task's cited scenarios
/pl-test-coder 278 --plan        # run against every cited scenario in the plan
```

Source: `commands/claude/pl-test-coder.md` · `skills/codex/pl-test-coder.md` · `skills/copilot/pl-test-coder.md` · `agents/test-coder.md`

---

## Adoption / Onboarding

### Picking Between `/pl-import` and `/pl-synthesize`

| Repo shape | Use |
|---|---|
| Clean, structured docs (frontmatter, conventional naming) + current | `/pl-import` |
| Multiple roadmaps of different eras | `/pl-synthesize` |
| Docs-only / greenfield / no source code yet | `/pl-synthesize` |
| Docs lie (claim done; code shows incomplete) | `/pl-synthesize` |
| You want faithful transcription | `/pl-import` |
| You want LLM curation grounded in code-evidence | `/pl-synthesize` |
| Stale-WIP branch (code is misleading) | `/pl-synthesize --treat-as-greenfield` |

Both verbs land in the same `/pl-spec-ingest` pipeline downstream.

See [`concepts.md#transcription-vs-synthesis`](concepts.md#transcription-vs-synthesis) for the conceptual split and concrete fixture examples.

---

### `/pl-import`

Import an existing repository's planning artefacts (tech specs, roadmaps, ADRs, backlog files, GitHub issues) into Planar without a goal statement. Discovers artefacts from the filesystem and git history, infers task completion status, and produces an ImportPlan for review before committing.

**Example:**
```
/pl-import .                          # preview import for the current repo
/pl-import . --apply                  # commit the import
/pl-import . --from-github --apply    # include open GitHub issues
/pl-import . --dry-run                # emit JSON ImportPlan without writing
```

Source: `commands/claude/pl-import.md` · `skills/codex/pl-import.md` · `agents/importer.md`

---

### `/pl-synthesize`

Synthesize fresh planning artifacts for a repo from its existing docs + git log + source code via an LLM pass. Sibling of `/pl-import` (which transcribes). Reach for `/pl-synthesize` when docs are messy, docs-only, or mid-evolution; reach for `/pl-import` when docs are clean and current.

The LLM runs in the vendor skill (not in Go); the Go side provides the deterministic floor via `codeprobe.EvidenceMap`, writes the `synthesis.Request` to a cache file for the skill to consume, validates the Result, and merges it with the deterministic baseline.

**Example:**
```
/pl-synthesize .                          # preview the current repo
/pl-synthesize . --apply                  # commit the synthesis
/pl-synthesize . --code-layout go --apply # override layout auto-detection
/pl-synthesize . --literal                # delegate to import (transcription)
```

Source: `commands/claude/pl-synthesize.md` · `skills/codex/pl-synthesize.md` · `skills/copilot/pl-synthesize.md` · `agents/synthesizer.md`. See [`concepts.md#transcription-vs-synthesis`](concepts.md#transcription-vs-synthesis) for the decision matrix.

---

## Planning Pipeline

These three skills cover the planning cycle from goal statement to operational-plane propagation.

### `/pl-spec-draft`

Draft planning documents (product spec, tech spec, roadmap, test spec) for a new feature from a goal statement. Creates a draft anchor plan, a workbench directory, and four artifact files. The user reviews and edits the documents before the next phase. The planner authors in four sequential phases — see [`agents/planner.md` §Authoring phases](../agents/planner.md#authoring-phases). Any `## Open questions` H3 items found in the drafted spec files are auto-registered as question entities via `planar question add` — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-draft "add billing export to CSV"
```

Source: `commands/claude/pl-spec-draft.md` · `skills/codex/pl-spec-draft.md` · `agents/planner.md`

---

### `/pl-spec-ingest`

Decompose workbench planning documents (`tech-spec.md`, `roadmap.md`) into a structured task graph in the database. Always runs in preview mode first; `--apply` is required to commit. Idempotent: re-running against an unchanged spec produces identical output. On each run (preview and apply), reconciles the spec body's `## Open questions` H3 items against existing question entities and surfaces drift warnings for any discrepancy — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-ingest 42              # preview the decomposition for plan 42
/pl-spec-ingest 42 --apply      # commit additions and updates
```

Source: `commands/claude/pl-spec-ingest.md` · `skills/codex/pl-spec-ingest.md` · `agents/ingestor.md`

---

### `/pl-ext-propagate`

Propagate a feature tree (anchor plan + descendants) to a registered external operational system. Creates external counterparts (Jira epics/stories/subtasks, GitHub parent issues or Projects v2) and records `external_links(link_role='mirror')` rows. Idempotent: already-linked entities are skipped.

**Example:**
```
/pl-ext-propagate 42                          # propagate to first registered system
/pl-ext-propagate 42 --system my-jira
/pl-ext-propagate 42 --dry-run                # preview without contacting the remote
```

Source: `commands/claude/pl-ext-propagate.md` · `skills/codex/pl-ext-propagate.md` · `agents/extsync.md`

---

## Workbench

These skills manage the bidirectional sync between the workbench filesystem and the database.

### `/pl-workbench`

Full workbench management: pull, push, sync, status, resolve, archive, restore, list, and publish. Use this skill when you need fine-grained control over sync direction or need to resolve a specific conflict.

**Example:**
```
/pl-workbench pull 42           # FS → DB for plan 42
/pl-workbench push 42           # DB → FS for plan 42
/pl-workbench status            # show all plans with FS/DB divergence
/pl-workbench resolve 17 --prefer fs
/pl-workbench publish 42 --system github
```

Source: `commands/claude/pl-workbench.md` · `skills/codex/pl-workbench.md`

---

### `/pl-workbench-sync`

High-level bidirectional sync: reconciles FS and DB in one pass, surfaces conflicts as `sync_events` rows. Use this when you want "make the workbench consistent" without choosing a direction.

**Example:**
```
/pl-workbench-sync 42
```

Source: `commands/claude/pl-workbench-sync.md` · `skills/codex/pl-workbench-sync.md`

---

### `/pl-workbench-archive`

Archive or restore a feature's workbench filesystem tree. `archive` removes the on-disk tree when the feature is done; `restore` recreates it from the DB. The database always retains all entity rows regardless of archive status.

**Example:**
```
/pl-workbench-archive archive 42
/pl-workbench-archive restore 42
```

Source: `commands/claude/pl-workbench-archive.md` · `skills/codex/pl-workbench-archive.md`

---

## Workspace

### `/pl-workspace-scan`

Scan a polyrepo workspace, refresh its `routing-table.json`, and regenerate its canonical `AGENTS.md`. Without `--enrich` the skill orchestrates the deterministic static pipeline (`planar workspace routing build` + `planar workspace regenerate`). With `--enrich` the skill additionally invokes the LLM at `temperature=0` per project — using the README excerpt and a depth-2 directory listing as inputs — writes validated results into `~/.planar/cache/workspace-enrichment/<org_id>/`, and re-builds so cached results merge into the table. Operator overrides in `routing-table-overrides.json` always win over enrichment. Cross-link: [docs/concepts.md § Workspace](concepts.md#workspace).

**Composition** (the 5-step pipeline from the skill body):

1. Resolve the target workspace from `--workspace`, the active `kind=org` scope entry, or fail with a clear message.
2. Run `planar workspace routing build [<workspace>]` to refresh the static routing table.
3. If `--enrich`: for each project where the cache fingerprint misses and the static summary is not human-authored, run the LLM enrichment loop and write validated results to the enrichment cache; then re-run `planar workspace routing build --enrich` so the Go builder merges the freshly-cached results.
4. Run `planar workspace regenerate [<workspace>]` to rebuild `AGENTS.md` from the updated routing table.
5. Print a one-line summary of what changed. With `--dry-run`, prefix the summary with `would scan:` and write nothing.

**Example:**
```
/pl-workspace-scan                          # scan active workspace, static only
/pl-workspace-scan --enrich                 # static scan + LLM enrichment pass
/pl-workspace-scan --workspace org:work     # explicit workspace target
/pl-workspace-scan --dry-run                # report what would change, no writes
```

Source: `commands/claude/pl-workspace-scan.md` · `skills/codex/pl-workspace-scan.md` · `skills/copilot/pl-workspace-scan.md`

---

## Reference Workflows

These skills wrap individual `planar` subcommand domains to give agents a consistent, documented entry point for common operations.

### `/pl-init`

Initialize the Planar database and register the current directory as a project. Run once per repo.

**Example:** `/pl-init`

Source: `commands/claude/pl-init.md` · `skills/codex/pl-init.md`

---

### `/pl-scope`

Inspect the cwd-derived scope and propose associations from git remote and path. The active scope stack was removed in plan 153 M5; cd into the target repo or pass `--scope <slug>` on individual verbs to override.

**Example:**
```
/pl-scope show               # print the cwd-derived scope set
/pl-scope show --json        # machine-readable shape
/pl-scope suggest            # candidate associations for this cwd
```

Source: `commands/claude/pl-scope.md` · `skills/codex/pl-scope.md`

---

### `/pl-plan`

Draft a plan from a goal, decompose into steps, link to specs and decisions.

**Example:** `/pl-plan "design the billing export schema"`

Source: `commands/claude/pl-plan.md` · `skills/codex/pl-plan.md`

---

### `/pl-task`

Add, list, prioritize, block, and complete tasks within active scope.

**Example:**
```
/pl-task add "implement CSV serialiser" --plan 42
/pl-task list
/pl-task done 17
```

Source: `commands/claude/pl-task.md` · `skills/codex/pl-task.md`

---

### `/pl-question`

Capture open questions during a session, answer them, and link to tasks and specs.

**Example:**
```
/pl-question add "Which date format for export timestamps?" --task 37
/pl-question answer 5 "ISO 8601 UTC, no timezone offset"
```

Source: `commands/claude/pl-question.md` · `skills/codex/pl-question.md`

---

### `/pl-scenario`

Author test scenarios from a spec or task, verify them, and record outcomes.

**Example:**
```
/pl-scenario add --task 37 "Export with 10k rows completes in under 5s"
/pl-scenario pass 3
```

Source: `commands/claude/pl-scenario.md` · `skills/codex/pl-scenario.md`

---

### `/pl-promote`

Surface personal entities that have matured and promote or demote them between scopes.

**Example:**
```
/pl-promote task:42 to assoc:project:my-app
/pl-promote demote task:42
```

Source: `commands/claude/pl-promote.md` · `skills/codex/pl-promote.md`

---

### `/pl-sync`

Pull from and push to the operational plane, surface and resolve conflicts.

**Example:**
```
/pl-sync pull --system my-jira
/pl-sync push task:37 --system my-jira
```

Source: `commands/claude/pl-sync.md` · `skills/codex/pl-sync.md`

---

### `/pl-ext-create`

Create a Jira or GitHub Issues counterpart from a local entity and record the external link.

**Example:**
```
/pl-ext-create task:37 --system my-gh
```

Source: `commands/claude/pl-ext-create.md` · `skills/codex/pl-ext-create.md`

---

### `/pl-audit-trail`

For a given external link, show every local session, decision, and commit tied to it.

**Example:**
```
/pl-audit-trail my-jira:PROJ-1234
```

Source: `commands/claude/pl-audit-trail.md` · `skills/codex/pl-audit-trail.md`

---

### `/pl-resume`

Resume an in-flight task from zero conversational context. Validates resume readiness first (checks that `next_action` is set, no unresolved questions block progress).

**Example:**
```
/pl-resume 37
```

Source: `commands/claude/pl-resume.md` · `skills/codex/pl-resume.md`

---

### `/pl-handoff`

Capture a context snapshot before terminating, validate it is resume-ready, and write the handoff record.

**Example:** `/pl-handoff`

Source: `commands/claude/pl-handoff.md` · `skills/codex/pl-handoff.md` · `agents/methodology.md`

---

### `/pl-help`

Summarize available skills and reference workflows for the current vendor surface.

**Example:** `/pl-help`

Source: `commands/claude/pl-help.md` · `skills/codex/pl-help.md`

---

### `/pl-health`

Report database and handoff readiness health: schema version, pending migrations, session state, unresolved sync conflicts.

**Example:** `/pl-health`

Source: `commands/claude/pl-health.md` · `skills/codex/pl-health.md`

---

### `/pl-status`

Summarize the current scope's state: active and paused plans, open tasks (todo / doing / blocked) grouped by plan, and open questions. Read-only. Use at the start of a session for quick orientation.

**Example:** `/pl-status`

Source: `commands/claude/pl-status.md` · `skills/codex/pl-status.md` · `skills/copilot/pl-status.md`

---

### `/pl-templates`

Inspect, validate, and render Planar JSON templates for external-system propagation (Jira, GitHub Issues, GitHub Projects).

**Example:**
```
/pl-templates list
/pl-templates validate
/pl-templates render task:37 --system my-jira
```

Source: `commands/claude/pl-templates.md` · `skills/codex/pl-templates.md`

---

## Agent Role Specs

The vendor-neutral role specs live under `agents/`. Vendor skill files defer to them for the authoritative behavior description.

| File | Role |
|------|------|
| `agents/methodology.md` | Shared orchestration methodology: iteration loop, reviewer decisions, escalation, concurrency rules, state capture |
| `agents/orchestrator.md` | Orchestrator role: phase descriptions, dispatch-shape gate, per-phase triggers |
| `agents/planner.md` | Planner role: input/output contract, document shape, workbench seeding |
| `agents/ingestor.md` | Ingestor role: parsing contract, idempotency invariant, preview-first rule |
| `agents/extsync.md` | Ext-sync role: strategy-selection contract, propagation walk, idempotency |
| `agents/coder.md` | Coder role: task implementation contract, test requirements, reporting format |
| `agents/reviewer.md` | Reviewer role: review criteria, decision taxonomy, caveat recording |
| `agents/models.md` | Tier-to-model resolution: maps `large` / `medium` tiers to concrete model IDs per vendor |

---

## Personal sandbox

The repo ships canonical skills and agents under `commands/claude/`, `skills/codex/`, `skills/copilot/`, and `agents/`. Operators who want **personal, machine-local skills and agents** — single-purpose workflows specific to their environment — use the sandbox at `~/.planar/local/{skills,agents}/`.

Authored sandbox sources are installed (symlink with copy fallback) into every vendor's install directory by `planar local link`. Skills are dir-shape (`~/.planar/local/skills/<name>/SKILL.md`); agents stay flat. Each vendor's discovery loader is shape-specific:

```
skills → ~/.claude/commands/local-<name>.md             (file symlink → <src>/<name>/SKILL.md)
         ~/.codex/skills/local-<name>                   (dir symlink → <src>/<name>/)
         ~/.copilot/skills/local-<name>                 (dir symlink → <src>/<name>/)
agents → ~/.planar/agents/local-<name>.md               (file symlink → <src>/<name>.md)
```

The `local-` prefix on the install name makes sandbox skills visibly user-authored in every vendor's listing and prevents collisions with canonical installs. `shadow: true` in source frontmatter drops the prefix to explicitly replace a canonical install (with a warning at link time). The Codex / Copilot dir-symlink shape is load-bearing: those loaders empirically reject symlinked SKILL.md files inside real directories, but follow directory symlinks correctly. See [concepts.md § Local sandbox](concepts.md#local-sandbox) for the full design.

**Canonical vs sandbox.**

| | Canonical | Sandbox |
|---|---|---|
| Location | `commands/claude/`, `skills/codex/`, `skills/copilot/`, `agents/` in the repo | `~/.planar/local/{skills,agents}/` on the operator's machine |
| Install | `install.sh` or `make install` from the repo checkout | `planar local link` |
| Authoring overhead | Commit, push, `planar skills render --check` against an out-of-tree staging dir across generated vendor trees | One file, one `planar local link` |
| Distribution | Shipped to everyone using the repo | This operator's machine only |
| Promotion | N/A | Manual: copy file into the repo and follow normal contribution flow. No `planar local promote` shortcut |

**CLI surface:** `planar local list`, `planar local link [--dry-run] [--vendor <v>] [--reconcile] [<name>]`, `planar local unlink <name> [--purge]`, `planar local import <path> [--kind skill|agent] [--force] [--dry-run] [--no-link]`, `planar local migrate [--dry-run]`. Full reference under `docs/cli-reference.md § Domain: local`.

**Manual promotion.** Sandbox skills earn promotion through the standard git contribution flow — no shortcut verb. See `docs/workflows.md § Recipe 14 — Author a personal skill in the sandbox` for the end-to-end walk-through.
