# Planar Concepts

This document explains the core concepts in Planar. Read it after `planar init` and before doing substantive work — the vocabulary here maps directly to CLI commands and database tables.

---

## Binaries

Planar ships as three executables, each with a disjoint capability boundary enforced **by the verb set the binary registers** (not by runtime ACLs). The boundary is a compile-time and install-time property: the binary on PATH literally has no verb for the work it is not allowed to do. This makes vendor-hook blast radius bounded — a hook configured with only `planar-agent` on its PATH cannot mutate planning state regardless of how it is invoked.

| Binary | Audience | Writes to |
|---|---|---|
| `planar` | Operator (human + scripts) | Planning entities (`plans`, `tasks.status` via manual transitions, `decisions`, `questions`, `scenarios`, `artifacts`, `annotations`, …) — everything **except** `agent_actions` / `agent_work_claims`. |
| `planar-agent` | Agent (vendor hook, orchestrator dispatch) + operator recovery | `agent_actions`, `agent_work_claims`, and `tasks.status` (the last only as part of atomic coordinated operations: `pull`, `complete`, `fail`, `release`, `block`). **Never** to plan / decision / question / scenario / artifact / annotation. |
| `planar-watch` | Operator (live view) + scripts (`--json`) | **Nothing.** Opens SQLite via `file:?mode=ro` so the driver itself rejects every write SQL string. |

**Capability invariant — `planar-agent`:** a process invoked as `planar-agent` has no verbs that mutate any planning entity. The verb set is exactly `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`action end`, `ingest`, `reconcile`, `abort`, `version`.

**Capability invariant — `planar-watch`:** the binary's verb set contains zero write verbs (`feed`, `ps`, `claims`, `actions`, `plans`, `log`, `version`, `completion` only). Enforced two ways: (1) the verb set; (2) the read-only DB handle.

Both invariants are locked by `integration_tests/capability_boundary_test.zig` — a future change that registers a write verb on `planar-watch` or a planning-entity verb on `planar-agent` fails CI immediately. The `planar agent <verb>` subcommand namespace deliberately does not exist; agent observability lives on `planar-watch`, agent-table writes live on `planar-agent`.

The ritual every code-writing agent dispatch follows is `planar-agent pull → heartbeat → complete|fail|release|block` (atomic across all three tables). See [agents/methodology.md § Coordination claims](../agents/methodology.md#coordination-claims) and the tech spec § "Agent methodology contract" for the full sequence.

---

## Scope

Scope determines which entities are included in queries by default and where new entities are created when the target is unambiguous from the current working directory.

There are three scope kinds:

| Kind | Meaning |
|------|---------|
| `repo` | Scoped to the current project (resolved from `cwd` against `projects.root_path`). |
| `assoc:<slug>` | Scoped to a named association — typically a `kind=org` workspace, a `client`, an `ad-hoc` grouping, or a `personal` bucket. |
| `global` | No project or association filter — personal, cross-cutting entities. |

Scope is a pure function of `(--scope flag, cwd, db schema)`. There is no ambient stack and no per-process session state to forget about: every invocation resolves from the same two inputs.

### Cwd-derivation: the primary signal

Both the read and write resolvers begin by walking from the current working directory up the filesystem. `DeriveFromCwd` collects every association whose registered root path is a prefix of cwd. Two kinds of root path are matched:

- A member `projects.root_path`. The cwd is inside a registered repo; the association inherits its `kind` (`project`, `host`, `path`, `lang`, etc.) and ranks accordingly.
- An `associations.config_json.root_path` for `kind in ('org', 'client', 'personal', 'ad-hoc')`. The cwd is at (or inside) a workspace root registered via `planar workspace init` or the equivalent assoc creation flow.

Each match becomes a `Candidate` carrying the association id, kind, and the root path that fired.

### Specificity ranking

```
narrowest first:
  1. project
  2. ad-hoc, personal
  3. client
  4. org
  5. host, path, lang  (auto-detected technical)
```

A `project` always outranks an `org` whose membership contains that project, so a cwd inside `~/work/repo-a/` resolves to `project:repo-a` even when `org:work` also matches. At the workspace root (`~/work/`, no project root_path contains it) only `org:work` matches, so the org wins — but at the workspace root writes refuse rather than land in the org by default (see below).

### Write resolution

Writes use the strict `scopearg.ResolveForWrite` algorithm. The first step that yields a single unambiguous scope wins:

1. **Explicit flag.** If `--scope` is passed, parse it and return. The flag is the user's stated intent; never override it.
2. **Cwd derivation, most-specific-wins.** Run `DeriveFromCwd`, rank by specificity, return the single most-specific match. Two refusals fire here:
   - If multiple candidates tie at the best rank, refuse with an `AmbiguousScopeError` listing the tied `--scope` values.
   - If the single winner is `kind=org` and the org has at least one member project, refuse with the workspace-root variant (see below). A zero-member org passes through — that is the workspace-init edge case where writing at the org level is the only sensible choice.
3. **Refuse.** Return an `AmbiguousScopeError`. No silent default; no fallback to ambient state.

### Workspace-root refusal

When cwd lands at a registered workspace root (org `config_json.root_path`) and the org has members, `task add` (or any other write verb without `--scope`) refuses with the candidate list:

```
error: you are in a workspace root with 3 member projects, but no
       specific project scope was passed. Choose one with --scope:

         --scope project:repo-a
         --scope project:repo-b
         --scope project:repo-c

       Or cd into a specific member project. Pass --scope
       assoc:work to write at the org level (cross-repo work).
       Pass --no-scope-check to override (legacy escape hatch).
