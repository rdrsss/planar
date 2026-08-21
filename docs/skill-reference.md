# Planar Skill Reference

Skills are the primary way users and agents interact with Planar. Each skill composes `planar` binary verbs into a higher-level workflow. Authoring is unified: one source file under `skills/src/` renders to the three vendor surfaces.

This document lists every available skill, grouped by purpose, with a one-line description and an example invocation. For the full behavior spec of each skill, open the source file linked under each entry.

---

## Choose By Intent

Start with the intent-oriented skill when the request spans several CLI
domains; use the listed verbs directly when you need their exact flags or one
narrow operation. `/pl-help <outcome>` can select among these routes and always
shows the underlying supported interface.

| Outcome | Recommended entry point | Underlying CLI surface |
|---|---|---|
| Decide what needs attention in the current scope | `/pl-status` | `planar dashboard --agents`, `planar plan next`, `planar health` |
| Observe one plan's actions, claims, failures, sync events, and handoffs | `/pl-observe --plan <id>` | `planar dashboard --agents`, `planar-watch actions|ps|feed|sync-events`, `planar handoff list` |
| Record or connect durable technical knowledge | `/pl-knowledge ...` | `planar decision`, `planar artifact`, `planar annotate`, `planar links` |
| Manage machine-local skills and agents | `/pl-local ...` | `planar local import|link|list|unlink|migrate`; repair uses `planar local link --reconcile` |
| Resume interrupted work or diagnose degraded state | `/pl-resume <task-id>` or `/pl-doctor` | `planar resume`, `planar audit`, `planar health`, `planar-agent reconcile` |
| Inspect one external item's local history | `/pl-audit-trail <system:key>` | `planar audit trail` |
| Reconcile a local/external sync conflict | `/pl-sync status` or `/pl-sync resolve <event-id>` | `planar sync status`, `planar audit trail`, guarded `planar sync resolve` |
| Maintain published documentation | Raised to tabularium (owns the doc-system tool and its `/pl-doc-maintain` full loop / `/pl-documenter` proposal sweep); planar drives the underlying tool verbs | `tabularium diff|cover|nodoc|lint|build|verify` |

`/pl-local-import` remains an import-only compatibility entry point; prefer
`/pl-local` for the complete local lifecycle. Documentation maintenance was
raised to tabularium (see the Documentation Maintenance section below); planar
drives the underlying `tabularium` tool verbs. Sync reconciliation is
shipped through `/pl-sync` and its gated `sync-reconciler` specialist.

---

## Source And Render Model

Planar authors each shared skill once at `skills/src/<name>.md`. `install.sh`
shells the external scriptorium binary to render vendor outputs (discovered by
`scripts/discover-scriptorium.sh`: PATH lookup + a `$SCRIPTORIUM_BIN`
override, gated on a version floor) — `scriptorium render --config
scriptorium.yaml` (plan 918):

- Claude projections are staged under `$PLANAR_HOME/commands/claude/`, installed
  to `~/.claude/commands/`, and invoked as `/<name>`.
- Codex projections are staged as
  `$PLANAR_HOME/codex-skills/<name>/SKILL.md` and installed under
  `$CODEX_HOME/skills/` (normally `~/.codex/skills/`).
- Copilot projections are staged under `$PLANAR_HOME/copilot-skills/` and
  installed to `~/.copilot/skills/`.

The old repo-relative `commands/claude/`, `skills/codex/`, and
`skills/copilot/` trees are not checked in. Do not author, link documentation
to, or commit those generated outputs. Edit `skills/src/<name>.md` or the
vendor-neutral role in `agents/`, then render to an out-of-tree destination for
validation. A normal full install renders and stages projections before wiring
the selected vendors.

Vendor profile data lives in scriptorium's built-in per-vendor profiles
(claude/codex/copilot/gemini), with the repo-root `scriptorium.yaml`
supplying overrides/additions; model-tier resolution stays Planar-side in the
shared resolver (`src/engine/models.zig`) and is regenerated into
the Tier Table lives in `agents/models.md`, hand-maintained; Planar no longer generates it.
Static render paths show each tier's default model (`list[0]` when the config
uses candidate lists); runtime orchestration may further select a
per-work-type candidate through the shared model resolver. Drift between
`skills/src/` and generated vendor trees is gated by `scriptorium check`
against an out-of-tree staging directory. The Tier Table is not gated here:
it is hand-maintained in `agents/models.md`, and
Planar neither generates nor validates it.

