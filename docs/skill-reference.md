# Planar Skill Reference

Skills are the primary way users and agents interact with Planar. Each skill composes `planar` binary verbs into a higher-level workflow. Authoring is unified: one source file under `skills/src/` renders to the three vendor surfaces.

This document lists every available skill, grouped by purpose, with a one-line description and an example invocation. For the full behavior spec of each skill, open the source file linked under each entry.

---

## Source And Render Model

Planar authors each shared skill once at `skills/src/<name>.md` and renders vendor outputs with `planar skills render`:

- `commands/claude/<name>.md` — generated, installed to `~/.claude/commands/<name>.md`, invoked as `/<name>`
- `skills/codex/<name>.md` — generated, materialized as `~/.planar/codex-skills/<name>/SKILL.md`, installed into `~/.codex/skills/<name>`
- `skills/copilot/<name>.md` — generated, installed to `~/.copilot/skills/<name>.md`

Vendor profile data and model-tier resolution are embedded directly into the `planar` binary at compile time (the YAML literal lives in `src/engine/skillrender.zig`; see `agents/models.md` for the rendered tier table). Drift between `skills/src/` and generated vendor trees is gated by `planar skills render --check` against an out-of-tree staging directory.

Rendered skills and vendor agent projections include
`x-planar-source-digest` and `x-planar-projection-digest` metadata. Both are
lowercase SHA-256 hex. The source value is shared by every vendor projection
of the same parsed authored file; the projection value also covers only the
vendor profile inputs that affect rendering and the rendered semantic payload.
Neither value depends on checkout/output paths, install paths, timestamps,
directory traversal, or local machine state. Skills and Claude/Copilot agents
carry these keys in YAML frontmatter; Codex TOML agents carry them as leading
comments to preserve its accepted key schema. Operators should treat the
values as renderer-owned metadata and regenerate projections rather than edit
them by hand.

Full installs record the selected managed projections in the versioned
`~/.planar/install-manifest.json` authority after vendor wiring succeeds. Its
rows contain vendor/kind/name identity, staged and installed paths, actual
link-or-copy kind, and the two expected digests. Only those rows are managed:
an unselected vendor or personal destination-only extension is outside
Planar's ownership. The file is atomically replaced, and older installations
that have only `.planar-install` remain valid legacy installs until the
operator reruns `install.sh`.

Use `planar skills status` to compare that authority with the staged and
vendor-installed projections. The command is read-only, identifies unselected
vendors without inventing missing rows, and labels destination-only personal
extensions `unmanaged` without claiming them. A stale or missing managed row
includes an exact scoped `planar skills repair ... --apply` command. Repair is
preview-first, follows each row's recorded copy/link kind, verifies the digest
after application, and never touches unmanaged content. A missing, malformed,
unsupported, or stamped legacy manifest instead routes to `./install.sh
--prefix <resolved-prefix>` from a Planar source checkout.

---

## Skill authoring feedback contract

Every unified skill under `skills/src/` is user-invocable unless its
frontmatter explicitly says `internal_only: true`. A user-invocable source must
contain these literal H2 sections, even when a particular return path tells the
skill to omit an empty field from its final operator output:

```markdown
## Context
## Intent
## Actions
## Result
## Warnings
## Next actions
## Recovery
```

Use each section to author the behavior for all relevant return paths:

- `Context` resolves scope, target, and mode before action.
- `Intent` restates the interpreted request in one sentence so a wrong target
  or mode is visible before consequential work.
- `Actions` reports `attempted`, `applied`, `skipped`, and `failed`. A
  multi-target operation reports all four counts and names every failed target.
- `Result` is mandatory in every final response. It reports the post-state
  identifiers, paths, or external URLs and says when the outcome is `ok`,
  `partial`, or `error`.
- `Warnings` is reserved for partial failures, assumptions that affect the
  result, unavailable verification, and degraded signal. An expected no-op is
  not itself a warning.
- `Next actions` contains zero to three executable recommendations. Do not pad
  a terminal result with generic advice.
- `Recovery` supplies an exact inspect, idempotent retry, resume, or real undo
  command when applicable. Do not invent rollback for independent writes or
  remote calls.

Prefer a stable `--json` CLI read for parsing and post-state verification, then
render concise prose. Exit code zero alone is not a verified mutation. When a
post-state read exists, use it and return its stable identifiers. When it does
not exist or fails, preserve the last known result, add a warning, and give the
inspection command. If the skill itself supports JSON, its JSON result mirrors
the same fields and action counts as the text response.

Return paths have precise meanings:

| Return path | Required operator feedback |
|---|---|
| Success | `outcome=ok`, verified result, and the applied action counts. |
| Successful no-op | `outcome=ok`, zero applied, the reason nothing changed, no warning for the expected empty state, and a useful next action when one exists. |
| Partial | `outcome=partial`, completed and failed targets, all four counts, and an exact per-failure retry or inspection command. Completed independent targets remain applied unless the CLI operation is atomic. |
| Failure | `outcome=error`, attempted versus applied work, last verified state, warnings, and an actionable inspect/retry/resume/undo command when applicable. |

Stronger role-specific contracts remain authoritative. For example, reviewer
verdicts and coder work-complete reports keep their canonical decision fields,
sections, and gate evidence; author the shared feedback fields around or as an
explicit mapping into that schema instead of replacing it with generic prose.

### Internal-only exemption

Use the exemption only for a helper with no direct operator invocation whose
calling skill or role owns the complete operator-facing result:

```yaml
---
slug: example-helper
internal_only: true
---
```

The source body must name the canonical caller and explain why feedback is
returned through that caller. The flag is not appropriate merely because a
skill is usually dispatched by the orchestrator, omitted from a common recipe,
or intended for advanced use: if an operator can invoke it as a supported
entry point, it is user-invocable. The exemption removes only the requirement
for the seven literal H2 sections. The helper must still return sufficient
failure, warning, and recovery detail for its caller to satisfy the shared
contract.