```

Fan-out is deliberately not the default: a workspace-root `task add` could plausibly mean any of the members or the org itself, and silently picking is the lectio incident pattern. The refusal forces the operator to state which.

### Read resolution

Reads use `scopearg.ResolveForRead`. The contract:

- If `--scope` is set, parse and return that single resolved scope.
- Otherwise run `DeriveFromCwd` and apply the same specificity ranking as the write path. Reads return a `[]Resolved` set, not a single scope:
  - At a workspace root, the set is the org plus every member project (the operator's expectation of "show me everything under this workspace").
  - At a member project root, the set is the project plus any cross-repo entities the project participates in via `touches` links (the polyrepo coordination story from plan 88).
- If cwd matches zero registered scopes and no flag is passed, refuse with a clear message instructing the operator to `cd` into a registered scope or pass `--scope global` for the global slice. There is no silent fallback.

This composes naturally with `task list`, `plan list`, `tree`, `audit-trail`, and `health` — every read verb sees the same cwd-derived set.

### Cross-scope guard

Layered on top of the write resolver, the [cross-scope guard](#cross-scope-guard) compares the operator's resolved scope against each *target entity's* stored `(scope_kind, scope_id)` and refuses if they disagree. The guard is membership-aware: an operator scope `assoc:<org>` covers any entity scoped to one of the org's member projects (via `project_associations`). The reverse — operator project, entity org — still refuses; cross-repo coordination requires either `cd` to the workspace root or `--scope assoc:<org>` explicitly.

### Removed: the active scope stack

Earlier releases maintained a per-database `active_scope` table and exposed `planar scope use`, `planar scope pop`, and `planar scope clear` to manipulate it. Plan 153 M5 dropped the table (migration `src/migrations/0009_drop_active_scope.sql`) and removed the verbs; concurrent sessions sharing one database can no longer trample each other through stack manipulation. Operators who habitually typed those verbs get an exit-1 redirect pointing at `planar scope show`.

**SQLite tables:** `associations`, `project_associations`, `projects`. **Primary verbs:** `planar scope show` (derived view), `planar scope suggest`. Set the scope for any verb by `cd`-ing into the target or passing `--scope <slug>`.

---

## Cross-scope guard

The cross-scope guard is a refusal mechanism that fires when the operator's resolved write scope disagrees with the target entity's stored `(scope_kind, scope_id)`. Guarded verbs read the entity's scope, resolve the operator's scope through the strict write resolver above, compare the two, and refuse with exit 1 if they differ.

### Why it exists

The motivating incident: an operator running from `~/work/lectio/` invoked `planar spec ingest 88` against a plan that belonged to a different project's scope. The verb materialised 126 child entities — plans, tasks, decisions, scenarios — under the cwd's association rather than the anchor plan's. The rows were recoverable but the silent cross-scope materialisation was the bug. The guard exists so verbs that walk *from* a parent entity *to* its children, or that mutate a specific existing entity, refuse to proceed when operator intent and entity provenance disagree.

### Which verbs are guarded

Two classes of verb are guarded:

- **Bulk-write-from-parent.** Verbs that take a parent entity id (typically a plan) and write a tree of derived rows. The parent's scope is the natural scope for the derived rows; running from a cwd that resolves to a different scope is the lectio incident pattern. Verbs: `spec ingest`, `ext propagate`, `sync push <link|kind:id>`, `sync pull <link|kind:id>`, `sync resolve`.
- **Mutating-existing-entity.** Verbs that take an existing entity id and rewrite it (or its links). Routing such a mutation through the wrong cwd updates the row but skews future writes that follow the same code path. Verbs: `plan update`, `plan step add/done/skip`, `task update/done/block` (both endpoints on `block`), `question update`, `scenario update`, `decision update`, `decision supersede` (both old and new), `artifact update`, `audit publish-decision`, `ext create --from`, `link <kind:id> --to`, `unlink`, `links update`.

### Membership-aware coverage

The comparison is not strict equality. An operator scope `assoc:<org>` covers any entity whose stored scope is a project that belongs to the org via `project_associations`. From a workspace-root cwd with `--scope assoc:work` (or via the workspace-root opt-in flow), the guard accepts writes targeting `project:repo-a`, `project:repo-b`, etc. — the org operator is "above" its member projects. The reverse direction (operator `project:repo-a`, entity in `assoc:work`) still refuses; cross-repo coordination requires explicit org scope.

### Which verbs are deliberately not guarded

Several verb classes were audited and explicitly left unguarded; the absence is not an oversight:

- **All `*_link` and `links add/remove` verbs.** `entity_links` is cross-entity by design — the polyrepo `touches`/`derives-from` story depends on edges crossing scope boundaries.
- **All `*_add` / `*_create` verbs.** A newly-created entity has its own `--scope` resolved through the write resolver; `--plan` or `--parent` on a create verb is a reference, not scope inheritance.
- **`sync push --all` / `sync pull --all`.** Bulk fan-outs that the operator opts into explicitly.
- **All read-only verbs.** `show`, `list`, `status`, `audit trail`, `tree` — reads do not corrupt state and the audit-from-anywhere case is the common case.
- **Identity-bucket verbs** (`assoc`, `init`, `promote`/`demote`, `scope`, `workspace`). Associations *are* scope; `promote`/`demote` deliberately cross scopes (that is the verb's job).
- **Operator-state verbs** (`handoff`, `capture`, `resume`). These manage vendor-session rows, not project-scoped entities. The legitimate polyrepo handoff workflow is "a session inside repo A captures a handoff that references a task in repo B".

### Escape hatch

`--no-scope-check` downgrades the refusal to a one-line stderr warning and proceeds. Appropriate uses: legacy scripts that cannot be updated immediately, one-off corrections after a scope-rewrite migration, and ad-hoc fix-ups. Inappropriate uses: routine workflows, skills, agents, orchestrator code. Reach for `--scope <slug>` first to assert explicit intent; `--no-scope-check` is the documented fallback when an explicit scope is not yet known.

See [docs/cli-reference.md § Cross-scope guard](cli-reference.md#cross-scope-guard) for the per-verb listing and the exact refusal message format.

---

## Association

An association is a named grouping of projects (repos). A single repo can belong to multiple associations. Associations are the main organizational primitive for work that spans more than one repo.

Associations have a `kind` that describes how they were formed:

| Kind | Meaning |
|------|---------|
| `org` | GitHub org or similar top-level org grouping |
| `project` | A cross-repo product or feature initiative |
| `client` | External client engagement |
| `personal` | Personal projects or scratch work |
| `ad-hoc` | Temporary grouping |
| `host` / `path` / `lang` | Auto-detected from git remote host, parent directory, or language ecosystem |

The `slug` on an association is the stable identifier used in scope references, config overrides, and workbench directory names. It must match `[a-z0-9:._-]+`.

`planar assoc detect` auto-detects associations from the current repo's git remote and parent path. `planar assoc add` creates one manually.

**SQLite table:** `associations`, `project_associations`. **Primary verbs:** `planar assoc add`, `planar assoc list`, `planar assoc detect`.

---

## Project

A project is a registered local working directory — what you'd call a "repo." It is identified by its `root_path` on disk and has a `slug` that is used in scope references (`--scope repo:<slug>`).

`planar init` registers the current directory as a project (or upgrades an existing registration) and applies any pending schema migrations. You only run it once per repo.

Projects are read-only after `init` — the project record is not meant to be updated or deleted. Associations are the mechanism for grouping projects.

**SQLite table:** `projects`. **Primary verbs:** `planar init`, `planar project list`, `planar project show`.

---

## Workspace

A workspace is a polyrepo grouping treated as a first-class operational surface. Mechanically it is an `associations` row of `kind=org` together with its `project_associations` members — no new table, no new schema. What makes it a distinct concept is the operational shape: a directory on disk (`~/work/`, `~/projects/`, etc.) that contains several sibling git repos, and a canonical AGENTS.md surface that coordinates work across them.

### Why the concept exists

Cross-repo work cannot land cleanly in any single project's scope. When a feature touches `repo-a` and `repo-b`, the planning artifacts, decisions, and open questions belong above the repo level. The workspace surface gives that "above-the-repo" content a stable home: an org association for the scope and a state directory for the generated content. Agents working in any member repo can read the workspace's AGENTS.md without having to navigate up.

### State directory and symlinks

The canonical content for a workspace lives at `~/.planar/workspaces/<org_id>/`:

- `AGENTS.md` — generated; the human-readable routing surface.
- `routing-table.json` — generated; structured project map (capabilities, dependencies, summaries, open-work counts).
- `config.toml` — optional; per-workspace settings (`enrich_command`, etc.).
- `routing-table-overrides.json` — optional; operator overrides merged on every routing build.
- `.manifest-docs` — drift manifest (plan 96) tracking the generated files.

Two symlinks at the workspace root (`<workspace-root>/AGENTS.md`, `<workspace-root>/CLAUDE.md`) point at the same canonical `AGENTS.md` target so Codex / Copilot (which read `AGENTS.md`) and Claude Code (which reads `CLAUDE.md`) see identical content. On filesystems that reject symlinks the installer falls back to a regular-file copy and records the degraded mode so regeneration rewrites the copy.

### Two-pass routing

The routing table is built in two passes. The static pass is always-on, deterministic, pure-Go: README first paragraph, manifest detection for capability tags, dependency inference from `go.mod` replace / `package.json` workspace deps, language census, and live Planar focus queries (open tasks, open questions, active plans). The LLM enrichment pass is opt-in via `pl-workspace-scan --enrich` or `planar workspace routing build --enrich`; it merges cached LLM results into the table, keyed by a content fingerprint so unchanged repos do not re-spend tokens. Manual overrides always win over enrichment, which always wins over static signals.

### Bare-init guardrail

`planar init` refuses when cwd has no `.git` but contains child repos — without the guardrail, a bare init in `~/work/` would register a semantically-wrong project row for the workspace directory itself. The refusal points at `planar workspace init`; `--allow-no-repo` is the escape hatch for the rare standalone non-repo case.

See [docs/architecture.md § Workspace State Directory Model](architecture.md#workspace-state-directory-model) for the full layout and [docs/workflows.md § Recipe 12](workflows.md#recipe-12--working-in-a-polyrepo-workspace) for the end-to-end recipe.

**SQLite tables:** `associations` (the `kind=org` row), `project_associations` (member projects). **Primary verbs:** `planar workspace init`, `planar workspace doctor`, `planar workspace routing build`, `planar workspace routing show`, `planar workspace regenerate`.

---

## Transcription vs Synthesis

Two verbs onboard an existing repo into Planar. They share the downstream `/pl-spec-ingest` pipeline but enter from different contracts; the choice is load-bearing.

- `import` is a **transcription** verb. It reads the repo's existing planning docs and emits them as Planar artifacts as-is. Bullets in a roadmap become tasks verbatim, frontmatter dictates classification, and status inference is bounded by an explicit confidence floor. Reach for it when the repo's planning material is clean, structured, current, and largely correlates with shipped code.

- `synthesize` is a **synthesis** verb. It reads both the existing docs *and* the source tree as input, then produces fresh `product_spec` / `tech_spec` / `roadmap` artifacts via an LLM pass. The original docs are preserved on the same anchor plan as `kind=research` reference artifacts — superseded but not deleted. Reach for it when the planning material is scattered across multiple drafts, mid-evolution, or contradicted by reality (e.g. a roadmap claims a milestone is done but no source files back the claim).

The load-bearing rule for `synthesize` is that **code presence beats text claims**. A task the LLM proposes with `status != "todo"` must cite a `code_evidence` path that exists in the probed source tree; the Go validator refuses results that violate the invariant. A roadmap line that says "M3 is finished" is treated as TODO unless source files, tests, or CI configs corroborate the claim. The greenfield case (no source detected) collapses naturally onto all-todo output.

### Picking between the two

| Repo shape | Pick |
|---|---|
| Clean structured docs + recent, code matches docs | `import` |
| Multiple roadmaps, ambiguous statuses, partial implementation | `synthesize` |
| Docs-only, no source code yet (greenfield) | `synthesize` |
| `docs/` + complete code + tests with reliable status correlation | `import` (consider `--interpret` for LLM polish) |

Both verbs land in the same downstream pipeline: artifacts in the workbench, review by the operator, then `/pl-spec-ingest` to decompose into the plan / task graph. The only divergence is at the entry point — what counts as the authoritative planning material.

### Concrete examples

**Greenfield (docs-only).** Your repo has `docs/product-roadmap.md` describing five phases but no source code yet — just the planning material and a README. Run `synthesize`: codeprobe finds zero source-file evidence, the request is flagged greenfield, and every task lands `status=todo`. Run `import` instead and git-log correlation matches the roadmap-adding commits to dozens of phase-1 task titles, marking 100+ tasks done because the commits *added* the roadmaps. The synthesis path makes the false-done problem structurally impossible.

**Docs-with-code (clean).** Your repo has conventional `docs/<project>_<kind>.md` naming, a working `Sources/` tree, and tests under `Tests/`. Run `import` for a faithful transcription that preserves the existing structure exactly. Run `synthesize` for an LLM-curated reorganization grounded in code-evidence: tasks are graded by what the source tree actually shows, and `code_evidence` citations annotate every non-todo task. Both produce similar plans; the choice is whether you want the repo's own structure or a fresh LLM read.

**Mid-evolution (docs lie).** Your roadmap claims Phase 2 is done; your code shows `internal/sessions/` is empty. Run `synthesize`: Phase 2 lands `status=active` (or todo) despite the roadmap claim because the EvidenceMap has `source=none, tests=none` for that area. The original Phase 2 done-claim survives as a `kind=research` reference artifact on the anchor plan, so operators see both narratives — the synthesized version is primary, the original docs are reference. Run `import` here and you'd transcribe the doc-claim verbatim, propagating the false-done into Planar's data model.

See [docs/cli-reference.md](cli-reference.md) for the full `import` and `synthesize` flag tables, and [docs/workflows.md](workflows.md) for end-to-end onboarding recipes for each repo shape ([Recipe 7](workflows.md#recipe-7--adopt-an-existing-repo-into-planar), [Recipe 7a](workflows.md#recipe-7a--greenfield-onboard-with-synthesize), [Recipe 7b](workflows.md#recipe-7b--docs-with-code-onboard-with-synthesize), [Recipe 7c](workflows.md#recipe-7c--mid-evolution-onboard-with-synthesize)).

---

## Plan

A plan is the anchor unit of work. It is a structured intent — a named body of work with a status lifecycle. Plans are hierarchical: a child plan has a `parent_plan_id` and belongs to its parent's feature tree.

### Status lifecycle

```
draft → active → done
          ↕
        paused
          ↓
       abandoned