Planar's own in-band `x-planar-source-digest`/`x-planar-projection-digest`
frontmatter metadata retired along with the in-tree renderer (plan 918 D5) —
scriptorium-rendered projections carry neither header. Scriptorium tracks
render/install freshness out-of-band in its own machine-local merkle+xxhash
manifest: `scriptorium check` reports drift (read-only, non-zero exit on any
finding) and `scriptorium status` gives the registry-vs-installed view.
`planar skills status`/`planar skills repair`, which used to read the in-band
digests, retired alongside them; `planar skills` itself is now a placeholder
verb with no subcommands.

Full installs still record the selected managed projections in Planar's own
versioned `~/.planar/install-manifest.json` authority after vendor wiring
succeeds (`scripts/install-manifest.sh`). Its rows contain vendor/kind/name
identity, staged and installed paths, and the actual link-or-copy kind; the
two digest columns are populated only when the staged file happens to carry
the legacy headers (nothing does, post-migration) and are left empty
otherwise — an empty digest is never treated as a mismatch. Only those rows
are managed: an unselected vendor or personal destination-only extension is
outside Planar's ownership. The file is atomically replaced, and older
installations that have only `.planar-install` remain valid legacy installs
until the operator reruns `install.sh`.

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

See `agents/doctrine.md` § Operator feedback contract
for the cross-role outcome and verification doctrine.

### Cross-scope write visibility

When a Planar-authored agent or skill is about to write outside the scope
derived from its cwd, it prints a standalone cue immediately before the write:

```text
[cross-scope write: project:planar]
```

Normalize labels independently from command targeting. For verbs that support
`--scope`, a repo/project slug `planar` uses cue `project:planar` and `--scope
repo:planar`; an ordinary association slug `org:acme` uses cue
`association:org:acme` and `--scope assoc:org:acme`; a legacy association row
whose slug is `project:planar` and whose `kind_label=project` uses cue
`project:planar` and `--scope assoc:project:planar`; global uses cue `global`
and `--scope global`. The legacy row never displays
`association:project:planar`.

When a verb has no `--scope`, preserve its supported explicit target instead.
Workbench workflows retain their positional `plan:<id>` target;
promote/demote retain `--to` or `--from`/global demotion; and workspace routing
or regeneration translates skill input `--workspace org:work` to the CLI's
positional `org:work` target. Before the targetless, fleet-wide `workspace
doctor`, emit one normalized cue per foreign registered org workspace, sorted
by label, then invoke doctor once. Never add `--scope` to a verb whose schema
lacks it. Same-scope writes MUST NOT emit any cross-scope cue. Commands remain
subject to their normal operator gates, scope guards, claim ownership, and
binary capability boundary; the cue changes transcript visibility only.

---

## Binary architecture

Planar ships four executables. Three are planning-state binaries, each with a disjoint capability boundary over the shared SQLite DB enforced by its verb set (not by runtime ACLs); skills and agents reach for the binary that matches the work - and only that binary. The capability boundary across those four is locked by integration tests (`integration_tests/capability_boundary_test.zig`). The fourth, `planar-execute`, is the deterministic, spawn-free Lua workflow engine and holds no DB handle.

- `planar` — operator binary. Read-write to the full schema; owns every planning-entity verb (`plan`, `task`, `decision`, `question`, `scenario`, `artifact`, `workbench`, `doc`, `spec`, `templates`, `ext`, `sync`, `init`, `dashboard`, `tree`, `audit`, `health`, …). Has **no** `agent` subcommand namespace; agent-table writes live on `planar-agent` and agent-table reads live on `planar-watch`.
- `planar-agent` — agent-callable coordination binary. Read-write only to its
  bounded coordination surfaces (`agent_actions`, `agent_work_claims`,
  `workflow_runs`, and `context_records`) plus `tasks.status` as part of atomic
  coordinated operations. Verbs include the claim ritual, action/ingest,
  workflow/context, and recovery surfaces. **Capability invariant:** a vendor
  hook configured with only `planar-agent` on its PATH cannot touch any plan /
  decision / question / scenario / artifact / annotation row.