See [`agents/doctrine.md` §Operator feedback contract](../agents/doctrine.md#operator-feedback-contract)
for the cross-role outcome and verification doctrine.

---

## Binary architecture

Planar ships five executables. Four are planning-state binaries, each with a disjoint capability boundary over the shared SQLite DB enforced by its verb set (not by runtime ACLs); skills and agents reach for the binary that matches the work - and only that binary. The capability boundary across those four is locked by integration tests (`integration_tests/capability_boundary_test.zig`). The fifth, `planar-execute`, is the deterministic, spawn-free Lua workflow engine and holds no DB handle.

- `planar` — operator binary. Read-write to the full schema; owns every planning-entity verb (`plan`, `task`, `decision`, `question`, `scenario`, `artifact`, `workbench`, `doc`, `spec`, `templates`, `ext`, `sync`, `init`, `dashboard`, `tree`, `audit`, `health`, …). Has **no** `agent` subcommand namespace; agent-table writes live on `planar-agent` and agent-table reads live on `planar-watch`.
- `planar-agent` — agent-callable coordination binary. Read-write **only** to `agent_actions`, `agent_work_claims`, and `tasks.status` (the last only as part of atomic coordinated operations). Verbs: `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`action end`, `ingest`, `reconcile`, `abort`, `version`. **Capability invariant:** a vendor hook configured with only `planar-agent` on its PATH cannot touch any plan / decision / question / scenario / artifact / annotation row.
- `planar-watch` — human-facing read-only viewer. Opens SQLite via `file:?mode=ro` so the driver itself refuses any write SQL. Verbs: `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `version`, `completion`. **Capability invariant:** a watcher process holding the binary on PATH cannot corrupt operator state even under hostile verb invocation — enforced both by the zero-write verb set and the read-only DB handle.
- `planar-doc` — doc-state binary. Owns manifest-driven documentation verbs (`build`, `verify`, `diff`, `cover`, `nodoc`, `lint`, `schema`) and never opens SQLite.
- `planar-execute` — deterministic, spawn-free Lua workflow engine (plan 633). A caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over an allowlisted host surface (`cli`/`git`/`fs`/`flow`/`ctx`) and collect its JSON result. It holds **no** DB handle (it shells the planning-state binaries for state) and exposes **no** model-spawning host function, so it is a workflow runner, not a harness. It is outside the claim ritual.

An external Lua-based **harness** (a **separate external project**, distinct from `planar-execute`) is a pure CLI driver that shells these binaries to orchestrate LLM calls; it holds no DB handle and is not part of the Planar binary set.

Every skill in this document routes its writes through the binary that owns them. Skills that schedule agent work (`/orchestrator`, `/pl-coder`) drive the `planar-agent pull → heartbeat → complete|fail|release|block` ritual; skills that surface live operator views (status, dashboard, audit trail) read through `planar` and `planar-watch`.

---

## Orchestration

These skills manage the full feature lifecycle and the coder/reviewer execution loop. They are the highest-level entry points.

### `/orchestrator`

Run the orchestrator over a goal, anchor plan, or task list. Manages all five phases (planning → ingestion → execution → propagation → archive) with reviewer iteration cap and user gates at each phase boundary.

**Example:**
```
/orchestrator "add billing export to CSV"
/orchestrator <plan-id>                       # resume from the plan's current status
/orchestrator <task-id> <task-id> --strict    # execute specific tasks, one cycle per task
/orchestrator <plan-id> --barrel-grouped      # one cycle per milestone; reviewer per group
/orchestrator <plan-id> --barrel-deferred     # coder cycles back-to-back; reviewer at milestone boundary
/orchestrator <plan-id> --barrel-bypass       # no reviewer; gates are the entire signal
/orchestrator <plan-id> --propagate --archive # execute, then propagate and archive
```

The Phase 3 dispatch gate offers six shapes (`strict`, `grouped`, `single`, `barrel-grouped`, `barrel-deferred`, `barrel-bypass`); see [`docs/concepts.md` §Dispatch shapes](concepts.md#dispatch-shapes) for the trade-off matrix. Phase 3.5 (test-coder dispatch) fires across all shapes when uncovered slugs intersect the cycle.

Source: `commands/claude/pl-orchestrator.md` · `skills/codex/pl-orchestrator.md` · `agents/orchestrator.md`

---


### `/coder`

Implement a scoped coding task end-to-end. Called by the orchestrator in Phase 3; can also be invoked directly for single-task work outside the full orchestrator flow.

**Example:**
```
/coder <task-id>
```

Source: `commands/claude/pl-coder.md` · `skills/codex/pl-coder.md` · `agents/coder.md`

---

### `/reviewer`

Review a coder change set. Returns one of `approve`, `request-changes`, `open-question`, or `abort`. Called by the orchestrator after each coder iteration; also invokable directly.

**Example:**
```
/reviewer <task-id> <iteration>
```

Source: `commands/claude/pl-reviewer.md` · `skills/codex/pl-reviewer.md` · `agents/reviewer.md`

### `/pl-test-coder`

Adversarial test-author dispatched between the coder and the reviewer (Phase 3.5) when `planar test-spec status` reports uncovered slugs intersecting the dispatched cycle. Reads the test-spec and the coder's diff; produces a test-only diff that closes uncovered slugs. When a new test fails on first run, the test-coder surfaces the failure with a classification (`test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`) — it never modifies the test to make it pass. Also invokable directly to backfill coverage on an already-committed change set.

**Example:**
```
/pl-test-coder <task-id>         # run against one task's cited scenarios
/pl-test-coder <plan-id> --plan  # run against every cited scenario in the plan
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

Both verbs land in the same `/pl-spec-review` -> `/pl-spec-ingest` pipeline
downstream.

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

The LLM runs in the vendor skill, not in the Planar binary. `src/engine/synthesize.zig` builds the deterministic `codeprobe.EvidenceMap` floor, writes a fingerprinted `synthesis.Request` to a cache file for the skill to consume, then on the follow-up invocation validates the skill's `Result` and merges it with the deterministic baseline.

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

These skills cover the planning cycle from goal statement to operational-plane propagation.

### `/pl-spec-draft`

Draft planning documents (product spec, tech spec, roadmap, test spec) for a new feature from a goal statement. Creates a draft anchor plan, a workbench directory, and four artifact files. The user reviews and edits the documents before the next phase. The planner authors in four sequential phases — see [`agents/planner.md` §Authoring phases](../agents/planner.md#authoring-phases). Its final draft-coverage check is the read-only `planar spec ingest <plan> --strict --json` preview; `test-spec status` is reserved for live rows after ingestion. Any `## Open questions` H3 items found in the drafted spec files are auto-registered as question entities via `planar question add` — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-draft "add billing export to CSV"
```

Source: `commands/claude/pl-spec-draft.md` · `skills/codex/pl-spec-draft.md` · `agents/planner.md`

---

### `/pl-spec-review`

Adversarially review draft planning specs before ingestion. Reconstructs what
the feature is supposed to be, checks whether it matches the user's intent,
classifies open questions, runs feature-gap and consistency analysis, and
checks roadmap/test-scenario readiness. Default mode is read-only; `--write`
only applies operator-approved artifact and question updates. For a draft plan,
the reviewer treats strict preview coverage (including uncovered slugs, orphan
scenarios, and slug collisions) as authoritative; after apply it switches to
`planar test-spec status <plan> --json` over live rows.

**Example:**
```
/pl-spec-review <plan-id>
/pl-spec-review <plan-id> --write
```

Source: `commands/claude/pl-spec-review.md` · `skills/codex/pl-spec-review.md` · `agents/spec-reviewer.md`

---

### `/pl-spec-ingest`

Decompose workbench planning documents (`tech-spec.md`, `roadmap.md`) into a structured task graph in the database. Always runs in preview mode first; `--apply` is required to commit. Idempotent: re-running against an unchanged spec produces identical output. On each run (preview and apply), reconciles the spec body's `## Open questions` H3 items against existing question entities and surfaces drift warnings for any discrepancy — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-ingest <plan-id>       # preview the decomposition
/pl-spec-ingest <plan-id> --apply
```

Source: `commands/claude/pl-spec-ingest.md` · `skills/codex/pl-spec-ingest.md` · `agents/ingestor.md`

---

### `/pl-ext-propagate`

Propagate a feature tree (anchor plan + descendants) to a registered external operational system. Creates external counterparts (Jira epics/stories/subtasks, GitHub parent issues or Projects v2) and records `external_links(link_role='mirror')` rows. Idempotent: already-linked entities are skipped.

**Example:**
```
/pl-ext-propagate <plan-id>                   # propagate to first registered system
/pl-ext-propagate <plan-id> --system my-jira
/pl-ext-propagate <plan-id> --dry-run         # preview without contacting the remote
```

Source: `commands/claude/pl-ext-propagate.md` · `skills/codex/pl-ext-propagate.md` · `agents/extsync.md`

---

## Workbench

These skills manage the bidirectional sync between the workbench filesystem and the database.

### `/pl-workbench`

Full workbench management: pull, push, sync, status, resolve, archive, restore, list, and publish. Use this skill when you need fine-grained control over sync direction or need to resolve a specific conflict.

**Example:**
```
/pl-workbench pull <plan-id>    # FS -> DB
/pl-workbench push <plan-id>    # DB -> FS
/pl-workbench status            # show all plans with FS/DB divergence
/pl-workbench resolve <conflict-id> --prefer fs
/pl-workbench publish <plan-id> --system github
```

Source: `commands/claude/pl-workbench.md` · `skills/codex/pl-workbench.md`

---

### `/pl-workbench-sync`

High-level bidirectional sync: reconciles FS and DB in one pass, surfaces conflicts as `sync_events` rows. Use this when you want "make the workbench consistent" without choosing a direction.

**Example:**
```
/pl-workbench-sync <plan-id>
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

Inspect the cwd-derived scope and propose associations from git remote and path. Scope is derived from cwd; cd into the target repo or pass `--scope <slug>` on individual verbs to override.

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
/pl-task add "implement CSV serialiser" --plan <plan-id>
/pl-task list
/pl-task done <task-id>
```

Source: `commands/claude/pl-task.md` · `skills/codex/pl-task.md`

---

### `/pl-question`

Capture open questions during a session, answer them, and link to tasks and specs.

**Example:**
```
/pl-question add "Which date format for export timestamps?" --task <task-id>
/pl-question answer <question-id> "ISO 8601 UTC, no timezone offset"
```

Source: `commands/claude/pl-question.md` · `skills/codex/pl-question.md`

---

### `/pl-scenario`

Author test scenarios from a spec or task, verify them, and record outcomes.

**Example:**
```
/pl-scenario add --task <task-id> "Export with 10k rows completes in under 5s"
/pl-scenario pass <scenario-id>
```

Source: `commands/claude/pl-scenario.md` · `skills/codex/pl-scenario.md`

---

### `/pl-promote`

Surface personal entities that have matured and promote or demote them between scopes.

**Example:**
```
/pl-promote task:<task-id> --to assoc:project:my-app
/pl-promote demote task:<task-id>
```

Source: `commands/claude/pl-promote.md` · `skills/codex/pl-promote.md`

---

### `/pl-sync`

Pull from and push to the operational plane, surface and resolve conflicts.

**Example:**
```
/pl-sync pull --system my-jira
/pl-sync push task:<task-id> --system my-jira
```

Source: `commands/claude/pl-sync.md` · `skills/codex/pl-sync.md`

---

### `/pl-ext-create`

Create a Jira or GitHub Issues counterpart from a local entity and record the external link.

**Example:**
```
/pl-ext-create my-gh --from task:<task-id>
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
/pl-resume <task-id>
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

Report database, handoff-readiness, and installed-projection health. The output
includes manifest-owned projection freshness and its exact repair command when
stale, missing, legacy, invalid, or unsupported state needs attention. The
read never repairs files; unmanaged local extensions and unselected vendors do
not degrade health.

**Example:** `/pl-health`

Source: `commands/claude/pl-health.md` · `skills/codex/pl-health.md`

---

### `/pl-doctor`

Guided diagnose-then-reconcile flow for a degraded Planar installation. Companion to `/pl-health` (which reports; this one acts).

**Flow:**

1. Run `planar health --json`. If `overall == "ok"`, stop.
2. Stale claims — `planar-agent reconcile --dry-run` to preview, then `planar-agent reconcile` to mark them stale. Only affects expired leases; never touches live claims.
3. Stale handoffs — list via `planar handoff list`, inspect with `planar handoff show`, then `planar handoff abandon <id>` per handoff (operator-confirmed).
4. Non-resumable in-flight tasks — surface each with its age, plan, and scope. Classify: cancel tasks in dead plans, reset weeks-stale tasks in active plans to `todo`, **leave recent (< 48h) tasks alone**. Each write is operator-confirmed.
5. Re-run `planar health` and report the new state.

**Safety contract:** Every destructive write is operator-confirmed before executing. Recent in-flight tasks are never auto-touched. Reconcile only affects expired claims. No `--no-scope-check`.

**Hard-won CLI facts encoded:**
- `planar health` is global (whole DB); `planar task list` is scope-filtered (cwd-derived); `planar handoff list` is global (no scope filter). `--scope global` on `task list` returns only global-scoped tasks — not all scopes. To enumerate in-flight tasks across all scopes, run `task list --status doing,blocked` per-association (from each project's directory or via `--scope <slug>`).
- Cross-scope task writes: `task update` (including `--status`) is scope-guarded and requires `--scope <task-assoc-slug>`; `task cancel` is id-based and unguarded (succeeds from any cwd).
- `task update` does not accept `--editor`; apply `--status` / `--next-action` directly as flags.

**Example:**
```
/pl-doctor
```

Source: `skills/src/pl-doctor.md`

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
/pl-templates render task:<task-id> --system my-jira
```

Source: `commands/claude/pl-templates.md` · `skills/codex/pl-templates.md`

---

## Usage Introspection

### `/pl-introspect`

Run a usage-introspection pass: mine `planar report --json` and local Claude
transcript JSONL files for friction patterns (failure clusters, retry sequences,
stale claims, gap features) and file each pattern as a structured finding on
the association's `planar-feedback` plan.

**Arguments:** `[--days <n>] [--scope <scope>]`

- `--days <n>` — report window (default 30).
- `--scope <scope>` — association scope for the feedback plan (default: cwd-derived).

**Example:**
```
/pl-introspect
/pl-introspect --days 7
/pl-introspect --scope assoc:my-org
```

**Finding taxonomy:** `failure-cluster`, `retry-pattern`, `abandoned-workflow`, `gap-feature`.

**Title convention:** `<taxonomy-key>: <signal-key>` (e.g. `retry-pattern: task add`). Titles are deterministic and signal-derived — re-runs over the same signal are idempotent via title-based dedup.

**Privacy:** Transcript text is ephemeral and never persisted. Finding bodies carry only aggregate signal (counts, verb paths, error categories). See [Usage Introspection Privacy Model](concepts.md#usage-introspection-privacy-model).

Source: `skills/src/pl-introspect.md` · `agents/introspector.md`

---

## Agent Role Specs

The vendor-neutral role specs live under `agents/`. Vendor skill files defer to them for the authoritative behavior description.

| File | Role |
|------|------|
| `agents/methodology.md` | Shared orchestration methodology: iteration loop, reviewer decisions, escalation, concurrency rules, state capture |
| `agents/orchestrator.md` | Orchestrator role: phase descriptions, dispatch-shape gate, per-phase triggers |
| `agents/spec-reviewer.md` | Spec reviewer role: adversarial planning review, open-question reconciliation, feature/test gap analysis |
| `agents/planner.md` | Planner role: input/output contract, document shape, workbench seeding |
| `agents/ingestor.md` | Ingestor role: parsing contract, idempotency invariant, preview-first rule |
| `agents/extsync.md` | Ext-sync role: strategy-selection contract, propagation walk, idempotency |
| `agents/coder.md` | Coder role: task implementation contract, test requirements, reporting format |
| `agents/reviewer.md` | Reviewer role: review criteria, decision taxonomy, caveat recording |
| `agents/introspector.md` | Introspector role: read surface, transcript-mining recipe, finding taxonomy, dedup contract, feedback-plan bootstrap |
| `agents/janitor.md` | Janitor role: merge verification, Planar state reconciliation, worktree/branch cleanup, plan closeout via the delivery-evidence gate |
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