```

- `draft`: planning documents are being authored. The workbench tree exists but tasks may not yet be created.
- `active`: tasks are being executed.
- `paused`: work is interrupted; resumable.
- `done`: all tasks complete.
- `abandoned`: work stopped without completion.

A plan has a filesystem-safe `slug` unique within its parent scope, used in workbench directory names.

The anchor plan for a feature is the top-level plan with no `parent_plan_id`. Child plans are used for sub-features or roadmap milestones within a larger feature.

**Status auto-promotion (plan 304).** Plan status is a function of task status, enforced at task-write time. The auto-promotion invariant fires inside every `task.Add` / `Update` / `Done` / `Reopen` / `Cancel` / `Block` transaction and applies a transition matrix that flips the plan based on the post-write task aggregate. The matrix lives in [`agents/methodology.md` §Plan-status invariant](../agents/methodology.md#plan-status-invariant).

Two operator-visible consequences:

- **Child plans auto-promote.** A child plan flips `draft → active` when any task starts, and `active → done` when every task is terminal (done or cancelled). The orchestrator no longer needs to walk child plans manually — `planar task done <id>` on the last task flips the parent child plan to `done` in the same transaction.
- **Anchor plans don't auto-promote to done.** The anchor's `done` transition is a release-gate decision; the invariant only auto-flips anchors to `active`. Operators close an anchor explicitly with `planar plan update <id> --status done`. A plan's status also depends only on its OWN tasks — a child plan being done does NOT propagate to its parent anchor's status.
- **Paused and abandoned are operator overrides.** Both are no-ops for the recompute. Re-engage with `plan update --status active`.

The opt-out is `--no-auto-promote` on the task verbs, used by migrations and scripted bulk edits that don't intend the plan-level transition.

Each transition emits a `session_entries` row with `prefix='note'` and a body that begins with the sentinel line `plan_status: <id>` — recoverable via `planar audit trail <plan> --grep "^plan_status:"`.

**SQLite table:** `plans`. **Primary verbs:** `planar plan create`, `planar plan show`, `planar plan list`, `planar plan active`, `planar plan done`, `planar plan abandon`.

---

## Task

A task is the leaf unit of work. It is attached to a plan via `plan_id` and optionally to a parent task via `parent_task_id` (subtasks). Tasks are the entities that agents implement.

### Status lifecycle

```
todo → doing → done
              → cancelled
              → blocked