- `planar-watch` — human-facing read-only viewer. Opens SQLite via `file:?mode=ro` so the driver itself refuses any write SQL. Verbs: `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `version`, `completion`. **Capability invariant:** a watcher process holding the binary on PATH cannot corrupt operator state even under hostile verb invocation — enforced both by the zero-write verb set and the read-only DB handle.
- `planar-execute` — deterministic, spawn-free Lua workflow engine (plan 633). A caller invokes `planar-execute run <wf.lua> --phase <name>` to run a deterministic workflow over an allowlisted host surface (`cli`/`git`/`fs`/`flow`/`ctx`) and collect its JSON result. It holds **no** DB handle (it shells the planning-state binaries for state) and exposes **no** model-spawning host function, so it is a workflow runner, not a harness. It is outside the claim ritual.

An external Lua-based **harness** (a **separate external project**, distinct from `planar-execute`) is a pure CLI driver that shells these binaries to orchestrate LLM calls; it holds no DB handle and is not part of the Planar binary set.

Documentation workflows call the separately installed `tabularium` tool. It is
not a Planar build artifact and owns its machine-local manifest database.

Every skill in this document routes its writes through the binary that owns them. Skills that schedule agent work (`/pl-orchestrator` and `/pl-coder`) drive the `planar-agent pull → heartbeat → complete|fail|release|block` ritual; skills that surface live operator views (status, dashboard, audit trail) read through `planar` and `planar-watch`.

---

## Orchestration

The orchestrator, coder, reviewer, research, and test-coder roles that manage
the full feature lifecycle and the coder/reviewer execution loop live in this
repo (raised to armarium at plan 918/929, returned at the armarium
reintegration). `/pl-orchestrator`, `/pl-coder`, `/pl-reviewer`,
`/pl-research`, and `/pl-test-coder` render and install from
`skills/src/` like every other skill. Planar also drives the primitives they
compose: the `planar-agent` claim ritual, `planar test-spec status` coverage
gating, and the dispatch-preview model-tier/routing surface. See
[`docs/operations.md` §2 The Orchestration Lifecycle](operations.md#2-the-orchestration-lifecycle)
and [`docs/concepts.md` §Dispatch shapes](concepts.md#dispatch-shapes).

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

Source: `skills/src/pl-import.md` · `agents/importer.md`

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

Source: `skills/src/pl-synthesize.md` · `agents/synthesizer.md`. See [`concepts.md#transcription-vs-synthesis`](concepts.md#transcription-vs-synthesis) for the decision matrix.

---

## Planning Pipeline

These skills cover the planning cycle from goal statement to operational-plane propagation.

### `/pl-spec-draft`