```

A task in `blocked` status requires `next_action` to be set — `planar resume validate` refuses a resume packet without it. `doing` tasks are the ones currently being worked on by an agent session.

Tasks carry a `title`, an optional `body` (Markdown), a `next_action` field for handoff continuity, and a `scope_kind`/`scope_id` pair that records which scope they belong to.

**SQLite table:** `tasks`. **Primary verbs:** `planar task add`, `planar task list`, `planar task show`, `planar task doing`, `planar task done`, `planar task block`, `planar task cancel`.

---

## Dispatch shapes

The orchestrator's Phase 3 dispatch gate offers six named **dispatch shapes** — each a distinct point on the *grouping* × *reviewer disposition* matrix. The shape picks how many tasks land in one coder cycle AND when (or whether) the reviewer is dispatched. The operator picks one shape per `/pl-orchestrator` invocation at the gate; the choice is recorded as a `session_entries` row (with `prefix='note'` + the sentinel body line `dispatch_shape: <shape>`) for the audit trail.

| Shape              | Grouping                     | Reviewer disposition  | Pick when |
|--------------------|------------------------------|-----------------------|-----------|
| `strict`           | One cycle per task           | Per task              | Fine-grained history; spec/schema changes; logic changes. |
| `grouped`          | Orchestrator-picked grouping | Per group             | Tasks share file scope; one review covers them all. |
| `single`           | All in one cycle             | Once                  | Tiny features where decomposition is theatre. |
| `barrel-grouped`   | Per milestone (locked in)    | Per group             | Throughput + per-group review. Alias for `grouped` with the milestone heuristic. |
| `barrel-deferred`  | Per milestone (default)      | Deferred at boundary  | Throughput + late review safety net. `--barrel-deferred-at plan` for once-per-plan. |
| `barrel-bypass`    | Per milestone (default)      | None                  | Maximum throughput. Quality gates ARE the review signal. |

The three `barrel-*` shapes formalize what was previously an emergent shortcut: barrel through milestones back-to-back, accept the gates as the entire signal, and defer (or skip) the reviewer. Naming them turns the shortcut into a contract.

**Phase 3.5 (test-coder dispatch) fires across all shapes** when uncovered slugs intersect the cycle's slugs. `barrel-bypass` bypasses the *reviewer*, not the *coverage gate*. The test-coder's `failure-surfaced` outcome halts the cycle regardless of mode.

**Iteration-5 cap.** Applies per reviewer dispatch under `strict`/`grouped`/`single`/`barrel-grouped`. Applies to the boundary reviewer dispatch under `barrel-deferred` (on the union diff). Undefined under `barrel-bypass` (no reviewer → no `request-changes` → no iteration).

**Audit trail.** Every cycle emits a `session_entries` row with `prefix='note'` and a structured body that begins with the sentinel line `dispatch_shape: <shape>`. The `session_entries.prefix` CHECK constraint allows a fixed set (`action / observation / decision / question / file / command / note / error / read`); the dispatch convention reuses `note` with the sentinel body as the grep-recoverable alternative — the same pattern used by plan 304's `plan_status:` audit trail.

```
dispatch_shape: <one of the six>
reviewer_disposition: <dispatched|skipped-by-profile|deferred|bypassed>
cycle_scope: plan:N milestone:M | task:T...
tasks: [<id>, <id>, ...]
```

Recover the per-cycle disposition with `planar audit trail <plan> --grep "^dispatch_shape:"`. No schema change; the sentinel-body convention is the contract.

**Pick-when summary:** when in doubt, pick `strict`. Move up the table (toward throughput) when you have high confidence in the gates and the spec, or when the diff cadence makes per-cycle reviewer dispatch wasteful. The orchestrator never picks a barrel mode silently — every shape change is an explicit operator choice at the gate.

For the canonical contract see [`agents/methodology.md` §Barrel modes](../agents/methodology.md#barrel-modes). For the CLI-flag surface see [`docs/cli-reference.md` §`/pl-orchestrator`](cli-reference.md#planar-orchestrator).

**SQLite tables:** none beyond `session_entries`. **Primary entry points:** `/pl-orchestrator` (the gate), `agents/methodology.md` §Barrel modes (the contract), `planar audit trail <plan>` (the forensic surface).

---

## Question

A question is an open inquiry attached to a scope, plan, or task. Agents record open questions rather than proceeding with uncertain assumptions. Questions gate on explicit answers before a task can be marked done.

### Status lifecycle

```
open → answered
     → wontfix
```

The `answered` status requires both `answer_body` and `answered_at` to be set — the schema enforces this with a CHECK constraint.

### Sources

A question entity can be created through two equivalent paths: (1) interactively with `planar question add "…" --plan <id>` at any time during a session, or (2) automatically by `/pl-spec-draft` when it seeds a workbench — the planner scans every drafted artifact for `## Open questions` sections and registers each H3 child heading as a question entity. Both paths produce an identical `questions` row; the two sources are interchangeable and resolve through the same lifecycle. `/pl-spec-ingest` reconciles spec-body question items against existing entities on every run, surfacing drift warnings when the spec and the entity table diverge. See the "Reviewing open questions" recipe in `docs/workflows.md` for a full walkthrough.

**SQLite table:** `questions`. **Primary verbs:** `planar question add`, `planar question list`, `planar question answer`, `planar question wontfix`. **Related verb:** `planar workbench extract-questions` (reads spec bodies; used internally by the planning skills).

---

## Scenario

A scenario is a verification test case attached to a spec, plan, or task. Scenarios are either auto-drafted by the planner agent from spec acceptance criteria or hand-written by the user.

The ingestor imports scenario sections from `tech-spec.md` into `test_scenarios` rows. The coder agent runs scenarios after implementation and records pass/fail outcomes.

**SQLite table:** `test_scenarios`. **Primary verbs:** `planar scenario add`, `planar scenario list`, `planar scenario pass`, `planar scenario fail`.

---

## Decision

A decision is a recorded design choice. Decisions have a `kind` (`design`, `technical`, `process`) and a status lifecycle:

```
proposed → accepted
         → superseded
         → withdrawn
```

Architecture decision records (ADRs) are artifacts of `kind=adr`, not decision rows — the `decisions` table is for in-flight design choices made during feature work. A decision row points to the session in which it was made and optionally to a plan.

**SQLite table:** `decisions`. **Primary verbs:** `planar decision add`, `planar decision list`, `planar decision accept`, `planar decision supersede`.

---

## Artifact

An artifact is a long-form prose document attached to a plan. Artifacts are the canonical location for specs, ADRs, design notes, and generated outputs.

### Artifact kinds

| Kind | Meaning |
|------|---------|
| `tech_spec` | Technical specification |
| `product_spec` | Product intent and user stories |
| `adr` | Architecture decision record |
| `design_note` | Design exploration or spike output |
| `summary` | Session summary or retrospective |
| `readme` | README-style overview |
| `roadmap` | Flat milestone list (consumed by the ingestor) |
| `generated` | Machine-generated output (diffs, reports) |
| `other` | Catch-all |
| `research` | Academic-tone investigation note. Has its own template. |
| `getting_started` | Onboarding doc, imperative tone. |
| `changelog_entry` | Single changelog entry; aggregated into a published changelog by the regenerator. |
| `glossary_term` | Single term definition; aggregated into a glossary page. |

Artifacts are stored as Markdown in the `body` column and mirrored to the workbench filesystem as `.md` files under the plan's workbench directory.

**SQLite table:** `artifacts`. **Primary verbs:** `planar artifact add`, `planar artifact show`, `planar artifact list`, `planar artifact update`.

---

## Workbench

The workbench is the bidirectionally synced filesystem view of an anchor plan and all its entities. Each anchor plan gets a directory at `$PLANAR_WORKBENCH_ROOT/<assoc-slug>/p<id>-<slug>/` (default root: `~/.planar/workbench/`).

The workbench is the primary drafting surface for agents and users. The planner agent writes documents into it. The user reads and edits them. The ingestor reads them back to decompose into tasks. Agents working on tasks write their outputs as artifact files in the workbench.

Sync is always explicit:
- `planar workbench push <plan>` — DB → FS
- `planar workbench pull <plan>` — FS → DB
- `planar workbench sync <plan>` — bidirectional

When a feature is complete, `planar workbench archive <plan>` removes the FS tree (the DB retains everything). `planar workbench restore <plan>` recreates it.

**SQLite table:** `workbench_sync_state` (tracks per-file sync state). **Primary verbs:** `planar workbench push`, `planar workbench pull`, `planar workbench sync`, `planar workbench status`, `planar workbench archive`, `planar workbench restore`, `planar workbench publish`.

---

## Entity Link

An entity link is a typed cross-entity relationship. Any two entities of any kind can be linked. Links are stored in the `entity_links` table.

### Relationship types

| Type | Meaning |
|------|---------|
| `derives-from` | This entity was derived from the referenced entity |
| `blocks` | This entity cannot proceed until the referenced entity is resolved |
| `addresses` | This entity addresses (resolves or mitigates) the referenced entity |
| `verifies` | This entity (typically a scenario) verifies the referenced entity |
| `cites` | This entity references the referenced entity for context |
| `supersedes` | This entity replaces the referenced entity |
| `touches` | This entity modifies or depends on the referenced entity (typically a task touching a repo) |