Draft planning documents (product spec, tech spec, roadmap, test spec) for a new feature from a goal statement. Creates a draft anchor plan, a workbench directory, and four artifact files. The user reviews and edits the documents before the next phase. The planner authors in four sequential phases — see [`agents/planner.md` §Authoring phases](../agents/planner.md#authoring-phases). Its final draft-coverage check is the read-only `planar spec ingest <plan> --strict --json` preview; `test-spec status` is reserved for live rows after ingestion. Any `## Open questions` H3 items found in the drafted spec files are auto-registered as question entities via `planar question add` — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-draft "add billing export to CSV"
```

Source: `skills/src/pl-spec-draft.md` · `agents/planner.md`

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

Every review also records four hazard-lens results as `finding`, `covered`, or
`not applicable`, with artifact evidence: resource lifecycle and cleanup;
deterministic ordering and replay; concurrency, ownership, cancellation, and
races; and escaping across shell/build/template interpretation boundaries. An
applicable omission becomes a feature gap. A non-applicable lens states why;
the reviewer does not manufacture boilerplate findings.

**Example:**
```
/pl-spec-review <plan-id>
/pl-spec-review <plan-id> --write
```

Source: `skills/src/pl-spec-review.md` · `agents/spec-reviewer.md`

---

### `/pl-spec-ingest`

Decompose workbench planning documents (`tech-spec.md`, `roadmap.md`) into a structured task graph in the database. Always runs in preview mode first; `--apply` is required to commit. Idempotent: re-running against an unchanged spec produces identical output. On each run (preview and apply), reconciles the spec body's `## Open questions` H3 items against existing question entities and surfaces drift warnings for any discrepancy — see the "Reviewing open questions" recipe in `docs/workflows.md`.

**Example:**
```
/pl-spec-ingest <plan-id>       # preview the decomposition
/pl-spec-ingest <plan-id> --apply
```

Source: `skills/src/pl-spec-ingest.md` · `agents/ingestor.md`

---

### `/pl-ext-propagate`

Propagate a feature tree (anchor plan + descendants) to a registered external operational system. Creates external counterparts (Jira epics/stories/subtasks, GitHub parent issues or Projects v2) and records `external_links(link_role='mirror')` rows. Idempotent: already-linked entities are skipped.

**Example:**
```
/pl-ext-propagate <plan-id>                   # propagate to first registered system
/pl-ext-propagate <plan-id> --system my-jira
/pl-ext-propagate <plan-id> --dry-run         # preview without contacting the remote
```

Source: `skills/src/pl-ext-propagate.md` · `agents/ext-sync.md`

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

Source: `skills/src/pl-workbench.md`

---

### `/pl-workbench-sync`

High-level bidirectional sync: reconciles FS and DB in one pass, surfaces conflicts as `sync_events` rows. Use this when you want "make the workbench consistent" without choosing a direction.

**Example:**
```
/pl-workbench-sync <plan-id>
```

Source: `skills/src/pl-workbench-sync.md`

---

### `/pl-workbench-archive`

Archive or restore a feature's workbench filesystem tree. `archive` removes the on-disk tree when the feature is done; `restore` recreates it from the DB. The database always retains all entity rows regardless of archive status.

**Example:**
```
/pl-workbench-archive archive 42
/pl-workbench-archive restore 42
```

Source: `skills/src/pl-workbench-archive.md`

---

## Workspace

### `/pl-workspace-scan`

Manage the complete polyrepo workspace lifecycle through the Planar CLI: initialize a workspace, diagnose registered workspaces, inspect or rebuild routing, scan with optional enrichment, regenerate canonical guidance, and repair drift. The compatible no-argument scan remains `planar workspace routing build` followed by `planar workspace regenerate`; `--enrich` delegates enrichment to the routing build instead of implementing a separate cache protocol. Cross-link: [docs/concepts.md § Workspace](concepts.md#workspace).

**Lifecycle operations:**

1. `init` runs `planar workspace init` from the workspace root.
2. `doctor` runs `planar workspace doctor` and verifies the repaired fleet with a second pass.
3. `routing-show` and `routing-build` inspect or rebuild routing with `planar workspace routing show|build`; build accepts `--enrich`.
4. `scan` composes routing build, routing show, and `planar workspace regenerate`; `regenerate` can also run independently after routing is verified.
5. `repair` composes doctor, routing rebuild and verification, regeneration, and a final doctor pass. Skill-level `--dry-run` performs reads and reports the commands that would run.

Workspace-root `AGENTS.md` and `CLAUDE.md` are generated links, or copy fallbacks,
to canonical state under `~/.planar/workspaces/<org_id>/`; do not hand-edit them.
Operator overrides belong in `routing-table-overrides.json` beside the canonical
target, while generated routing and guidance are refreshed through the CLI.

**Example:**
```
/pl-workspace-scan                          # scan active workspace, static only
/pl-workspace-scan --enrich                 # static scan + LLM enrichment pass
/pl-workspace-scan init                     # initialize from the workspace root
/pl-workspace-scan doctor                   # diagnose and repair registered workspaces
/pl-workspace-scan routing-show --workspace org:work
/pl-workspace-scan repair --workspace org:work
/pl-workspace-scan --workspace org:work     # explicit workspace target
/pl-workspace-scan --dry-run                # report what would change, no writes
```

Source: `skills/src/pl-workspace-scan.md`

---

## Reference Workflows

These skills wrap individual `planar` subcommand domains to give agents a consistent, documented entry point for common operations.

### `/pl-init`

Initialize the Planar database and register the current directory as a project. Run once per repo.

**Example:** `/pl-init`

Source: `skills/src/pl-init.md`

---

### `/pl-scope`

Inspect the cwd-derived scope and propose associations from git remote and path. Scope is derived from cwd; cd into the target repo or pass `--scope <slug>` on individual verbs to override.

**Example:**
```
/pl-scope show               # print the cwd-derived scope set
/pl-scope show --json        # machine-readable shape
/pl-scope suggest            # candidate associations for this cwd
```

Source: `skills/src/pl-scope.md`

---

### `/pl-plan`

Manage the full plan lifecycle: create and update plans, add/complete/skip/link
ordered steps, link related entities, select claim-aware next work, recommend an
execution strategy, repair derived status, and close out eligible plans. Writes
use the cwd-derived scope (or an explicit `--scope <slug>`), and every mutation
is verified with `plan show --json`; closeout is previewed before application.
Claimed agent work still ends through one atomic `planar-agent` terminal verb,
never `task done` followed by claim release.

**Example:**
```
/pl-plan create "Design the billing export schema"
/pl-plan step add <plan-id> "Define the export contract"
/pl-plan next <plan-id>
/pl-plan recommend-strategy <plan-id>
/pl-plan closeout <plan-id> --dry-run
```

Source: `skills/src/pl-plan.md`

---

### `/pl-task`

Manage the full task lifecycle within the cwd-derived scope: create, inspect,
update, prioritize, block, reopen, complete, cancel, link, and record repository
touches. After every mutation the skill reads back `task show --json` (and
`task touches list --json` for touches), verifies status and relationships, and
reports `next_action` plus an executable next plan command. Manual `task done`
is only for unclaimed work; claimed agent work uses one atomic
`planar-agent complete|fail|release|block` terminal operation.

**Example:**
```
/pl-task add "implement CSV serialiser" --plan <plan-id>
/pl-task block <task-id> --on <blocker-id>
/pl-task reopen <task-id>
/pl-task touches add <task-id> billing-api --path src/export/
/pl-task done <unclaimed-task-id>
```

Source: `skills/src/pl-task.md`

---

### `/pl-question`

Capture open questions during a session, answer them, and link to tasks and specs.

**Example:**
```
/pl-question add "Which date format for export timestamps?" --task <task-id>
/pl-question answer <question-id> "ISO 8601 UTC, no timezone offset"
```

Source: `skills/src/pl-question.md`

---

### `/pl-knowledge`

Manage durable decisions, artifacts, anchored annotations, and typed entity
relationships through one intent-oriented workflow. The skill resolves every
natural-language target to one typed entity before writing, honors entity scope
guards, reads the changed state back, and reports relationships in
`kind:id --[relationship]--> kind:id` form. It composes rather than replaces
the `planar decision`, `artifact`, `annotate`, and `links` domains.

**Example:**
```
/pl-knowledge capture "Adopt SQLite WAL" --plan 42 --artifact 17
/pl-knowledge annotate --anchor-path src/db/db.zig --line-start 88 "Explain the retry boundary"
/pl-knowledge link annotation:12 plan:42 --relationship addresses
/pl-knowledge link decision:9 artifact:17 --relationship cites
```

Source: `skills/src/pl-knowledge.md`

---

### `/pl-scenario`

Author test scenarios from a spec or task, verify them, and record outcomes.

**Example:**
```
/pl-scenario add --task <task-id> "Export with 10k rows completes in under 5s"
/pl-scenario pass <scenario-id>
```

Source: `skills/src/pl-scenario.md`

---

### `/pl-promote`

Surface personal entities that have matured and promote or demote them between scopes.

**Example:**
```
/pl-promote task:<task-id> --to assoc:project:my-app
/pl-promote demote task:<task-id>
```

Source: `skills/src/pl-promote.md`

---

### `/pl-sync`

Pull from and push to the operational plane, inspect field-level conflicts, and
coordinate explicitly approved reconciliation. For each currently conflicted
link, the skill combines link status, the latest unresolved conflict event from
the link's audit trail, and current entity state. The event evidence must expose
both observable values, their provenance and observation times, and a non-empty
provider version; incomplete, stale, or contradictory evidence forces
`defer`.

The large-tier, coordinate `sync-reconciler` recommends exactly one
disposition:

| Disposition | Effect |
|---|---|
| `keep-local` | After approval, push the complete current local entity with `planar sync resolve <event-id> --keep local ...`; this is not a field-level patch. |
| `keep-remote` | After approval, overwrite the local entity with the complete observed remote entity using `--keep remote`; this is not a field-level patch. |
| `manual-merge` | Do not resolve yet. The operator reviews a proposed merged value, edits through the entity's normal guarded `planar <kind>` workflow, reviews the resulting local post-state, and then separately confirms `keep-local` for that event. |
| `defer` | Write nothing because evidence is insufficient or resolution was declined or postponed; refresh with a guarded pull before rebuilding evidence. |

Reconciliation is read-and-recommend by default. Before either whole-entity
resolution, the operator must see both values and provenance, the exact event,
recommended disposition, rationale, whole-entity effect, and proposed command,
then explicitly approve that event and disposition. Approval applies only to
the displayed evidence and is invalid if the event, local version, token, or
remote evidence changes. The guarded resolve passes the approved evidence token
and reviewed local `updated_at`; the skill and specialist never edit SQLite,
invoke an adapter directly, bypass scope checks, or synthesize direct local or
remote field mutations.

`manual-merge` has two distinct gates: approval of proposed merge text does not
authorize the local edit, and approval of the guarded local edit does not
authorize pushing it. After the operator performs the edit, the workflow shows
the exact local post-state and waits for a second confirmation naming the
conflict event and `keep-local` before resolving.

Every applied resolution is verified by rereading `sync status`, the link's
audit trail, and the entity. Success requires the conflict to be closed, the
returned `new_event_id` to identify a matching resolution event, and entity
post-state to match the approved whole-entity effect; command exit alone is not
enough.

**Example:**
```
/pl-sync pull --all --system my-jira
/pl-sync push task:<task-id> --system my-jira
/pl-sync status
/pl-sync resolve <event-id>
```

Source: `skills/src/pl-sync.md` · `agents/sync-reconciler.md`

---

### `/pl-ext-create`

Create a Jira or GitHub Issues counterpart from a local entity and record the external link.

**Example:**
```
/pl-ext-create my-gh --from task:<task-id>
```

Source: `skills/src/pl-ext-create.md`

---

### `/pl-audit-trail`

For a given external link, show every local session, decision, and commit tied to it.

**Example:**
```
/pl-audit-trail my-jira:PROJ-1234
```

Source: `skills/src/pl-audit-trail.md`

---

### `/pl-resume`

Resume an in-flight task from zero conversational context. Validates resume readiness first (checks that `next_action` is set, no unresolved questions block progress).

**Example:**
```
/pl-resume <task-id>
```

Source: `skills/src/pl-resume.md`

---

### `/pl-handoff`

Capture a context snapshot before terminating, validate it is resume-ready, and write the handoff record.

**Example:** `/pl-handoff`

Source: `skills/src/pl-handoff.md` · `agents/methodology.md` § Heartbeat status contract

---

### `/pl-help`

Route an operator outcome to an available workflow, or show the exact CLI help
for a named verb. Intent routing covers interrupted-work recovery, active-work
observation, durable knowledge, operator-local skills and agents, feedback,
and published-documentation maintenance. It distinguishes invocable workflows
from CLI fallbacks when a planned skill is not yet authored, and always exposes
the underlying supported CLI commands.

**Example:**
```
/pl-help
/pl-help resume interrupted work
/pl-help inspect active agents
/pl-help task
/pl-help sync resolve
```

Source: `skills/src/pl-help.md`

---

### `/pl-health`

Read and explain global Planar health, including database reachability, schema
currency, SQLite integrity, resumability, stale handoffs/claims, and installed
projection freshness. Degraded contributors route to executable, read-first
diagnostics for `/pl-doctor`, `/pl-resume`, claim reconciliation, explicit
projection repair, or configuration validation. Health never performs those
repairs itself, and all state access stays behind `planar` or `planar-agent`
CLI verbs rather than direct database, config-file, or install-tree inspection.
Manifest-owned projections report exact repair commands when stale, missing,
legacy, invalid, or unsupported, while unmanaged local extensions and
unselected vendors do not degrade health.

**Example:**
```
/pl-health
/pl-health --json
```

Source: `skills/src/pl-health.md`

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

Answer what needs attention now in the current scope: sync conflicts, stale
claims and handoffs, blocked work and open questions, active claims/actions,
then claim-aware next work. Empty sections are suppressed and the read-only
summary stays concise while providing executable next actions. It resolves
scope and state only through the CLI, filters global handoffs back to the
current scope, and never recommends work marked claimed, stale, or blocked.

**Example:** `/pl-status`

Source: `skills/src/pl-status.md`

---

### `/pl-observe`

Build a read-only operational snapshot for one plan: its action topology,
active and stale claims, recent failures, sync events, and attributable
handoffs. Use `/pl-observe` for “what has been happening on this plan?” and
`/pl-status` for “what needs attention or should I do next?” The workflow reads
through `planar` and `planar-watch`; their individual verbs remain available
for direct drill-down or continuous following.

**Example:**
```
/pl-observe --plan 808
/pl-observe --plan 808 --since 2026-07-13T00:00:00Z --limit 50
planar-watch log --task 4889 --limit 50 --json
```

Source: `skills/src/pl-observe.md`

---

### `/pl-templates`

Inspect, validate, and render Planar JSON templates for external-system propagation (Jira, GitHub Issues, GitHub Projects).

**Example:**
```
/pl-templates list
/pl-templates validate
/pl-templates render task:<task-id> --system my-jira
```

Source: `skills/src/pl-templates.md`

### `/pl-models-config`

Guided inspection and configuration of per-role model routing. It shells the supported CLI surfaces (`planar models list|routing|candidates|evals|refresh|apply` and `planar config show|validate`) rather than reading or writing config directly. Use it to see installed provider CLIs, effective role -> vendor/model resolution, tier candidate lists, work-type routing, and read-only routing evals.

`models evals` is advisory only: it aggregates completed dispatch history from `session_entries`, `agent_work_claims`, and test-coder `agent_actions`, reports insufficient-data rows honestly, and writes nothing. Any recommended routing-map change requires an explicit operator-confirmed edit to `~/.planar/config.toml`, followed by `planar models candidates --json` verification.

Source: `skills/src/pl-models-config.md`

---

## Documentation Maintenance

The documenter and doc-author roles and the `/pl-documenter` / `/pl-doc-maintain`
skills that drive the gated documentation-maintenance loop were raised to
tabularium (the stack's standalone documentation tool, which owns the manifest
database they operate) at the doc-cluster transfer (planar plan 933). They no
longer render or install from this repo — see tabularium's own skill and agent
sources for those entry points. Planar retains and drives the primitives they
compose: the `planar-agent` claim ritual and the underlying `tabularium diff` /
`cover` / `nodoc` / `lint` / `build` / `verify` tool verbs. For the operator
recipes see [`workflows.md` Recipe 33](workflows.md#recipe-33--run-the-gated-documentation-maintenance-loop)
and the Phase 6 lifecycle in [`operations.md`](operations.md#2-the-orchestration-lifecycle).

---

## Usage Introspection

### `/pl-introspect`

Preview a usage-introspection pass over the structurally redacted
`planar report --json` bundle and recognized Claude, Codex, Copilot, and CLI-log
sources. Adapters normalize only vendor, verb path, category, count, and time
window; transcript prose, argument values, entity titles, scope slugs, and raw
paths are discarded. Missing or malformed optional sources degrade
`signal_coverage` instead of being reported as observed zero.

Preview is the default and writes nothing. After the operator reviews the
proposals, an explicit apply gate creates or reuses the association's
`planar-feedback` plan and files only approved findings. Repeated signal is
deduplicated; a multi-finding failure preserves verified completed rows and
returns `partial` with per-finding recovery rather than claiming rollback.

**Arguments:** `[--days <n>] [--scope <scope>]`

- `--days <n>` — report window (default 30).
- `--scope <scope>` — association scope for the feedback plan (default: cwd-derived).

**Example:**
```
/pl-introspect
/pl-introspect --days 7
/pl-introspect --scope assoc:my-org
/pl-introspect --apply                  # still pauses for explicit confirmation
```

**Finding taxonomy:** `failure-cluster`, `retry-pattern`, `abandoned-workflow`, `gap-feature`.

**Title convention:** `<taxonomy-key>: <signal-key>` (e.g. `retry-pattern: task add`). Titles are deterministic and signal-derived — re-runs over the same signal are idempotent via title-based dedup.

**Privacy:** Transcript text is ephemeral and never persisted. Finding bodies carry only aggregate signal (counts, verb paths, error categories). See [Usage Introspection Privacy Model](concepts.md#usage-introspection-privacy-model).

Source: `skills/src/pl-introspect.md` · `agents/introspector.md`

---

### `/pl-feedback-triage`

Review redacted task/question findings, obtain a deterministic
`feedback-triager` assessment, and preview structured severity, disposition,
reproduction, duplicate, entity, and relationship changes. The initial request
never counts as confirmation: local apply requires an explicit row-level gate,
and optional external reporting routes through `pl-report-issue` with a second
complete issue-body preview. Declining external publication preserves verified
local triage and posts nothing.

Structured triage is inspected with `planar feedback triage list|show` and
applied with `planar feedback triage set` only after approval. Independent
approved findings may complete when another fails; the result is `partial`
with exact inspection or retry commands for failed targets.

**Example:**
```
/pl-feedback-triage --plan 812
/pl-feedback-triage --finding question:42
/pl-feedback-triage --finding task:17 --apply
```

Source: `skills/src/pl-feedback-triage.md` · `agents/feedback-triager.md`

---

## Agent Role Specs

The vendor-neutral role specs live under `agents/`. Vendor skill files defer to them for the authoritative behavior description. The orchestrator, coder, reviewer, research, test-coder, and janitor roles — plus their companion methodology, doctrine, cross-scope-writes, and model-tier-routing docs — live here too (raised to armarium at plan 918/929, returned at the armarium reintegration). The documenter and doc-author roles live in tabularium (which owns the doc-system tool they drive; moved at the doc-cluster transfer, planar plan 933); see tabularium's own agent sources for those.

| File | Role |
|------|------|
| `agents/orchestrator.md` | Orchestrator role: full-lifecycle software-delivery dispatch, phase gates, strategy/isolation/dispatch-shape selection |
| `agents/coder.md` | Coder role: implements one task per dispatch, claim-ritual discipline, work-complete report |
| `agents/reviewer.md` | Reviewer role: blind-read review of coder output, approve/request-changes/open-question/abort |
| `agents/research.md` | Research role: read-only cited findings-brief investigation |
| `agents/test-coder.md` | Test-coder role: verification-only coverage expansion between coder and reviewer |
| `agents/janitor.md` | Janitor role: delivery verification, integration, reconciliation, closeout |
| `agents/methodology.md` | Shared coordination contract: claims, strategies, barrel modes, worktrees, heartbeat status |
| `agents/doctrine.md` | Cross-role doctrine: dispatch-shape selection, common defects, operator feedback contract |
| `agents/models.md` | Model-tier routing: the hand-maintained Tier Table and routing axes |
| `agents/cross-scope-writes.md` | Cross-scope write visibility doctrine |
| `agents/spec-reviewer.md` | Spec reviewer role: adversarial planning review, open-question reconciliation, feature/test gap analysis, and evidence-backed classification of the four recurring hazard lenses |
| `agents/planner.md` | Planner role: input/output contract, document shape, workbench seeding |
| `agents/ingestor.md` | Ingestor role: parsing contract, idempotency invariant, preview-first rule |
| `agents/ext-sync.md` | Ext-sync role: strategy-selection contract, propagation walk, idempotency |
| `(raised to tabularium)` | Documenter and doc-author roles — read-only documentation drift classifier and operator-approved prose author — raised to tabularium at the doc-cluster transfer (planar plan 933) |
| `agents/introspector.md` | Introspector role: cross-vendor redacted signal adapters, preview/apply gate, finding taxonomy, dedup contract, feedback-plan bootstrap |
| `agents/feedback-triager.md` | Feedback triager role: deterministic severity and disposition guidance, reproduction evidence, preview/apply gate, local mutation boundary, and status/result contracts |
| `agents/sync-reconciler.md` | Large-tier coordinate role: compares local and remote conflict evidence, recommends one of four dispositions, and coordinates only the exact whole-entity resolution the operator confirms; it is read-and-recommend by default and never performs direct local or remote field mutation |

---

## Personal sandbox

The repo authors canonical skills under `skills/src/` and canonical roles under
`agents/`; installed vendor projections are generated artifacts. Operators who
want **personal, machine-local skills and agents** — single-purpose workflows
specific to their environment — use the sandbox at
`~/.planar/local/{skills,agents}/`.

Authored sandbox sources are installed (symlink with copy fallback) into every vendor's install directory by `planar local link`. Skills are dir-shape (`~/.planar/local/skills/<name>/SKILL.md`); agents stay flat. Each vendor's discovery loader is shape-specific:

```
skills → ~/.claude/commands/local-<name>.md             (file symlink → <src>/<name>/SKILL.md)
         ~/.codex/skills/local-<name>                   (dir symlink → <src>/<name>/)
         ~/.copilot/skills/local-<name>                 (dir symlink → <src>/<name>/)
agents → ~/.planar/agents/local-<name>.md               (file symlink → <src>/<name>.md)
```

The `local-` prefix on the install name makes sandbox skills visibly user-authored in every vendor's listing and prevents collisions with canonical installs. `shadow: true` in source frontmatter drops the prefix to explicitly replace a canonical install (with a warning at link time). The Codex / Copilot dir-symlink shape is load-bearing: those loaders empirically reject symlinked SKILL.md files inside real directories, but follow directory symlinks correctly. See [concepts.md § Local sandbox](concepts.md#local-sandbox) for the full design.

### `/pl-local`

Canonical lifecycle workflow for operator-local skills and agents: import,
link, list, unlink, migrate legacy sources, and repair vendor-link drift. Every
operation goes through `planar local`, verifies persisted installs with
`planar local list --json`, and reports partial multi-target results without
inventing rollback. The skill-level `repair` operation maps to
`planar local link --reconcile`; there is no standalone `planar local repair`
verb. Sources remain machine-local, and canonical promotion remains the manual
contribution flow—there is no `planar local promote` verb.

**Example:**
```
/pl-local import ~/my-skills/
/pl-local link fixup-protos --vendor codex
/pl-local list
/pl-local unlink fixup-protos
/pl-local migrate --dry-run
/pl-local repair
```

Source: `skills/src/pl-local.md`

---

### `/pl-local-import`

Compatibility entry point that preserves existing import-only invocations and
routes them to the canonical `/pl-local import` contract with the same path,
kind, `--force`, `--dry-run`, and `--no-link` options. Use `/pl-local` for
list, link, unlink, migrate, and repair; the compatibility wrapper adds no
flags or CLI verbs of its own.

**Example:**
```
/pl-local-import ~/my-skills/fixup-protos.md
/pl-local-import ~/my-agents/ --kind agent
```

Source: `skills/src/pl-local-import.md`

---

**Canonical vs sandbox.**

| | Canonical | Sandbox |
|---|---|---|
| Location | `skills/src/` and `agents/` in the repo; generated projections are staged under `$PLANAR_HOME` | `~/.planar/local/{skills,agents}/` on the operator's machine |
| Install | `install.sh` or `make install-full` from the repo checkout | `planar local link` |
| Authoring overhead | Commit the unified source, then run semantic lint and `scriptorium check` against an out-of-tree staging dir | One file, one `planar local link` |
| Distribution | Shipped to everyone using the repo | This operator's machine only |
| Promotion | N/A | Manual: copy file into the repo and follow normal contribution flow. No `planar local promote` shortcut |

**CLI surface:** `planar local list`, `planar local link [--dry-run] [--vendor <v>] [--reconcile] [<name>]`, `planar local unlink <name> [--purge]`, `planar local import <path> [--kind skill|agent] [--force] [--dry-run] [--no-link]`, `planar local migrate [--dry-run]`. Full reference under `docs/cli-reference.md § Domain: local`.

**Manual promotion.** Sandbox skills earn promotion through the standard git contribution flow — no shortcut verb. See `docs/workflows.md § Recipe 14 — Author a personal skill in the sandbox` for the end-to-end walk-through.