The `touches` relationship is particularly important for ext-sync: it records which repos a task touches, which is the input to the GitHub strategy-selection algorithm.

**SQLite table:** `entity_links`. **Primary verbs:** `planar links add`, `planar links list`, `planar links remove`.

---

## External Link

An external link binds a local entity to a ticket in an external system (Jira, GitHub Issues). External links are recorded in `external_links`.

The `link_role` column distinguishes the relationship kind:

| Role | Meaning |
|------|---------|
| `mirror` | This external item was created by Planar's `ext propagate` and tracks the local entity one-to-one |
| `reference` | This external item was created independently; the link is informational |

Each `external_links` row also carries a `config_json` blob used by the ext-sync engine to cache per-feature propagation state (selected GitHub strategy, Projects v2 node id, etc.). This is what makes strategy selection sticky across re-propagation runs.

`planar link <entity> <system-slug>:<external-id>` creates a reference link manually. `planar ext propagate <plan>` creates mirror links automatically.

**SQLite table:** `external_links`, `external_systems`, `sync_events`. **Primary verbs:** `planar link`, `planar unlink`, `planar ext propagate`, `planar ext create`, `planar sync pull`, `planar sync push`.

---

## Session

A session is a durable agent-execution record. One session = one vendor's episode of work, scoped to a task and project. Sessions are the backbone of agent continuity — they make it possible to resume work after compaction, a vendor switch, or a process restart.

Every command that writes to the database appends a `session_entries` row to the current active session for the affected task. If no session exists, one is auto-created using the `PLANAR_VENDOR` environment variable.

A **handoff** record is written at the end of a session via `planar handoff`. It includes a `context_snapshots` row (the resume packet) and the metadata needed for the receiving agent to pick up where the previous one left off.

`planar resume <task-id>` assembles the resume packet — task state, recent session entries, open questions, decisions made, and next action — into a structured prompt the new agent session reads at startup.

**SQLite tables:** `sessions`, `session_entries`, `context_snapshots`, `handoffs`. **Primary verbs:** `planar capture session`, `planar capture end`, `planar capture note`, `planar handoff`, `planar resume`.

---

## Templates layer

Planar ships per-entity-kind Markdown templates that seed new entities created via the editor-first `add` verbs. The shipped set lives in the repo under `templates/entity/` and is installed to `~/.planar/templates/entity/` by `install.sh` (existing files are preserved on re-install; `--force` overwrites).

Layout:

```
~/.planar/templates/entity/
├── artifact/
│   ├── product_spec.md
│   ├── tech_spec.md
│   ├── roadmap.md
│   ├── research.md
│   ├── decision-record.md
│   └── default.md       # fallback for unrecognized artifact subkinds
├── decision.md
├── task.md
├── question.md
└── scenario.md
```

Each template is a Markdown file with a YAML frontmatter block plus a short body skeleton. Placeholders use Go `text/template` syntax: `{{.Title}}`, `{{.PlanID}}`, `{{.PlanSlug}}`, `{{.ArtifactKind}}` (artifact templates only), `{{.Priority}}` (task template only), `{{.Date}}`. The loader (`internal/editflow/templates`) renders with `Option("missingkey=error")` so a typo surfaces immediately.

**Customizing.** Operators edit the installed file directly; `install.sh` does not clobber an existing template unless run with `--force`. Per-workspace template overrides are a future seam tracked in the M2 tech-spec.

**Validation.** A unit-time `LintAll` renders every shipped template against a sentinel `Vars` to catch placeholder typos. A richer schema-aware validator at install time is tracked as a follow-up (task 740).

**SQLite tables:** none — templates are pure filesystem assets. **Primary entry points:** `templates.Resolve`, `templates.Render`, `templates.LintAll` (Go package, not CLI surface).

## Color output

`planar tree`, the per-entity `list` verbs (`plan list`, `task list`, `question list`, `decision list`, `artifact list`, `scenario list`), and the child-plan summary section of `plan show` colorize the status column based on the entity kind that owns the status. Status is the only field colorized; titles, IDs, dates, and relationship arrows stay plain.

**Palette families.** The palette uses six visual categories from the 8-color ANSI set (universal across modern terminals: macOS Terminal, iTerm2, kitty, alacritty, Windows Terminal). The map is keyed by `(entity_kind, status)` so the same status text on different entities can take different colors.

| Family | Color | Statuses |
|---|---|---|
| Live in-flight | bold cyan | `plan.active`, `task.doing`, `plan_step.in-progress` |
| Ready / open | default | `task.todo`, `question.open`, `decision.proposed`, `plan.draft`, `artifact.draft`, `scenario.draft`, `annotation.active`, `plan_step.pending` |
| Done / accepted | green | `plan.done`, `task.done`, `question.answered`, `decision.accepted`, `scenario.verified`, `annotation.resolved`, `scenario_outcome.pass`, `artifact.active`, `plan_step.done` |
| Caution | yellow | `plan.paused`, `task.blocked`, `scenario.failing`, `scenario.ready` |
| Hard fail | red | `scenario_outcome.error`, `scenario_outcome.fail`, `question.wontfix` |
| Terminal-dim | gray | `plan.abandoned`, `task.cancelled`, `annotation.dismissed`, `annotation.archived`, `artifact.superseded`, `artifact.retired`, `decision.superseded`, `decision.withdrawn`, `scenario.retired`, `scenario_outcome.skipped`, `plan_step.skipped` |

**Mode precedence (highest wins).**

1. `NO_COLOR` env var (any non-empty value) — never emit color
2. `--no-color` or `--color=never` — never emit color
3. `--color=always` — emit color
4. `--color=auto` (default) and stdout is a TTY — emit color
5. otherwise — do not emit color

**JSON bypass.** The `--json` output path never calls into the color helper. The bypass is structural, not toggle-based: scripts piping JSON receive byte-identical output regardless of `--color` or `NO_COLOR`.

**Drift gate.** A unit test in `src/internal/output/color/` walks every entity package's `Statuses` slice (`plan.PlanStatuses`, `plan.StepStatuses`, `task.Statuses`, `question.Statuses`, `decision.Statuses`, `artifact.Statuses`, `scenario.Statuses`, `scenario.Outcomes`, `annotate.Statuses`) and asserts a palette entry. A future migration that adds a new status without updating the palette fails the test and the build is red.

**SQLite tables:** none — color is a pure rendering concern. **Primary entry points:** `color.Configure`, `color.Status`, `color.PadRight` (Go package, not CLI surface).

## Local sandbox

A user-local authoring surface for personal skills and agents under `~/.planar/local/`. The sandbox is one-way: the operator drops a single skill (as a `<name>/SKILL.md` directory) or agent (as a flat `<name>.md` file), then `planar local link` fans installs out to each vendor's surface (`~/.claude/commands/`, `~/.codex/skills/`, `~/.copilot/skills/`). Edits to the source SKILL.md are instantly live in every vendor — see *Per-vendor install layout* below.

**Directory layout.**

```
~/.planar/local/
├── skills/
│   ├── fixup-protos/
│   │   ├── SKILL.md              # the source-of-truth file
│   │   └── helper.sh             # optional auxiliary files travel with the skill
│   └── .link-manifest.json       # written by `planar local link`
└── agents/
    └── pedantic-reviewer.md      # agents stay flat — no vendor loader for agents
```

Skills are dir-shape because the dir-symlink installs into Codex / Copilot need a real directory to symlink. Auxiliary files inside the skill directory (helper scripts, data files, icons) travel with the skill — they appear inside the vendor install directory via the same directory symlink. Agents stay flat: they install into `~/.planar/agents/` which has no vendor loader, so the directory wrapping buys nothing.

A file's parent directory tree is authoritative for its kind: a `<name>/SKILL.md` under `skills/` is a skill regardless of what its frontmatter says. The `kind:` field in frontmatter (when present) must match the directory, or the file is rejected as malformed.

**Migrating from the legacy flat layout.** A pre-reshape sandbox stored each skill as a flat `~/.planar/local/skills/<name>.md`. Run `planar local migrate` to convert these to the dir-shape `<name>/SKILL.md` layout. `planar local link` flags any remaining flat skill files with a warning pointing at the migrate verb.

**Frontmatter schema.** The frontmatter mirrors the canonical skill convention used under the repo's `commands/claude/`. Sandbox-specific keys are `shadow:` and `vendors:`; vendors ignore them.

```yaml
---
description: "Rebuild and re-import Go protos in the current repo"
argument-hint: "<optional usage hint>"
tier: medium                    # small | medium | large; maps to model
model: claude-opus-4-7          # optional explicit model override
shadow: false                   # true → link without the local- prefix (shadows canonical)
vendors:                        # subset of {claude, codex, copilot};
  - claude                      #   defaults to all three if omitted
  - codex
---

# Body content
The agent should...
```

**Link semantics.** Linked filenames default to `local-<source-name>` so sandbox installs are visibly user-authored and never collide with canonical skills. With `shadow: true`, the prefix is dropped and the install replaces the same-named canonical install (with a warning at link time naming what's being shadowed). The link package writes a `.link-manifest.json` per `<kind>/` directory recording the per-vendor targets so unlinking is fast and self-documenting.

**Per-vendor install layout.** Each vendor's discovery loader is shape-specific, so the link layer installs three different shapes:

| Vendor  | Install entry                                       | Shape | Edit-and-live |
|---------|-----------------------------------------------------|-------|---------------|
| claude  | `~/.claude/commands/local-<name>.md`                | file symlink → `<src>/<name>/SKILL.md` | yes |
| codex   | `~/.codex/skills/local-<name>`                      | **directory symlink** → `<src>/<name>/` | yes |
| copilot | `~/.copilot/skills/local-<name>`                    | **directory symlink** → `<src>/<name>/` | yes |
| agents  | `~/.planar/agents/local-<name>.md`                  | file symlink → `<src>/<name>.md` | yes |

The dir-symlink shape for Codex and Copilot is load-bearing. Empirically their discovery loaders stat each entry in the skills directory and read the SKILL.md inside — a symlinked SKILL.md *inside* a real directory is treated as missing, while a symlinked *directory* pointing at a real source dir is followed correctly. The sandbox keeps skills as `<name>/SKILL.md` source dirs so the dir-symlink resolves to a real file.

`planar local unlink` removes the symlink itself; the underlying source directory at `~/.planar/local/skills/<name>/` is left untouched. Use `unlink --purge` to also delete the source.

**Promotion is manual.** No `planar local promote` shortcut. A skill earning a place in the canonical repo means going through the normal git contribution flow: copy the file into `skills/src/`, run `make render`, commit, push, and let `make render-check` plus any remaining relevant validators gate it. The absence of a shortcut is deliberate — canonical and sandbox have different bars.

**SQLite tables:** none — the sandbox is filesystem state. **Primary entry points:** `sandbox.WalkSandbox`, `sandbox.Migrate`, `link.Link`, `link.Unlink`, `link.List`, `importer.Import`. CLI surface: `planar local {list, link, unlink, import, migrate}`.

## Test spec

The fourth planning document, alongside product-spec, tech-spec, and roadmap. The planner emits a `test-spec.md` for every new feature; the ingestor decomposes its `## Scenarios` section into `test_scenarios` rows with `verifies` edges to the tasks each scenario covers.

**Authoring shape.** Scenarios are grouped into four return-path buckets, each asking a different question:

- **Happy path** — valid input, meaningful output. The function does the thing.
- **Empty / null return** — valid input, legitimately empty output (no rows, nil pointer, "not found"). A correctness path, not an error path. Easy to skip; often hides the subtlest bugs (conflating "no results" with "error").
- **Error return** — operation cannot proceed. The function returns a non-nil error.
- **Edge case** — boundary conditions (zero / one / max inputs, off-by-one, concurrent access).

Each `### Scenario: …` H3 carries leading `**Verifies:** task:N, task:M` / `**Kind:** unit|integration` / `**Acceptance:** <observable result>` lines. The ingestor extracts these into structured fields on the `test_scenarios` row.

**Coverage-gap checklist.** Below the scenarios, the operator confirms per-function compliance with each bucket via Markdown checkboxes. Gaps marked N/A require a one-line justification so the reviewer can confirm the absence is deliberate.

**Cross-references.** The test-spec carries `verifies: [artifact:<product-spec-id>]` in its frontmatter so the cross-reference machinery tracks which user stories the test plan covers. Scenarios cite tasks via `**Verifies:** task:<id>` *or* `**Verifies:** task:<slug>`. The slug form (plan 286) is the canonical citation chain: scenarios drafted before tasks exist still resolve at apply time, because the ingestor looks up `tasks.slug` against the `[slug: …]` annotations on the roadmap bullets. Unresolvable slugs are a hard error at apply — the operator either adds the missing `[slug:]` to the roadmap or removes the citation.

**Coverage gate.** `planar spec ingest` prints a `coverage:` summary after the additions/updates/removals totals: how many tasks carry a `[slug:]`, how many slug-bearing tasks have a scenario verifying them, and any orphan scenarios (no parseable `**Verifies:**` line). Pass `--strict` to promote uncovered tasks and orphan scenarios from a printed warning into a non-zero exit; this is the gate test-coder cycles depend on. The read-only inspector `planar test-spec status <plan>` prints the same view per-milestone with a four-bucket breakdown (happy / empty / error / edge), classified by scenario-title prefix.

**Planning loop integration.** The planner authors the test-spec in Phase 4 of its authoring pipeline (see [`agents/planner.md` §Authoring phases](../agents/planner.md#authoring-phases)). Phase 4 is purely adversarial: what could go wrong, what scenarios prove this works, what scenarios prove it doesn't. The planner explicitly does NOT propose implementations of the tests — that's the [test-coder](#test-coder)'s job (see below).

**Orchestrator integration.** When dispatched tasks have `[slug:]` annotations on their roadmap bullets and the test-spec cites those slugs via `task:<slug>`, the orchestrator's Phase 3.5 dispatches the test-coder agent. The gating oracle is `planar test-spec status <plan> --json` — the orchestrator does not re-implement coverage calculation. The reviewer then runs `planar test-spec status` against the post-diff DB; any slug claimed by the brief that still appears in the uncovered set is a `request-changes` finding citing the verb output verbatim.

**SQLite tables:** none beyond the existing `test_scenarios` and `entity_links`. **Primary entry points:** `ingestor.ParseTestSpec` (parses the body), `workbench.LoadCrossRefs` (reads the cross-reference edges), the `test_spec` artifact kind in `artifacts.kind`.

## Test-coder

The fourth agent role, dispatched between the coder and the reviewer in [Phase 3.5](../agents/methodology.md#phase-35--test-coder-dispatch). Reads the test-spec and the coder's diff; produces a test-only diff that closes uncovered slugs. Never modifies a failing test to make it pass — surfaces failures with a classification (`test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`).

**Cognitive-split rationale.** A coder writing tests for their own feature has the wrong incentive: make-the-green-test-pass shapes both the feature and the test. A test-coder reading the test-spec and the coder's already-committed feature has no incentive to make tests easy to pass — only to verify the cited scenarios. The two roles enforce different mental models. The load-bearing clause is **tests-may-be-elevating-bugs**: when a new test fails on first run, the test-coder does NOT modify the test, it classifies the failure and reports it. A red test is signal, not noise.

**Position in the loop.** Phase 3.5 fires *only* when (a) the cycle's dispatched tasks carry `[slug: …]` annotations AND (b) `planar test-spec status --json` reports `uncovered_task_slugs` intersecting the cycle's slugs. Slug-less tasks are out of scope by construction. The decision taxonomy is:

- `expanded` — test diff covers cited scenarios; all new tests pass. Orchestrator stages the diff alongside the coder's; reviewer sees the union.
- `no-expansion-needed` — cited scenarios already verified by the coder's diff or pre-existing tests. Reviewer sees the coder's diff alone.
- `failure-surfaced` — at least one new test fails on first run. Orchestrator escalates to the operator with the test-coder's classification; reviewer NOT dispatched until the operator resolves.
- `abort` — cannot satisfy the brief.

**Iteration cap.** The test-coder cycle has its own cap (default 2; the work shape is "expand or don't" rather than "iterate to convergence"). Independent of the coder/reviewer's 5-iteration cap.

**Manual invocation.** Operators can invoke `/pl-test-coder <task-id>` directly to backfill coverage on an already-committed change set, or `/pl-test-coder <plan-id> --plan` to run against every cited scenario in a plan. Useful after authoring a new test-spec for an older feature.

**SQLite tables:** none. **Primary entry points:** [`agents/test-coder.md`](../agents/test-coder.md) (canonical contract), `commands/claude/pl-test-coder.md` + `skills/codex/pl-test-coder.md` + `skills/copilot/pl-test-coder.md` (vendor surfaces), `planar test-spec status` (gating verb), `planar spec ingest --strict` (ingest-time gate).

## Cross-references

Every artifact, task, scenario, decision, and question can carry outgoing edges of three relationship kinds: **`verifies`** (this entity verifies another — used by `test_scenarios` to point at tasks), **`cites`** (this entity references another for context but does not depend on it), and **`derives-from`** (this entity derives from another — a tech-spec derives from a product-spec). All three are stored as rows in the `entity_links` table.

**Frontmatter surface.** The workbench `FrontMatter` struct exposes three optional list fields:

```yaml
---
entity_kind: artifact
entity_id: 42
artifact_kind: test_spec
verifies:
  - artifact:131   # the product-spec
cites:
  - plan:277       # a related plan
derives-from:
  - artifact:132   # the tech-spec
---
```

Each entry is a `"<kind>:<id>"` reference parsed into an `EntityRef` struct. Custom YAML marshalling (`UnmarshalYAML` / `MarshalYAML`) round-trips the string form. Malformed entries (empty kind, non-integer id, etc.) error at parse time so drift surfaces immediately.

**Push and pull.** On `workbench push`, the renderer reads the entity's `entity_links` rows and populates the three frontmatter lists. On `workbench pull`, the parser reads the frontmatter lists and reconciles them against `entity_links` (additive in v1; orphan-edge removal is a follow-up). Same pattern as the `touches:` field used by cross-repo tasks.

**Authoring discipline.** The planner emits frontmatter cross-references when relationships are obvious (test-spec verifies product-spec; tech-spec derives from product-spec). Operators can hand-edit the lists at any time via `<entity> edit <id>`. The new entries become `entity_links` rows on the next pull.

**Why frontmatter, not a body section.** A first design appended a `## Cross-references` Markdown section to artifact bodies. That broke the ingestor — the roadmap parser iterates every `## …` H2 as a milestone, and the injected section shifted milestone counts. Frontmatter avoids the collision. The `workbench.RenderCrossRefsSection` helper exists for callers (e.g. `planar tree` or `planar <entity> view`) that want a human-readable rendering, but the body itself stays clean.

**SQLite tables:** `entity_links` (existing; widened CHECK accepts `verifies` / `cites` / `derives-from` since plan 4). **Primary entry points:** `workbench.LoadCrossRefs(db, kind, id)`, `workbench.RenderCrossRefsSection`, `FrontMatter.Verifies` / `Cites` / `DerivesFrom`.
