# Planar Workflows

End-to-end recipes for common Planar operations. These assume you have run `planar init` and understand the core concepts (see `docs/concepts.md`).

---

## Recipe 1 — Start a New Feature

Use this when you have a goal and want to produce a structured plan with tasks ready for execution.

**What happens:** The planner agent drafts **four** planning documents into the workbench in four sequential authoring phases (product → tech → roadmap → test). The user reviews and edits them. The ingestor decomposes them into plans, tasks, decisions, and scenarios in the database.

### Step 1 — Draft planning documents

```
/pl-spec-draft "add billing export to CSV"
```

The planner creates:
- A draft anchor plan with a generated slug (e.g. `billing-export-csv`).
- A workbench directory at `~/.planar/workbench/<assoc>/p<id>-billing-export-csv/`.
- Four artifact files: `product-spec.md`, `tech-spec.md`, `roadmap.md`, `test-spec.md`.

Expected output:
```
plan 42 created  [draft]  billing-export-csv
workbench: ~/.planar/workbench/project:my-app/p42-billing-export-csv/
artifacts: product-spec.md, tech-spec.md, roadmap.md, test-spec.md
```

### Step 2 — Review and edit

Open the workbench directory. Read all four documents. Edit the `roadmap.md` section headers and bullet points — these become plan and task rows. **Annotate every testable bullet with a `[slug: foo-bar]` tag** so the test-spec can cite it by stable name even before ids are assigned. Edit the `## Decisions` section in `tech-spec.md` — these become decision rows. Edit acceptance criteria in `product-spec.md` — these become scenario rows. Edit the `## Scenarios` section in `test-spec.md` — each `### Scenario:` H3 becomes a `test_scenarios` row with `verifies` edges to the cited tasks. Cite tasks via `**Verifies:** task:<slug>` referencing the `[slug: …]` annotations on the roadmap bullets. The test-spec's four return-path buckets (happy / empty-null / error / edge) are the structured surface for thinking about coverage; the coverage-gap checklist at the bottom is the reviewer's compliance pass.

When satisfied, signal readiness to ingest.

### Step 3 — Preview ingestion

```
/pl-spec-ingest 42
```

The ingestor reads the workbench documents and prints a tree-shaped diff of proposed additions:

```
project:my-app/billing-export-csv/
  + plan       Phase 1 — Schema                  (3 tasks)
  +   task     Add export_jobs table
  +   task     Add billing_exports table
  +   task     Write migration 0008
  + plan       Phase 2 — Implementation           (2 tasks)
  +   task     Implement CSV serialiser
  +   task     Wire up export endpoint
  + decision   Use ISO 8601 for timestamps

5 additions. Run with --apply to commit.
coverage: 5 tasks (5 with slug, 0 without); 0 uncovered
```

The `coverage:` line follows the totals. It reports how many of the tasks have a `[slug:]` annotation and how many of those slugs are cited by at least one scenario in `test-spec.md`. Uncovered slugs and orphan scenarios (missing `**Verifies:**` line) are listed inline. To turn the gate into a hard refusal, run with `--strict`:

```
/pl-spec-ingest 42 --strict
```

`--strict` exits non-zero on any uncovered task or orphan scenario, naming each gap so the operator can fix them in one pass — usually by adding the missing `[slug:]` to a roadmap bullet or filling in a scenario's `**Verifies:**` line.

For a per-milestone view (with a four-bucket breakdown by scenario-title prefix) use the read-only inspector:

```
planar test-spec status 42
```

No changes are written by either preview or `test-spec status`.

### Step 4 — Apply

Review the diff. If it looks correct:

```
/pl-spec-ingest 42 --apply
```

Add `--strict` to the apply call when the test-spec is meant to be complete — the gate will reject the apply if any slug-bearing task is still uncovered.

Tasks, child plans, and decisions are now in the database. The anchor plan status is unchanged (still `draft`). Activate it before execution:

```
planar plan active 42
```

---

## Recipe 2 — Run the Orchestrator

Use this when an anchor plan is `active` and you want to execute tasks.

**What happens:** The orchestrator analyzes task coupling, proposes a dispatch shape (strict/grouped/single), waits for confirmation, then dispatches tasks to the coder. Each coder output goes through the reviewer. The loop cap is 5 iterations per dispatch cycle.

### Invoke

```
/orchestrator 42
```

The orchestrator reads the anchor plan's task graph and proposes a dispatch shape:

```
Plan 42 "billing-export-csv" — 5 todo tasks

Proposed dispatch shape: grouped
  Group A: tasks 43, 44, 45  (all touch migrations/ — one coder cycle)
  Group B: tasks 46, 47      (independent of Group A)

Accept this shape? [yes / edit / strict / single]
```

Confirm (`yes`) or adjust. The orchestrator then dispatches Group A to `/coder`, routes the output to `/reviewer`, and iterates based on reviewer feedback. On `approve`, tasks flip to `done` and Group B is dispatched.

### Flags

| Flag | Effect |
|------|--------|
| `--strict` | One coder cycle per task; skip the dispatch-shape gate |
| `--grouped` | Orchestrator picks groupings; skip the gate |
| `--batch 43,44,45 --batch 46,47` | Explicit groupings; skip the gate |
| `--propagate` | After all tasks are done, run `ext propagate` automatically |
| `--archive` | After propagation, archive the workbench tree |

### Iteration cap

Each dispatch cycle allows at most 5 coder iterations. On iteration 5, the reviewer must choose `approve` (with caveats recorded as follow-up tasks) or `abort` (escalate to user). `request-changes` is not a valid outcome on iteration 5.

### Phase 3.5 — Test-coder dispatch

After the coder reports done, the orchestrator consults `planar test-spec status <plan> --json` against the post-coder DB. When `uncovered_task_slugs` intersects the cycle's dispatched slugs, the orchestrator dispatches the test-coder before the reviewer:

```
Phase 3.5: test-spec coverage check for plan 42

Cycle dispatched slugs: [add-migration, wire-rpc, gateway-config]
planar test-spec status --json:
  uncovered_task_slugs: [add-migration, gateway-config]

Intersection non-empty → dispatching /pl-test-coder for slugs:
  - add-migration
  - gateway-config
```

The test-coder reads the test-spec sections cited via those slugs and the coder's diff (`git diff <coder-cycle-base>..HEAD`), then produces a test-only diff. Four outcomes:

| Outcome | Action |
|---------|--------|
| `expanded` | Test diff staged alongside coder's; reviewer sees the union. |
| `no-expansion-needed` | Cited scenarios already verified; reviewer sees the coder's diff alone. |
| `failure-surfaced` | A new test fails on first run. Orchestrator escalates with the classification (`test-wrong-author-error` / `code-wrong-bug-surfaced` / `ambiguous-operator-decide`). Reviewer NOT dispatched. |
| `abort` | Brief unsatisfiable. Escalates. |

The reviewer then runs `planar test-spec status <plan>` against the post-diff DB and treats any leftover uncovered slug claimed by the brief as a `request-changes` finding citing the verb output verbatim. The verb is the authoritative oracle; eyeball-comparing diffs to scenario prose is not the reviewer's job here.

#### Worked example: failure-surfaced

The coder ships `add-migration` with happy-path tests. The test-spec also cites a `task:add-migration` scenario for the "rollback on FK violation" path. The test-coder writes that test; it fails because the rollback path was never implemented. The test-coder classifies it as `code-wrong-bug-surfaced` and reports:

```
test-coder report:
  decision: failure-surfaced
  failures:
    - test: TestMigration_RollbackOnFKViolation
      scenario: task:add-migration (Error return — FK violation)
      recommendation: code-wrong-bug-surfaced
      detail: assertion expected rollback at line N; observed no rollback
```

The orchestrator escalates. The operator decides whether to dispatch a new coder cycle to implement the rollback, refine the test-spec (if the scenario is over-specified), or accept the gap as a deferred follow-up task. The reviewer is not dispatched until the operator resolves.

#### Manual invocation

To backfill coverage on an already-committed change set:

```
/pl-test-coder <task-id>          # one task's cited scenarios
/pl-test-coder <plan-id> --plan   # every cited scenario in the plan
```

Useful after authoring a new test-spec for an older feature, or for a second-pass coverage check on a PR.

### Picking a barrel mode

The orchestrator's gate offers six shapes — three classic (`strict`, `grouped`, `single`) and three barrel modes (`barrel-grouped`, `barrel-deferred`, `barrel-bypass`). Each barrel mode trades safety for throughput at a different point. The worked examples below show the gate proposal, the audit-trail excerpt, and when to pick each.

#### barrel-grouped (alias for grouped, explicit barrel namespace)

```
/orchestrator 42 --barrel-grouped
```

Equivalent to `--grouped` with the milestone heuristic locked in. Each milestone becomes one coder cycle, then one reviewer dispatch per group per the existing dispatch profile.

Audit-trail excerpt (one entry per cycle):
```
dispatch_shape: barrel-grouped
reviewer_disposition: dispatched
cycle_scope: plan:42 milestone:43
tasks: [44, 45, 46]
```

**Pick when:** you want milestone-granular history with full reviewer coverage. The most conservative barrel mode.

#### barrel-deferred (per-milestone boundary)

```
/orchestrator 42 --barrel-deferred                       # default: per-milestone
/orchestrator 42 --barrel-deferred --barrel-deferred-at plan   # once at end of plan
```

Coder cycles run back-to-back without reviewer dispatch between them. At the milestone (or plan) boundary, one reviewer fires against the union of all queued diffs.

Audit-trail excerpt:
```
# After cycle 1 (M1 coder done, no reviewer yet):
dispatch_shape: barrel-deferred
reviewer_disposition: deferred
cycle_scope: plan:42 milestone:43
tasks: [44, 45, 46]

# After cycle 2 (M2 coder done, no reviewer yet):
dispatch_shape: barrel-deferred
reviewer_disposition: deferred
cycle_scope: plan:42 milestone:47
tasks: [48, 49]

# At milestone boundary close, reviewer fires on the union diff for M1 (or M1∪M2 if --barrel-deferred-at plan):
dispatch_shape: barrel-deferred
reviewer_disposition: dispatched
cycle_scope: plan:42 boundary:milestone:43
tasks: [44, 45, 46]
```

**Pick when:** you want throughput between cycles but a late safety net. Useful for long plans where per-cycle review would be expensive but you still want a human/agent review at sensible boundaries.

**Trade-off:** an `abort` on the boundary reviewer dispatch halts every cycle in the queue. The operator's incentive to not pick over-aggressive boundaries.

#### barrel-bypass (maximum throughput, gates are the entire signal)

```
/orchestrator 42 --barrel-bypass
```

No reviewer dispatch at all. Coder cycles run back-to-back; quality gates (`make fmt-check` + `make build` + `make test` + `make test-integration` **twice** + `planar skills render --check` against an out-of-tree staging dir + any remaining relevant validators) are the entire signal.

Audit-trail excerpt:
```
dispatch_shape: barrel-bypass
reviewer_disposition: bypassed
cycle_scope: plan:42 milestone:43
tasks: [44, 45, 46]
```

**Pick when:** you trust the gates completely (large existing test surface, strict typing, render-check/validator coverage in place) and want to ship the plan as fast as possible. Common for docs-only plans, vendor-surface mirror plans, or methodology updates where the diff IS the verification.

**Trade-off:** uncaught defects must surface via runtime testing or out-of-band review. The closest retroactive surface is `git blame` + `/pl-reviewer <task-id>` against a still-active task.

**Phase 3.5 still fires across all three barrel modes** when uncovered slugs intersect the cycle. Barrel-bypass bypasses the reviewer, not the coverage gate. The test-coder's `failure-surfaced` outcome halts the cycle and escalates to the operator regardless of mode.

For the canonical contract see [`agents/methodology.md` §Barrel modes](../agents/methodology.md#barrel-modes); for the dispatch-shape concept see [`docs/concepts.md` §Dispatch shapes](concepts.md#dispatch-shapes).

---

## Recipe 3 — Propagate to GitHub Issues

Use this after tasks are done (or any time you want external counterparts created).

**What happens:** The ext-sync agent walks the feature tree top-down. It creates external counterparts for every entity not yet linked and records `external_links(link_role='mirror')` rows and `sync_events(outcome='ok')` rows.

### Register the external system (once per system)

```
planar ext register github my-gh --project myorg/myrepo
```

### Propagate

```
/pl-ext-propagate 42 --system my-gh
```

On first propagation, the strategy is selected automatically:

- 0 touched repos → `github-zero-repo` (parent issue in `github_lead_repo` from config)
- 1 touched repo → `github-parent-issue`
- 2+ touched repos → `github-projects-v2`

The selected strategy is cached and reused on subsequent propagation runs. To force re-detection:

```
/pl-ext-propagate 42 --system my-gh --restrategize
```

Expected output:
```
propagate plan:42 → my-gh (github-parent-issue): 7 created, 0 skipped, 0 failed
  created   plan:42 "billing-export-csv" → myorg/myrepo#101
  created   plan:43 "Phase 1 — Schema"  → myorg/myrepo#102
  created   task:44 "Add export_jobs table" → myorg/myrepo#103
  ...
```

### Dry run

```
/pl-ext-propagate 42 --system my-gh --dry-run
```

Prints what would be created without contacting the remote.

### Verify existing counterparts

```
planar ext propagate 42 --system my-gh --verify-counterparts
```

Probes the remote to confirm every already-linked entity still exists. Missing counterparts are written as `sync_events(outcome='counterpart-missing')` rows. Use `--unlink` to remove stale links or `--recreate` to recreate them immediately.

---

## Recipe 4 — Handoff Between Agents or Sessions

Use this when an agent session is ending and a new session needs to resume the work. Also useful when switching vendors (e.g., Claude → Codex).

### Capture the handoff

At the end of the current session:

```
/pl-handoff
```

The handoff skill:
1. Validates that the current task has a `next_action` set.
2. Checks for unresolved blocking questions.
3. Writes a `context_snapshots` row with the full task state, recent session entries, open questions, and decisions made.
4. Writes a `handoffs` row marking the session as handed-off.

Expected output:
```
handoff record written  session:201 → ready for resume
next_action: "implement the CSV writer; see tech-spec.md § Serialisation"
snapshot: context_snapshots:47
```

### Resume in the new session

```
/pl-resume 37
```

The resume skill reads:
- Current task state and body.
- The most recent `context_snapshots` row for this task.
- Open questions (unresolved questions blocking progress).
- Decisions made in prior sessions.
- The `next_action` field.

It assembles these into a structured prompt that the new agent session reads at startup, providing full context without relying on conversational history.

`planar resume validate 37` checks resume readiness without producing the packet — useful for CI or pre-handoff verification.

---

## Recipe 5 — Snapshot a Feature for External Consumption

Use this when you want to commit a workbench snapshot into a git repo's `docs/` or deliver it to a stakeholder.

**What happens:** `workbench publish` copies the workbench tree into an arbitrary path on disk. The copy is a flat directory snapshot — no sync tracking, no DB references. Useful for archiving a feature spec into the host repo or generating a deliverable.

### Publish the workbench tree

```
planar workbench publish 42 --to ~/myrepo/docs/planning/billing-export
```

Expected output:
```
published 12 files → ~/myrepo/docs/planning/billing-export/
```

### Options

| Flag | Effect |
|------|--------|
| `--include <glob>` | Include only files matching the glob (repeatable) |
| `--exclude <glob>` | Exclude files matching the glob (repeatable) |
| `--dry-run` | Print what would be copied without writing |
| `--delete-removed` | Delete destination files that no longer exist in the workbench |

### Commit the snapshot

```bash
cd ~/myrepo
git add docs/planning/billing-export/
git commit -m "snapshot billing-export spec for review"
```

The snapshot is a static copy. It does not stay in sync with the workbench automatically — re-run `workbench publish` to refresh it.

---

## Recipe 6 — Archive a Completed Feature

Use this after a feature ships and the on-disk workbench tree is no longer actively needed.

**What happens:** `workbench archive` removes the filesystem tree. The database retains every entity row, relationship, artifact, decision, and scenario. `workbench restore` recreates the tree byte-identically from the DB at any time.

### Mark the plan done

```
planar plan done 42
```

Verify:
```
planar plan show 42
# status: done
```

### Archive the workbench tree

```
planar workbench archive 42
```

Expected output:
```
archived plan:42  removed ~/.planar/workbench/project:my-app/p42-billing-export-csv/
database: all entities retained
```

Via skill:
```
/pl-workbench-archive archive 42
```

### Restore when needed

```
planar workbench restore 42
```

Expected output:
```
restored plan:42 → ~/.planar/workbench/project:my-app/p42-billing-export-csv/
12 files written
```

The restored tree is byte-identical to the pre-archive state. All workbench sync state (`workbench_sync_state`) is restored, so `workbench status` and subsequent push/pull/sync operations work correctly.

---

## Recipe 7 — Adopt an Existing Repo into Planar

Use this when you have months or years of accumulated work (specs, ADRs, roadmap files, TODO lists, backlog Markdown, git history) and want to import it into Planar without starting from scratch.

**What happens:** The importer walks the repo filesystem, reads planning artefacts, infers task completion status from checkbox state and git history, and produces an ImportPlan. The user reviews the preview, then applies it. Idempotency ensures re-runs after new commits are safe.

### Step 1 — Install Planar and initialize

```
planar init
```

Run once per machine. Creates `~/.planar/planar.db` and applies all migrations.

### Step 2 — Set scope (optional)

By default `import` resolves scope from cwd: run it from inside the repo you want to adopt and the resolved scope is `project:<that-repo>`. To import under a different association, pass `--scope` on the `/pl-import` invocation:

```
/pl-import . --scope assoc:project:my-app
```

To import under global scope, run from outside any registered scope and pass `--scope global` explicitly.

### Step 3 — Preview the import

```
/pl-import .
```

The importer discovers artefacts from the current directory, infers task statuses, and prints a tree-shaped diff without writing anything:

```
./
  + plan       M1 — Schema                  (3 tasks)
  +   task     Add foundation migration      [done — git-log match]
  +   task     Add planning tables           [done — git-log match]
  +   task     Add work-item tables          [todo]
  + artifact   docs/tech-spec.md            [kind=tech_spec]
  + decision   Use pure-Go SQLite           [from: docs/adr/0005-go-as-runtime.md]

5 additions, 0 skipped.
Run with --apply to commit.
```

Review the tree. Check that:
- Milestone headings mapped to the right child plans.
- Task statuses (`done` / `todo`) look correct.
- Artifacts and decisions were discovered.

### Step 4 — Optionally include GitHub issues

If the repo has open GitHub issues you want to import as tasks:

```
/pl-import . --from-github
```

Requires the `gh` CLI on PATH and a GitHub remote at `origin`. Previews without writing.

### Step 5 — Apply

```
/pl-import . --apply
```

Commits the import. Expected output:

```
applied: 4 plans, 18 tasks, 3 artifacts, 5 decisions, 2 skipped
anchor plan id: 87
```

To include GitHub issues in the same apply run:

```
/pl-import . --from-github --apply
```

### Step 6 — Review the imported tree

```
planar tree
planar plan list
planar task list
```

The anchor plan (id 87 in the example above) is the entry point. Child plans correspond to milestones. Tasks carry inferred statuses.

### Step 7 — Propagate to an external system (optional)

If you want the imported tree mirrored to GitHub Issues or Jira:

```
/pl-ext-propagate 87 --system my-gh
```

See Recipe 3 for the full propagation walkthrough.

### Step 8 — Re-run incrementally

After new commits land, re-run the preview to see what new work can be imported:

```
/pl-import .
```

Items already in the database are reported as **Skipped**. Only new items appear as additions. Apply when satisfied:

```
/pl-import . --apply
```

---

## Recipe 7a — Greenfield Onboard with synthesize

Use this when the repo is docs-only or has no implementation yet — a freshly cut roadmap, a planning sandbox, or a project where the code tree is intentionally empty. `import` would treat every roadmap-adding commit as evidence that the work is done; `synthesize` reads the source tree and refuses to mark anything done when there is no source to back the claim.

**What happens:** the synthesizer's deterministic floor probes the source tree, sees zero meaningful evidence, and sets `greenfield=true` on the `synthesis.Request`. The vendor skill's contract forbids `status != "todo"` under greenfield; the Go-side `synthesis.Validate` enforces it. Every task lands as todo regardless of what the docs claim.

### Step 1 — Run synthesize against the greenfield repo

```
/pl-synthesize ~/projects/holdfast
```

Auto-detection sets greenfield mode because codeprobe reports no source-file evidence (no `Sources/`, no `src/`, no `internal/`, no `cmd/` — just `docs/` and `README.md`). The preview annotates the mode:

```
greenfield mode: yes (auto-detected — no source-file evidence)
synthesis-request written to ~/.planar/cache/bootstrap-synthesis/holdfast/_pending.json
Awaiting LLM synthesis. Re-run `planar synthesize ~/projects/holdfast` after the vendor skill produces the Result.
```

### Step 2 — The vendor skill produces a Result

The Claude / Codex / Copilot skill picks up the pending request, runs the LLM at temperature 0, and writes a Result whose every task has `status=todo`. `synthesis.Validate` rejects any Result that violates the greenfield invariant.

### Step 3 — Re-run synthesize to merge and preview

```
/pl-synthesize ~/projects/holdfast
```

Preview shows every roadmap milestone as a child plan with `status=draft` and every extracted task at `status=todo`. Compare to what `import` would have produced: the roadmap-adding commit ("docs: phase 1 roadmap") matches git-log to dozens of phase-1 task titles via fuzzy correlation, so without the >25% auto-done refusal `import` would have marked them done. The greenfield mode of `synthesize` makes the false-done problem structurally impossible.

### Step 4 — Apply

```
/pl-synthesize ~/projects/holdfast --apply
```

Reference artifacts (the original `docs/product-roadmap.md` etc.) are preserved on the anchor plan as `kind=research`; the synthesized `product_spec` / `tech_spec` / `roadmap` are the primary planning material.

**When to escape greenfield auto-detection.** If the repo has a non-conventional code layout that codeprobe under-detects (e.g. monorepo with code only under `vendor/`), pass `--treat-as-nongreenfield` to bypass auto-detection.

See [Recipe 7](#recipe-7--adopt-an-existing-repo-into-planar) for the sibling transcription path.

---

## Recipe 7b — Docs-with-Code Onboard with synthesize

Use this when the repo has both clean planning material and a working source tree, and you want the LLM to assess what's actually there rather than transcribe the docs verbatim.

**What happens:** the synthesizer probes the source tree, builds a per-FeatureArea EvidenceMap (source / test / CI / commit signals with `SignalStrength` scores), and hands both the docs and the EvidenceMap to the LLM. Tasks with `status != "todo"` must cite a `code_evidence` path that exists in the EvidenceMap; the validator rejects anything claiming completion without code to back it.

### Step 1 — Run synthesize against the repo

```
/pl-synthesize ~/projects/lectio
```

Auto-detection: code is present (`Sources/Lectio/`, `Tests/LectioTests/`), so greenfield mode is OFF. Layout auto-detection picks `swift` from `Package.swift`.

```
greenfield mode: no
code-layout: swift (from Package.swift)
feature areas: 7 (Reader, Annotations, Sync, ...)
synthesis-request written to ~/.planar/cache/bootstrap-synthesis/lectio/_pending.json
```

### Step 2 — Re-run after the vendor skill produces a Result

```
/pl-synthesize ~/projects/lectio
```

Preview shows tasks with code-evidence citations annotated:

```
+ task     Wire annotations storage          [done — cited: Sources/Lectio/Annotations/Store.swift]
+ task     Add sync conflict resolution      [todo]
+ task     Implement Reader pagination       [doing — cited: Sources/Lectio/Reader/Pager.swift]
```

The "code wins" rule: if the docs claim a task is done but no source file backs the claim, the LLM is contractually forbidden from marking it done — `synthesis.Validate` rejects the Result.

### Step 3 — Compare with import

For the same fixture, `planar import ~/projects/lectio` would produce a similar plan tree — but with statuses driven by git-log correlation (commit titles vs. task titles) rather than code-presence. Both verbs land in the same downstream pipeline; `import` preserves the original doc structure exactly, while `synthesize` produces an LLM-curated reorganization grounded in what the code actually shows.

### Step 4 — Apply

```
/pl-synthesize ~/projects/lectio --apply
```

See [Transcription vs Synthesis](./concepts.md#transcription-vs-synthesis) for the conceptual split.

---

## Recipe 7c — Mid-Evolution Onboard with synthesize

The most interesting case: the docs claim more than the code shows. A roadmap declares Phase 2 complete; the source tree shows Phase 2's directory is empty. `import` would faithfully transcribe the docs' claim (or git-log-correlate it to done); `synthesize` reads the source and refuses to mark a phase done without code evidence.

### Step 1 — Run synthesize against the mid-evolution repo

```
/pl-synthesize ~/projects/midevo
```

The deterministic floor probes the tree:

```
greenfield mode: no
code-layout: go (from go.mod)
feature areas: 4 (sessions, transport, auth, telemetry)
  sessions: source=none, tests=none, ci=none, commits=2 (docs only)
  transport: source=strong, tests=present, ci=present, commits=14
  auth: source=present, tests=none, ci=none, commits=5
  telemetry: source=none, tests=none, ci=none, commits=1 (docs only)
synthesis-request written to ~/.planar/cache/bootstrap-synthesis/midevo/_pending.json
```

### Step 2 — Re-run after the vendor skill produces a Result

```
/pl-synthesize ~/projects/midevo
```

Phase 2 (sessions) lands as `status=active` (or todo) despite the roadmap claiming `done`, because the EvidenceMap shows `source=none, tests=none`. Phase 3 (transport) lands as `done` with a code-evidence citation. The original Phase 2 done-claim survives as a `kind=research` reference artifact on the anchor plan, so the operator can see both narratives:

- Synthesized roadmap (primary): Phase 2 active, Phase 3 done.
- Reference artifact (the original `docs/product-roadmap.md`): Phase 2 marked done.

### Step 3 — When to use `--treat-as-nongreenfield`

For repos where codeprobe under-detects code (non-conventional layouts, code under `vendor/`, generated code only, etc.), pass `--treat-as-nongreenfield` to force the synthesizer out of greenfield mode. This is rare; reach for it only after the preview shows `greenfield mode: yes` despite obvious source files being present.

### Step 4 — Apply

```
/pl-synthesize ~/projects/midevo --apply
```

The audit trail preserves the original doc-claims; operators reviewing the imported tree see the synthesized version as primary and the original docs as research-grade reference.

See [Recipe 7](#recipe-7--adopt-an-existing-repo-into-planar) for the sibling transcription path and [Recipe 7a](#recipe-7a--greenfield-onboard-with-synthesize) / [Recipe 7b](#recipe-7b--docs-with-code-onboard-with-synthesize) for the other synthesis shapes.

---

## Recipe 8 — Reviewing Open Questions

Use this when a spec has a `## Open questions` section and you want to track those items as first-class question entities, resolve them, and confirm the spec stays in sync.

**What happens:** `/pl-spec-draft` auto-registers each H3 item under `## Open questions` as a `questions` row when it seeds the workbench. `/pl-spec-ingest` reconciles on re-run — it compares the live question entities against the current spec body and surfaces drift warnings for items that were added or removed from the spec without a corresponding entity update.

### Step 1 — Author open questions in a spec

In any planning artifact (`product-spec.md`, `tech-spec.md`), add an `## Open questions` section. Each question is an H3 child heading:

```markdown
## Open questions

### Which date format for export timestamps?

ISO 8601 UTC is the default preference; confirm with the consumer team.

### Should partial rows be silently skipped or cause an error?

Current thinking: error loudly — silent skips are hard to debug.
```

The H3 heading text becomes the `title` of the question entity. The body paragraph(s) beneath it become the question's `body`. Each H3 is one entity.

### Step 2 — Register questions automatically via /pl-spec-draft

When you run the planner skill on a goal statement, it drafts the spec files and calls `planar workbench extract-questions` internally. Any `## Open questions` H3 items found are registered as question entities via `planar question add`:

```
/pl-spec-draft "add billing export to CSV"
```

Expected output includes a line like:

```
questions: 3 registered from tech-spec.md
```

### Step 3 — List open questions

```
planar question list --status open
```

Expected output:

```
id  title                                              status  plan
5   Which date format for export timestamps?           open    42
6   Should partial rows be silently skipped or error?  open    42
7   Do we need a download-progress indicator?          open    42
```

### Step 4 — Resolve questions

Answer a question:

```
planar question answer 5 "ISO 8601 UTC, no timezone offset — confirmed with consumer team"
```

Mark a question as won't-fix (decided not to address):

```
planar question wontfix 7
```

Both commands flip the question's `status` out of `open`. An `answered` row also records `answer_body` and `answered_at`.

### Step 5 — Re-ingest after spec edits

If you edited the spec body after the initial draft — adding, removing, or rewriting H3 question items — run the ingestor again to reconcile:

```
/pl-spec-ingest 42
```

The ingestor reads the current spec body, calls `planar workbench extract-questions 42`, and compares the live H3 items against the existing question entities. Drift surfaces as warnings in the preview output:

```
project:my-app/billing-export-csv/
  ~ question  "Which date format for export timestamps?" — answered, still in spec body (ok)
  ! question  "Should partial rows be silently skipped or error?" — in spec body but no entity found
  ~ question  "Do we need a download-progress indicator?" — entity exists, removed from spec body

2 drift warning(s). Review before --apply.
```

Fix drift by either updating the spec body or updating the entity status, then re-run with `--apply`.

### Cross-references

- CLI verbs: `planar workbench extract-questions`, `planar question list`, `planar question answer`, `planar question wontfix` — see `docs/cli-reference.md`.
- Skills: `/pl-spec-draft` (auto-registers questions on draft), `/pl-spec-ingest` (reconciles on re-ingest) — see `docs/skill-reference.md`.

---

## Recipe 9 — Working with multiple agents across repos

Use this when a planner dispatches coder agents to different repos in parallel — the polyrepo orchestrator case. The strict write-scope resolver was designed for it; this recipe walks through the call shape that keeps each coder writing to the correct association without explicit `--scope` plumbing everywhere.

**What happens:** Each coder runs in its own process with `cwd` set to the repo it owns. Step 2 of `ResolveForWrite` (cwd derivation, most-specific-wins) picks the right association from cwd alone. The orchestrator itself — whose cwd may not match any single target repo — passes `--scope` explicitly on cross-repo writes. A second layer, the [cross-scope guard](concepts.md#cross-scope-guard), then verifies the operator's resolved scope agrees with each *target entity's* stored scope before the write proceeds: a coder that drifts into the wrong cwd, or an orchestrator that passes a stale `--scope`, gets refused with exit 1 instead of silently writing under the wrong association.

### Coder pattern: rely on cwd derivation

When a coder is spawned for a task that touches `repo-a`, the dispatcher `cd`s the coder process into the repo before invoking write verbs:

```bash
cd ~/work/repo-a
planar task doing 142
# … coder works …
planar task done 142
```

No `--scope` flag is needed. The resolver sees that `cwd` is inside the registered `project:repo-a` root, ranks `project` above the org association `project:repo-a` belongs to, and resolves to `project:repo-a`. The success line shows `[from cwd]`. The cross-scope guard then confirms that task 142 actually belongs to `project:repo-a`; if the dispatcher accidentally `cd`-ed into `repo-b` for a task owned by `repo-a`, the guard refuses with a multi-line error naming both scopes and pointing at `--scope` / `--no-scope-check` as remediation paths.

This is the dominant pattern. Every coder, ingestor, and planner skill that runs against a known repo cwd works without scope plumbing.

### Orchestrator pattern: pass `--scope` explicitly

The orchestrator's own cwd is wherever it was invoked from — usually a workspace root or an unrelated directory. When it writes to entities in a target repo's association (e.g. updating a task in `project:repo-b` while dispatching), it must pass `--scope` explicitly:

```bash
planar task update 142 --next-action "wire up the CSV writer" \
  --scope assoc:project-repo-b
```

The success line shows `[from flag]`. The guard then verifies the explicit scope matches task 142's stored scope. This protects against the silent-wrong-scope class of bug: if the orchestrator's ambient stack has stale entries from a previous run, the explicit flag still routes the write correctly *and* the guard catches the case where the flag itself is wrong. New decisions logged from the orchestrator follow the same pattern:

```bash
planar decision add "Adopt strict resolver for writes" \
  --scope assoc:project-repo-b \
  --kind design
```

`decision add` is a create verb, so the guard does not fire — the new decision's scope is whatever `--scope` resolves to. The guard fires on subsequent `decision update`, `decision supersede`, and `audit publish-decision` calls.

### Debugging stale stack state

### Inspecting the resolved scope

`planar scope show` prints the cwd-derived scope set:

```
$ planar scope show
resolved scope (from cwd):
  project:repo-a  (root_path: /Users/mn/work/repo-a)
```

From a workspace root, it lists the org plus every member project. From outside any registered scope, it says `none` and tells you to `cd` or pass `--scope`.

### When the cwd does not match

If the orchestrator runs from a directory that is not inside any registered project, the resolver refuses with `AmbiguousScopeError` (writes) or `OutsideRegisteredScopeError` (reads). For cross-repo planners, pass `--scope <slug>` explicitly — it makes intent legible in the command history. The active scope stack was removed in plan 153 M5, so there is no implicit fallback that could mask the cwd mismatch.

The `--no-scope-check` escape hatch exists for legacy callers that cannot be updated immediately; it converts a cross-scope-guard refusal into a one-line stderr warning and proceeds. Do not use it in new orchestrator code. See [`./cli-reference.md#cross-scope-guard`](cli-reference.md#cross-scope-guard) for the full guarded/unguarded matrix and the exact refusal message format.

---

## Recipe 10 — Doc hygiene pre-commit

Outward-facing docs under `docs/` carry citation footnotes and provenance front matter. The `planar doc` verbs validate both halves; a shipped git hook wires that validation into pre-commit so drift cannot land silently.

### What it checks

- `planar doc manifest verify` — O(1) compare against the stored `.manifest-docs` root hash. Fails when any doc on disk has drifted, been added, or been deleted relative to the manifest.
- `planar doc lint` — walks every `*.md` under `docs/`, validates that GFM footnote citations (bracketed-caret-id form) are declared in front matter, that declared references are actually used, that external URLs resolve (cached, 30-day TTL), and that planar entity references resolve in the database.

### Opt in

The hook is shipped but not installed by default. Symlink it from `.git/hooks/`:

```
ln -s ../../scripts/git-hooks/pre-commit-docs .git/hooks/pre-commit
```

Verify it runs:

```
git commit -m 'noop'
# → runs planar doc manifest verify then planar doc lint
```

### Fixing failures

- `manifest is out of date` — review the drift with `planar doc manifest diff`. The output classifies each path as `regenerate-candidate`, `hand-edit`, `new-authoring`, or `deletion`. Once the docs themselves are settled, refresh the manifest:

```
planar doc manifest update
```

- `doc lint: N issue(s)` — each line is `<path>: <type>: <detail>`. Issue types:

  - `undeclared_citation` — a bracketed-caret-id footnote in body with no matching entry in front matter `references:`. Add the declaration.
  - `unused_declaration` — front matter declares an id never cited. Remove it or cite it.
  - `unresolvable_external` — external URL HEAD returned non-2xx. Fix the URL.
  - `unresolvable_planar` — planar entity (e.g. `decision:7`) does not resolve. Fix the ref.
  - `malformed_entry` — declared entry missing required fields (e.g. external missing `url`).

### Bypass

Use `git commit --no-verify` once you have read the failures and decided to defer the fix. Note in the commit message why.

---

## Recipe 11 — Enriching a workspace routing table with LLM summaries

`planar workspace routing build` is deterministic by default: it scans
manifest files, READMEs, and dependency declarations, and writes a
`routing-table.json` containing static signals. Plan 135 M4 adds an
opt-in enrichment pass that merges LLM-derived summaries, capability
tags, and dependency hints into the same table — without making the Go
binary depend on any LLM SDK.

### The contract

The Go builder never calls a model. It looks up structured results in
a content-addressed cache:

```
~/.planar/cache/workspace-enrichment/<org_id>/<slug>-<sha256>.json
```

The `<sha256>` is computed from the project's README excerpt (first
64 KiB) plus a sorted depth-2 directory listing. If the project's
content has not changed since the last run, the fingerprint is
identical and the cache hits — no tokens are spent.

### Populating the cache (recommended): `pl-workspace-scan --enrich`

The canonical producer is the `pl-workspace-scan` vendor skill (Claude /
Codex / Copilot). The skill runs inside an LLM session, fingerprints
every project, calls the model with temperature 0, and writes the
results into the cache before invoking the builder:

```
/pl-workspace-scan --enrich
planar workspace routing build --enrich
```

The skill is responsible for temperature-0 + seed discipline. The Go
side validates only that each result carries a non-empty `provenance`
string and that the result's `fingerprint_hash` matches the cache
filename; a result whose `provenance` contains a non-zero temperature
hint (e.g. `temperature=0.7`) produces a warning but is still applied.

### Populating the cache (power user): `enrich_command`

Operators who do not want to route through a vendor skill can wire a
shell bridge in the workspace config:

```toml
# ~/.planar/workspaces/<org_id>/config.toml

enrich_command = "/usr/local/bin/my-enrich-bridge"
enrich_timeout_seconds = 30
```

When `--enrich` runs against a project with no cached result, the
builder pipes a JSON `Request` on stdin and reads a JSON `Result` on
stdout. The Result is validated (fingerprint match, non-empty
provenance) and atomically written to the cache. A non-zero exit, a
parse failure, or a validation error logs a warning and the project is
treated as a cache miss — the whole build never fails on enrichment
errors. Each invocation is bounded by `enrich_timeout_seconds` (default
30).

### Override precedence

Three sources can populate a project's `summary`, `capabilities`, and
`depends_on` fields:

1. Manual overrides in `<state-dir>/routing-table-overrides.json` —
   tagged `*_source="manual"`. Always wins.
2. LLM enrichment from the cache — tagged `*_source="llm"`.
3. Static signals (README first paragraph, manifest detection, go.mod
   replace directives) — tagged `*_source="readme"`/`"static"`/`"go.mod"`.

`planar workspace routing build --enrich` applies them in that order:
static signals are written first, then LLM results replace any
non-manual values, then manual overrides re-apply unconditionally.

### Inspecting the cache

Each cache file is a small JSON blob — `cat` one to see what the LLM
produced:

```
cat ~/.planar/cache/workspace-enrichment/1/repo-a-<hash>.json
```

The file carries `summary`, `capabilities`, `depends_on`, the
`fingerprint_hash` it was produced against, the `provenance` string,
and an RFC3339 `generated_at` timestamp.

### Invalidating the cache

Cache invalidation is by deletion. The next build with `--enrich` will
treat the project as a cache miss and emit a hint to re-run the skill:

```
rm ~/.planar/cache/workspace-enrichment/1/repo-a-<hash>.json
# or wipe the whole org
rm -rf ~/.planar/cache/workspace-enrichment/1/
```

A README edit or a new file appearing at depth ≤ 2 of the project root
also invalidates the cache implicitly by changing the fingerprint.

---

## Recipe 12 — Working in a polyrepo workspace

Use this when your daily working tree is a workspace directory (`~/work/`, `~/projects/`, etc.) that contains several sibling git repos and you want Planar to coordinate work across them. The workspace surface creates an `org`-kind association, registers each child repo as a project member, and lays down a canonical `AGENTS.md` at the workspace root so any agent that walks the cwd sees the same routing content regardless of vendor.

**What happens:** `planar workspace init` builds the org + project rows in one transaction, creates the state directory under `~/.planar/workspaces/<org_id>/`, scans the member repos to produce a structured routing table, regenerates `AGENTS.md` from that table, and installs symlinks at the workspace root (`AGENTS.md`, `CLAUDE.md`) that point at the canonical content. Subsequent shape changes (new repo, new plans, dependencies shifting) are absorbed by re-running the routing build and regenerate verbs; the symlinks stay valid.

**Scope discipline in a workspace.** The [cross-scope guard](concepts.md#cross-scope-guard) fires on every mutating verb that takes an existing entity id, not only on `spec ingest` (the original lectio incident). When you are working from the workspace root (`~/work/`), cwd resolves to `org:work`; a `planar task update 142` against a task that belongs to `project:repo-a` is refused unless you `cd ~/work/repo-a` first or pass `--scope project:repo-a` explicitly. The same rule applies to `plan update`, `decision update`, `decision supersede`, `audit publish-decision`, `artifact update`, single-target `sync push`/`sync pull`/`sync resolve`, `ext create --from`, `ext propagate`, `link`/`unlink`, and `links update`. Create verbs (`task add`, `decision add`, etc.) and entity-link verbs (`task link`, `links add`, `task touches add`) are not guarded — they are designed to cross scopes.

### Step 1 — Initialize the workspace

From the workspace root (`~/work/` with three `.git`-bearing children):

```
cd ~/work
planar workspace init
```

The verb refuses if `~/work/.git` exists (use `planar init` for a single repo) or if no child has its own `.git` (a workspace must contain repos). On success:

```
created org:work (Work)
  ├─ project:repo-a   [/Users/mn/work/repo-a]   (auto-created, member-of org:work)
  ├─ project:repo-b   [/Users/mn/work/repo-b]   (auto-created, member-of org:work)
  └─ project:repo-c   [/Users/mn/work/repo-c]   (auto-created, member-of org:work)

3 repos initialized as projects, all members of org:work.
Routing table refreshed (3 projects, 1 cross-repo deps).
AGENTS.md regenerated (1842 bytes).
Symlinks installed: AGENTS.md, CLAUDE.md (strategy: symlink).
```

Inspect what landed:

```
ls -la ~/work/AGENTS.md ~/work/CLAUDE.md
# both → /Users/mn/.planar/workspaces/1/AGENTS.md

ls ~/.planar/workspaces/1/
# AGENTS.md  routing-table.json  .manifest-docs
```

### Step 2 — Refresh as work progresses

When new plans land, new tasks are filed, repos are added to the org, or READMEs change, refresh the routing table and the AGENTS.md surface:

```
planar workspace routing build
planar workspace regenerate
```

`routing build` is deterministic and cheap (pure Go, no LLM); run it whenever a shape change should be reflected in the table. `regenerate` re-renders `AGENTS.md` from the latest routing table plus live DB queries (open task counts, open question lists). The symlinks at the workspace root point at the canonical target and never need rewriting.

### Step 3 — Optional LLM enrichment

Static signals (README first paragraph, manifest-derived capability tags) produce a useful table on the first scan. For richer per-project summaries and capability inference, run the `pl-workspace-scan` skill with `--enrich`:

```
/pl-workspace-scan --enrich
```

The skill fingerprints every project (sha256 of README excerpt + sorted depth-2 dir listing), invokes the LLM at `temperature=0`, writes validated results into `~/.planar/cache/workspace-enrichment/<org_id>/`, then runs `planar workspace routing build --enrich` to merge cached results into the table. Cache invalidation is implicit: a README edit or a new file at depth ≤ 2 changes the fingerprint so the next `--enrich` run treats the project as a cache miss. See [Recipe 11](#recipe-11--enriching-a-workspace-routing-table-with-llm-summaries) for the full enrichment contract.

### Step 4 — Inspect the workspace

```
planar workspace doctor
# org:work ok

planar workspace routing show
# Pretty-prints per-project capabilities, summary, dependencies, open-task counts.

planar workspace routing show --json | jq '.projects[] | {slug, capabilities}'
# Machine-readable dump for scripting.
```

`doctor` walks every `kind=org` association, verifies the state directory exists, and reconciles the workspace-root symlinks. It is idempotent and safe to run on every shell startup.

### Step 5 — Add a new repo to an existing workspace

When a new `.git`-bearing directory appears under `~/work/`, register it as a project and link it to the org. The cleanest path uses the existing association verbs and then refreshes the routing surface:

```
cd ~/work/repo-d
planar init                        # registers project:repo-d (cwd is a repo, no guardrail)
planar assoc add org:work ~/work/repo-d

cd ~/work
planar workspace routing build     # picks up the new member
planar workspace regenerate        # AGENTS.md now lists repo-d
```

You can also re-run `planar workspace init` from the workspace root — it is idempotent on existing org / project / membership rows and will pick up the new child repo as part of the same scan. The post-init pipeline rebuilds the routing table and AGENTS.md in one step.

### Step 6 — Retiring a workspace

There is no `planar workspace destroy` verb today. Retirement is a manual operation against the filesystem:

```
# Remove the symlinks at the workspace root
rm ~/work/AGENTS.md ~/work/CLAUDE.md

# Drop the canonical state directory
rm -rf ~/.planar/workspaces/1/

# (Optional) drop the org association from the database
planar assoc remove org:work
```

The `associations` row, the `projects` rows, and their membership links remain in the database after the filesystem cleanup unless the final step is taken. Removing the org also cascades the `project_associations` membership rows; the projects themselves stay (they may belong to other orgs or be referenced directly). Subsequent `planar workspace doctor` runs treat the now-absent state dir as a deleted workspace and emit no warnings for an org that is no longer registered.

### Cross-references

- Concept: [docs/concepts.md § Workspace](concepts.md#workspace).
- Architecture: [docs/architecture.md § Workspace State Directory Model](architecture.md#workspace-state-directory-model).
- CLI verbs: [docs/cli-reference.md § Domain: `workspace`](cli-reference.md#domain-workspace).
- Skill source: [commands/claude/pl-workspace-scan.md](../commands/claude/pl-workspace-scan.md) — full skill body for the LLM enrichment pass.

---

## Recipe 13 — Maintaining the docs surface

Outward-facing docs under `docs/` carry citation footnotes, provenance front matter, and a per-doc record in `.manifest-docs`. Recipes 10 (pre-commit hook) and 12 (workspace) cover the mechanical guards; this recipe covers the human-judgement layer: keeping published docs in sync with internal artifacts, decisions, and done plans over the long arc of a feature.

### Daily / per-PR loop

Every change that touches `docs/` is covered by two verbs.

- `planar doc manifest verify` — O(1) root-hash compare against `.manifest-docs`. The pre-commit hook from [Recipe 10](#recipe-10--doc-hygiene-pre-commit) invokes this; the recipe also explains how to interpret each drift signal.
- `planar doc lint` — validates footnote citations, front-matter `references:` blocks, and entity refs (`artifact:47`, `decision:3`, `plan:12`). Run with `--no-refs` for the offline / fast path.

When either verb fails, fix the issue and retry — the pre-commit hook keeps drift out of the tree.

### After landing a body of work

When an anchor plan moves to `done`, its outward-facing record is a doc citing `plan:<id>` (and any decisions / artifacts that crystallised during the work). Before opening the PR that lands the body of work:

```
planar doc backlinks plan:<id>
```

- One or more paths returned: the historical record is already wired up. Done.
- Empty output: publish a doc that cites the plan. The synthesis prompt lives in the vendor skill — run `pl-doc-promote --kind feature --slug <slug> --source plan:<id>` (or `--kind research` / `--kind getting_started` as appropriate). The skill resolves the doc-prompt template, calls the LLM, writes a synthesised body to a tmpfile, and invokes `planar doc promote --body-file ...` so the result lands atomically with provenance front matter and a manifest entry.

The same loop applies to recording new ADRs (`--source decision:<id>`) or graduating exploratory research notes into a feature catalog entry (`--source artifact:<research-id>`).

### After mutating sources

Editing an artifact, decision, or plan body invalidates any doc synthesised from it. The manifest detects this via per-source xxh64 hashes; surface the affected docs with:

```
planar doc manifest diff
```

Each row is one of:

- `regenerate-candidate` — the doc was last synthesised from a source whose body has changed. Fix with `pl-doc-regenerate <path>` (single doc) or `pl-doc-regenerate --all` (batch). The verb refuses paths classified as `hand-edit`; review each refusal manually before deciding to merge or override with `--force`.
- `hand-edit` — the doc has been edited since it was synthesised. Leave it, or run `pl-doc-regenerate --merge <path>` and review the diff.
- `new-authoring` — a new `*.md` file under `docs/` not yet registered. Either run `planar doc manifest update` (if it's hand-authored) or use `pl-doc-promote` to register it with proper provenance.
- `deletion` — a manifest entry has no on-disk file. Confirm the removal was intentional, then `planar doc manifest update` to drop the record.

The pre-commit hook gates on `manifest verify`, not `manifest diff`, so the diff is your visibility into "what would `manifest update` change". Always read it before running update.

### Quarterly hygiene

Two verbs surface the long-tail debt that daily flow misses.

- `planar doc coverage` — lists every plan with `status=done` that no manifest entry cites. A non-empty list means the historical record has gaps: each row is a done body of work without an outward-facing summary. Pick the highest-value gaps and run `pl-doc-promote --source plan:<id>` to close them.
- `planar doc orphans --kind research` — lists research artifacts older than 30 days (override via `--since N`) that no manifest entry cites. Research artifacts have an explicit lifecycle of "exploratory → cited → promoted or archived"; orphans surfaces the first transition stalling out. For each row decide: promote into a `feature` or `getting_started` doc, or archive the note via `planar artifact status <id> retired`.

Both verbs accept `--json` for scripting. Suggested cadence: run them at the start of each planning cycle and triage into the backlog before scoping new work.

### Cross-references

- Pre-commit hook: [Recipe 10 — Doc hygiene pre-commit](#recipe-10--doc-hygiene-pre-commit).
- Workspace docs surface: [Recipe 12 — Working in a polyrepo workspace](#recipe-12--working-in-a-polyrepo-workspace).
- CLI verbs: [docs/cli-reference.md § Domain: `doc`](cli-reference.md#domain-doc).
- Skill sources: [commands/claude/pl-doc-promote.md](../commands/claude/pl-doc-promote.md) and [commands/claude/pl-doc-regenerate.md](../commands/claude/pl-doc-regenerate.md).

---

## Common Patterns

### Check what is in scope

```
planar scope show
planar task list
planar plan list
```

### Switch association context

```
cd ~/work/my-app           # cwd derivation picks project:my-app
planar task list           # reads scoped from cwd
# or, from anywhere:
planar task list --scope project:my-app
```

### Review the audit trail for an external ticket

```
/pl-audit-trail my-jira:PROJ-1234
```

Shows every local session, decision, commit annotation, and sync event tied to the external ticket.

### Check system health before a session

```
/pl-health
```

Reports schema version, open sessions, unresolved sync conflicts, and handoff readiness. Fix any reported issues before starting work.

### Sync local changes to the remote

```
planar sync push --system my-jira
```

Pushes local mutations (status changes, field updates) to the external system for all entities that have an `external_links(link_role='mirror')` row.

```
planar sync pull --system my-jira
```

Pulls remote changes into `sync_events` rows. Conflicts (both sides changed) are surfaced as `sync_events(outcome='conflict')` and require explicit resolution via `planar sync resolve`.

---

## Recipe 14 — Author a personal skill in the sandbox

Use the local sandbox at `~/.planar/local/` to author personal, machine-local skills and agents that ship into every vendor's surface without the overhead of repo contribution and parity validation. Promotion to the canonical Planar repo is intentionally a separate, manual operation.

### Step 1 — Author the source file

Skills live as dir-shape sources (`<name>/SKILL.md`); agents stay flat:

```
mkdir -p ~/.planar/local/skills/fixup-protos
cat > ~/.planar/local/skills/fixup-protos/SKILL.md <<'EOF'
---
description: "Rebuild and re-import Go protos in the current repo"
tier: medium
---

# Fixup Protos

Steps:

1. Run `make protos`.
2. ...
EOF
```

For agents drop a flat file at `~/.planar/local/agents/<name>.md`.

The dir-shape for skills is what lets the link layer install Codex / Copilot via directory symlinks — those loaders don't follow symlinked SKILL.md files inside real directories, but they do follow directory symlinks pointing at real source dirs. Auxiliary files alongside SKILL.md (helper scripts, datafiles, icons) travel with the skill via the same directory symlink.

**Importing from an external collection.** If the operator already maintains a personal skill folder elsewhere (a separate git repo, a Dropbox directory, a collection of skills), the entire collection can be imported in one shot:

```
planar local import ~/my-skills/                         # collection of flat and/or dir-shape skills
planar local import ~/my-skills/fixup-protos.md          # a single flat file → wrapped into <name>/SKILL.md
planar local import ~/my-skills/jira-bulk-edit/          # a single dir-shape skill (containing SKILL.md)
planar local import ~/my-agents/ --kind agent            # agents go flat to the agents/ dir
```

`planar local import` validates each input's frontmatter, materializes it into the sandbox in the correct shape (skills become `<name>/SKILL.md`; agents stay flat), and (by default) invokes the link step in the same call. Dir-shape inputs preserve any auxiliary files alongside SKILL.md. Name collisions with existing sandbox entries are skipped unless `--force` is passed. Pass `--no-link` to import without linking, or `--dry-run` to preview.

**Migrating from the legacy flat sandbox.** A pre-reshape sandbox stored each skill as a flat `~/.planar/local/skills/<name>.md`. Run `planar local migrate` once to convert these to the dir-shape `<name>/SKILL.md` layout. The verb is idempotent and refuses to overwrite operator content (collisions are skipped with a reason). `planar local link` flags any remaining flat skill files with a warning pointing at the migrate verb.

### Step 2 — Link the source into every vendor surface

```
planar local link
```

Walks every source under `~/.planar/local/{skills,agents}/`, parses each one's frontmatter, and creates per-vendor installs:

```
fixup-protos (skill)
  claude   created [symlink]  →  /Users/you/.claude/commands/local-fixup-protos.md
  codex    created [symlink]  →  /Users/you/.codex/skills/local-fixup-protos
  copilot  created [symlink]  →  /Users/you/.copilot/skills/local-fixup-protos

done: 3 linked, 0 unchanged, 0 skipped across 1 source(s)
```

The `local-` prefix on the install name makes sandbox skills visibly user-authored in every vendor's listing. Claude installs are flat `.md` files (file symlink → `<src>/SKILL.md`); Codex and Copilot install as directory symlinks pointing at the source dir — that's the shape each vendor's discovery loader expects (see *Per-vendor install layout* in [concepts.md](concepts.md#local-sandbox)).

Useful flags:
- `--dry-run` previews without writing.
- `--vendor claude` (repeatable) restricts the operation to one or more vendors. Composes with the source's `vendors:` frontmatter list (intersection).
- `--reconcile` walks the manifest, removes entries whose source file has been deleted by hand, and removes the per-vendor installs those entries pointed at.

### Step 3 — Iterate

Edit the source SKILL.md directly:

```
vim ~/.planar/local/skills/fixup-protos/SKILL.md
```

The vendor surface paths resolve to the source through symlinks (file symlink for Claude, directory symlink for Codex / Copilot), so every vendor sees the edit immediately — no re-link required. Exception: if `os.Symlink` failed at link time (filesystems that reject symlinks), the link layer falls back to a file copy (or a directory-tree copy for the dir-symlink layout) and the operator must re-run `planar local link` after every edit. The link layer emits a warning naming this case so it is never silent.

### Step 4 — Restrict to one vendor (optional)

Set `vendors:` in the source frontmatter to opt out of vendors that do not apply:

```yaml
---
description: "Claude-specific helper"
vendors: [claude]
---
```

### Step 5 — Shadow a canonical install (advanced)

Set `shadow: true` in the source frontmatter to drop the `local-` prefix on every linked filename:

```yaml
---
description: "My override for the canonical fixup-protos"
shadow: true
---
```

The sandbox file then shadows whatever canonical install carries the same name. `planar local link` always emits a warning naming the shadowed target so this is never silent.

### Step 6 — Inspect installs

```
planar local list
```

```
name          kind     vendor   status   target
fixup-protos  skill    claude   live     /Users/you/.claude/commands/local-fixup-protos.md
fixup-protos  skill    codex    live     /Users/you/.codex/skills/local-fixup-protos
fixup-protos  skill    copilot  live     /Users/you/.copilot/skills/local-fixup-protos
```

`live` means the install file exists and (for symlinks) resolves to the source. `broken` means the symlink target is gone or wrong. `missing` means the install file was deleted out from under the manifest. Statuses colorize via the standard palette.

### Step 7 — Remove an install

```
planar local unlink fixup-protos
```

Removes every per-vendor install for `fixup-protos`. The source file in `~/.planar/local/` is preserved. To also delete the source, pass `--purge`.

### Step 8 — Promote to canonical (manual)

Once a sandbox skill earns its keep, promote it by copying the file into the repo by hand:

```
cp ~/.planar/local/skills/fixup-protos/SKILL.md commands/claude/fixup-protos.md
cp ~/.planar/local/skills/fixup-protos/SKILL.md skills/codex/fixup-protos.md
cp ~/.planar/local/skills/fixup-protos/SKILL.md skills/copilot/fixup-protos.md

git add commands/claude/fixup-protos.md skills/codex/fixup-protos.md skills/copilot/fixup-protos.md
git commit -m "skill: fixup-protos"
git push   # PR through the normal contribution flow
```

There is no `planar local promote` shortcut — that is deliberate. Canonical skills go through `planar skills render --check` against an out-of-tree staging dir, any remaining relevant validators, and contribution review; sandbox skills do not. Keeping the boundary loud preserves the difference. After promotion, you can run `planar local unlink fixup-protos --purge` to retire the sandbox copy.

---

## Recipe 15 — Editor-first authoring

Plan 226 closed the friction of the original `push → edit → pull` rhythm: every entity now has a unified `<entity> edit <id>` verb that pushes (if needed), opens `$EDITOR` on the workbench file, validates frontmatter mutations on save, and pulls the result back into the DB. Companion `view`/`diff` verbs and bulk-review surfaces round out the loop.

### Step 1 — Edit one entity

```
planar artifact edit 42
```

The verb opens the workbench file for artifact 42 in `$EDITOR`. On save:

- The body is applied to the DB row.
- Allowed frontmatter mutations (title, status, priority on tasks, etc.) are applied through the M5 mutation validator.
- Disallowed mutations (identity fields, anchor moves without `--allow-replan`) are rejected; the workbench file is preserved so the operator can fix and retry.

Same shape across kinds: `task edit`, `question edit`, `decision edit`, `scenario edit`.

### Step 2 — View or diff without editing

```
planar artifact view 42      # exec $PAGER on the workbench file
planar artifact diff 42      # unified diff between DB and FS
```

`view` is the read path; `diff` shows where the FS file has diverged from what the DB would render. Both auto-push if the file is missing.

### Step 3 — Resolve a concurrent-change conflict

If the DB changes between editor-open and save (concurrent `task update`, `workbench pull`, etc.), the save detects the conflict and refuses by default. Pick a resolution policy explicitly:

```
planar task edit 47 --force-conflict-resolution=remerge   # open a three-way file with conflict markers
planar task edit 47 --force-conflict-resolution=force     # overwrite (warns naming the discarded changes)
planar task edit 47 --force-conflict-resolution=abort     # default: refuse and exit non-zero
```

### Step 4 — Bulk-review across many entities

For sweeping through every open question (or every open task) tied to a plan:

```
planar question review 226
planar task review     226
```

Each verb builds a focal file (`REVIEW.md` or `TASKS.md`) in a temp session directory, copies the originating artifact alongside as context, and execs `$EDITOR` on the directory. On save, populated blocks become per-entity verb invocations:

- Question review: a non-blank `### Answer` block → `planar question answer <id> "<body>"`; a single line `WONTFIX` (case-sensitive) → `planar question wontfix <id>`; blank → skip.
- Task review: each task block has editable `Status:`, `Priority:`, `Next-action:` lines; non-empty changes apply via `planar task update`.

The session directory is removed on success; preserved on save failure so unsaved work survives. Add `--editor-hints` to drop a `.code-workspace` (VS Code) and `.vimrc-local` (Vim) in the session dir.

### Step 5 — Bulk-edit the whole feature

```
planar workbench edit 226
```

Push the plan's entities to the workbench, exec `$EDITOR` on the feature directory, pull on save. Use this when you want to edit several files in one session and let the pull machinery reconcile them.

### When to reach for which verb

- One file, one entity: `<entity> edit`.
- Read-only inspection: `<entity> view`.
- "Is the FS file in sync with the DB?": `<entity> diff`.
- Sweep through open questions or tasks on a plan: `<entity> review`.
- "I want to edit 4 files at once on one plan": `workbench edit`.
- Old `push → edit → pull` rhythm: still works; it's just no longer the default.

## Recipe 16 — Observing agent activity from the operator binary

You are an operator watching a feature in flight. One or more agents (planner, coder, reviewer, test-coder, orchestrator, ext-sync) are running against the plan you own, each holding a claim on a task and emitting actions. This recipe walks the **read** verbs on the `planar` binary that give you a complete operator-side view of what's happening, without ever calling the write side.

### What lives where

By design, the `planar` binary has **no `planar agent` subcommand**. Agent observability is split across three surfaces and they earn their separate places:

| Binary | Role | Verbs |
|--------|------|-------|
| `planar` (this binary) | Operator reads of agent state, folded into existing verbs. | `dashboard --agents`, `plan next`, `tree`, `audit trail`, `health` |
| `planar-agent` | Agent ritual + operator-recovery writes. Owns every write to `agent_work_claims` / `agent_actions`. | `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`end`, `ingest`, `reconcile`, `abort` |
| `planar-watch` | Live streaming viewer (forthcoming in plan 85 M8). Pure read. | `feed`, `ps`, `claims`, `actions`, `plans`, `log` |

This recipe covers the **planar** binary's read surface. The ritual recipe for `planar-agent` lives in plan 85 M5; the streaming-viewer recipe for `planar-watch` lives in plan 85 M8.

### Step 1 — Survey what's in flight

```
planar dashboard --agents
```

Output (text mode):

```
active plans: 3    active claims: 2    stale claims: 0
  plan:85  [active]  Agent activity tracking
  plan:88  [active]  Doc-system improvements
  plan:91  [draft]   Workbench redesign
active claims:
  task:541  vendor:claude-code  branch:feat/m3-reads  sha:a1b2c3d4  dirty:dirty  repo:/home/me/work/planar  token:9f2c…
  task:548  vendor:codex        branch:doc/regenerate sha:5e7f9012  dirty:clean  repo:/home/me/work/planar  token:7a13…
next available by plan:
  plan:85  available:1
  plan:88  available:4
  plan:91  available:0
```

The `--agents` flag is the toggle. Without it, `dashboard` is a plain plan summary; with it, the same verb folds in the live claim block and the per-plan "next available" tally. The locality columns (`branch`, `sha`, `dirty`, `repo`) come straight off `agent_work_claims` — if you see a claim on a dirty checkout or a branch that disagrees with your own, that's the signal to step in.

For scripted dashboards add `--json`:

```
planar dashboard --agents --json | jq .
```

The JSON shape pins the contract: `active_plans`, `claims.{active,stale}`, `next_available_by_plan`. See `docs/cli-reference.md` § `planar dashboard` for the full row shapes.

### Step 2 — Drill into one plan's queue

```
planar plan next 85 --include-claimed --include-stale
```

Output:

```
plan:85  available:1  claimed:1  stale:0  blocked:0  done:9
  available  task:551  M3 cycle B reviewer fixes  [pri:20]
  claimed    task:541  planar plan next selector  [pri:10, claim:9f2c…]
```

The bucket breakdown is the operator counterpart to `planar-agent peek`. Where `peek` returns the single highest-priority pickable task (the next thing an agent would `pull`), `plan next` returns the FULL classification: `available`, `claimed`, `stale`, `blocked`. The text-mode renderer hides `claimed` and `stale` rows by default so the queue you scan first is the available work; pass `--include-claimed` / `--include-stale` to surface them.

The JSON shape carries every bucket regardless of the text-mode flags:

```
planar plan next 85 --json | jq '.summary, .claimed[0]'
```

### Step 3 — Walk a plan's hierarchy

```
planar tree --scope plan:85
```

The `tree` verb is unchanged by the agent-activity work, but it remains the structural counterpart to `plan next`: `plan next` tells you "what is pullable right now", `tree` tells you "what is the shape of this feature". Use both when triaging.

### Step 4 — Inspect the audit trail for one entity

```
planar audit trail task:541
```

The `audit_trail` read surface stitches together `audit_log` (the operator-write log) with `entity_links` and now joins against `agent_actions` for any actions taken on the entity. You see each role's `started_at` / `ended_at` / `outcome` plus the operator-side decisions that ran around them.

### Step 5 — Confirm the database is healthy

```
planar health
```

`health` is the always-on smoke check: schema-current, integrity, in-flight tasks resumable, pending handoffs fresh. It is not agent-activity-aware (the claim table is consulted by `dashboard --agents` and `plan next`, not by health), but it is the first verb to run when something looks wrong before you spend time chasing the wrong layer.

### Putting it together

The full operator-side observation loop is exactly these five verbs. None of them write; they read across `plans`, `tasks`, `agent_work_claims`, `agent_actions`, `audit_log`, and `entity_links` to produce the operator view:

```
planar dashboard --agents              # who is doing what, right now
planar plan next <plan> --include-claimed --include-stale
                                        # one plan's queue, every bucket
planar tree --scope plan:<plan>        # the feature's structure
planar audit trail <kind:id>           # one entity's history
planar health                          # is the database itself OK
```

When you find a stuck claim (active for too long, branch you do not recognise, agent vanished), the recovery verbs live on the other binary:

```
planar-agent reconcile --dry-run       # what would be marked stale
planar-agent reconcile                 # mark expired-lease claims stale
planar-agent abort --claim <token> --reason "operator: agent gone silent"
                                        # force-release a specific claim
```

`reconcile` and `abort` live on `planar-agent` because both are writes to `agent_work_claims` and the capability boundary tracks tables, not audience. The `planar` binary remains write-free on the agent activity layer.

## Recipe 17 — Wiring Claude Code hooks to `planar-agent ingest`

Claude Code emits a JSON hook event for each session-lifecycle and message-level transition. Pipe those events into `planar-agent ingest --vendor claude` and Planar opens (or reuses) a `sessions` row and records each event as an `agent_actions` row. The same operator-side surfaces from Recipe 16 (`dashboard --agents`, `tree`, `audit trail`) then show the activity automatically.

### Prerequisites

- `planar init` has been run against the database the agent should write to. The agent and operator binaries must agree on `$PLANAR_DB` (default `~/.planar/state.db`).
- The Planar binaries are on `$PATH`; `which planar-agent` should print a real path.

### Step 1 — Configure the Claude Code hook

Edit `~/.claude/settings.json` (the per-user Claude Code settings) and add a hook entry for each event type you want recorded. The minimal viable subset is the five event types the M4 adapter knows:

```json
{
  "hooks": {
    "session_start": [
      {
        "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/state.db" }
      }
    ],
    "session_end": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/state.db" } }
    ],
    "user_message": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/state.db" } }
    ],
    "assistant_message": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/state.db" } }
    ],
    "tool_call": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/state.db" } }
    ]
  }
}
```

Notes on the invocation:

- `--vendor claude` selects the Claude adapter. `--vendor copilot` selects the GitHub Copilot adapter wired in M6 (see Recipe 18); `--vendor codex` parses but exits non-zero — the codex slot is reserved for a future adapter.
- `--event @-` reads the hook payload from stdin; Claude Code pipes the JSON envelope to the configured command. Use `--event @<path>` if your hook runner stages payloads on disk instead.
- `PLANAR_DB` MUST point at the same database file `planar` and `planar-watch` open. The hook subprocess does not inherit your shell's `$PLANAR_DB`, so set it explicitly in the hook's `env` block.
- Each invocation opens its own SQLite connection. WAL mode (set by the engine on connect) lets dozens of concurrent hooks coexist with `planar-watch --follow` readers.

### Step 2 — Smoke-test the wiring by hand

Before letting Claude Code drive it, confirm the binary round-trips a payload locally:

```sh
echo '{"event_type":"session_start","session_id":"smoke","model":"claude-opus-4-7"}' \
  | planar-agent ingest --vendor claude --event @- --json
```

Expected output (single-line):

```
{"ok":true,"sessions_created":1,"claims_created":0,"actions_created":0,"events_processed":1}
```

A second invocation with the same `session_id` MUST report `sessions_created:0` (the row was reused). A `tool_call` payload increments `actions_created` instead.

### Step 3 — Observe the session

With the hook firing, Recipe 16's verbs show the activity:

```sh
planar dashboard --agents          # active vendor sessions
planar audit session <id>          # the session_entries timeline
```

The session id surfaced by `dashboard --agents` is the Planar-side id; pair it with the vendor's `session_id` field from the hook payload via `vendor_session_id` in the JSON shape.

### Failure modes

- **Malformed payload** (`{` truncated, not JSON): `ingest` exits non-zero with `error: malformed event payload…` on stderr and writes no rows.
- **Unknown `event_type`** (the payload is valid JSON but the event_type field is outside the adapter's known set): `ingest` exits non-zero with `error: unknown event_type in payload…`. Add the new event type to `src/engine/external/agentingest/claude.zig` if you want it recorded.
- **`$PLANAR_DB` missing or unreadable**: the schema-version handshake fails at startup; `ingest` exits non-zero before parsing.

Both error paths are atomic — the surrounding `BEGIN IMMEDIATE` transaction rolls back so no partial rows land in `sessions` or `agent_actions`.

## Recipe 18 — Wiring GitHub Copilot hooks to `planar-agent ingest`

The M6 second-vendor adapter ships Copilot support behind `--vendor copilot`. GitHub Copilot — the Coding Agent and the Copilot Chat session surface — emits a namespaced event taxonomy (`session.started`, `session.completed`, `turn.user`, `turn.assistant`, `tool.invocation`) carried on a JSON envelope. Pipe those events into `planar-agent ingest --vendor copilot --event @-` and Planar opens (or reuses) a `sessions` row and records each event as an `agent_actions` row exactly as it does for Claude. The same operator surfaces from Recipe 16 (`dashboard --agents`, `tree`, `audit trail`) then show Copilot activity interleaved with Claude activity.

### Prerequisites

- `planar init` has been run against the database Copilot should write to. Copilot's hook subprocess and the operator binaries must agree on `$PLANAR_DB` (default `~/.planar/state.db`).
- The Planar binaries are on `$PATH`; `which planar-agent` should print a real path.
- A Copilot deployment that supports outbound event webhooks or shell-hook commands. Both the GitHub Copilot Coding Agent (server-side) and a self-hosted Copilot-style runner (e.g. a CI bot wired to the Copilot Chat API) can be configured to fire one shell command per event.

### Step 1 — Configure the Copilot hook

The exact configuration surface depends on which Copilot product you're wiring:

- **GitHub Copilot Coding Agent** (per-repo or org-level): add a `copilot.hooks` entry to the repo's `.github/copilot.yml`. The runner pipes each event to the configured command on stdin.

  ```yaml
  copilot:
    hooks:
      session.started:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/state.db"
      session.completed:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/state.db"
      turn.user:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/state.db"
      turn.assistant:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/state.db"
      tool.invocation:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/state.db"
  ```

- **Self-hosted Copilot Chat runner**: register a webhook target that POSTs each event as JSON to a small forwarder, then `curl --data-binary @-` it into `planar-agent ingest --vendor copilot --event @-`. The forwarder is a one-liner; the payload shape is the same.

Notes on the invocation:

- `--vendor copilot` selects the Copilot adapter wired in M6. `--vendor claude` selects the Claude adapter (Recipe 17); `--vendor codex` is reserved and exits non-zero today.
- `--event @-` reads the hook payload from stdin. Use `--event @<path>` if your runner stages payloads on disk first.
- `PLANAR_DB` MUST point at the same database file `planar` and `planar-watch` open. Hook subprocesses do not inherit your shell's `$PLANAR_DB`; set it explicitly in the `env` block.
- Each invocation opens its own SQLite connection. WAL mode (set by the engine on connect) lets dozens of concurrent hooks coexist with `planar-watch --follow` readers.

### Step 2 — Smoke-test the wiring by hand

Before letting Copilot drive it, confirm the binary round-trips a payload locally:

```sh
echo '{"event":"session.started","session_id":"smoke","model":"gpt-5-copilot","role":"coder"}' \
  | planar-agent ingest --vendor copilot --event @- --json
```

Expected output (single-line):

```
{"ok":true,"sessions_created":1,"claims_created":0,"actions_created":0,"events_processed":1}
```

A second invocation with the same `session_id` MUST report `sessions_created:0` (the row was reused). A `tool.invocation` payload increments `actions_created` instead.

### Step 3 — Observe the session

```sh
planar dashboard --agents          # active vendor sessions (claude + copilot interleaved)
planar audit session <id>          # the session_entries timeline
```

The session id surfaced by `dashboard --agents` is the Planar-side id; pair it with the vendor's `session_id` field from the hook payload via `vendor_session_id` in the JSON shape. Sessions from the two vendors live in the same `sessions` table — they're distinguished by the `vendor` column.

### Event mapping reference

Copilot's namespaced events are collapsed into the same normalized `Event` shape Claude uses. The mapping is:

| Copilot `event`        | Normalized variant | Notes                                       |
|------------------------|--------------------|---------------------------------------------|
| `session.started`      | `session_start`    | New session OR reuse if `session_id` known. |
| `session.completed`    | `session_end`      | No-op if session already ended.             |
| `turn.user`            | `action_atomic`    | `action_kind=user_message`.                 |
| `turn.assistant`       | `action_atomic`    | `action_kind=assistant_message`.            |
| `tool.invocation`      | `action_atomic`    | `action_kind=tool_call`.                    |

The Copilot adapter accepts BOTH `status` (Copilot's documented field name) and `outcome` (Claude's, for parity) on the atomic-action events; `status` wins when both are present. Allowed values: `ok` | `error` | `aborted` | `timeout`. Default is `ok`.

### Failure modes

- **Malformed payload** (`{` truncated, not JSON, missing required `event` or `session_id` field): `ingest` exits non-zero with `error: malformed event payload (vendor=copilot)…` on stderr and writes no rows.
- **Unknown event** (payload is valid JSON but `event` is not in the table above): `ingest` exits non-zero with `error: unknown event_type in payload (vendor=copilot)…`. Add the new event name to `src/engine/external/agentingest/copilot.zig` if you want it recorded.
- **`$PLANAR_DB` missing or unreadable**: the schema-version handshake fails at startup; `ingest` exits non-zero before parsing.

Both error paths are atomic — the surrounding `BEGIN IMMEDIATE` transaction rolls back so no partial rows land in `sessions` or `agent_actions`.

## Recipe 19 — Live agent cockpit with `planar-watch`

`planar-watch` is the third binary in the three-binary architecture — the **human-facing read-only viewer**. It opens the database in strict read-only mode (`SQLITE_OPEN_READONLY`); the SQLite driver itself refuses every write SQL string, which is the second line of defense behind the binary's "no write verbs registered" capability boundary. The first defense is the verb tree itself: it contains exactly six read verbs — `feed`, `ps`, `claims`, `actions`, `plans`, `log` — plus the conventional `version` and `completion` helpers, and zero anything that mutates state.

This recipe walks the streaming-cockpit workflow. The companion recipe for the operator's read-fold-ins on the `planar` binary lives in Recipe 16.

### Step 1 — Stream the cross-cutting activity feed

```
planar-watch                                     # default: feed
planar-watch feed --follow                       # stream until SIGINT
planar-watch feed --follow --json | jq -c .      # NDJSON, one event per line
planar-watch feed --vendor claude --plan 85      # narrow by vendor + plan
planar-watch feed --since 2026-05-27T00:00:00Z   # only newer events
```

`feed` is the lowest-friction view: every claim transition, action transition, and (where applicable) task status change ordered by occurrence time. With `--follow` the loop polls every `--interval` (default `1s`; e.g. `100ms` for tests). The JSON event shape is stable across the transport-tier ladder — see `docs/architecture.md` § "Live tail / follow implementation".

### Step 2 — Snapshot the active claims (`ps`)

```
planar-watch ps                                  # text table
planar-watch ps --json                           # generated_at + active + stale arrays
planar-watch ps --stale                          # also include lease-expired / stale
planar-watch ps --vendor codex --follow          # live filter
```

`ps` is the operator's "what's running right now" snapshot. With `--stale` it also includes claims whose lease has expired (and that `planar-agent reconcile` would mark stale on its next sweep) — the diagnostic surface for wedged agents.

### Step 3 — Drill into one claim or task

```
planar-watch log --task 541 --json               # task history (actions + claim transitions)
planar-watch log --claim 9f2c… --json            # one specific claim_token's history
planar-watch log --plan 85 --json
planar-watch log --entity question:42 --json
planar-watch log --session 17 --json
```

`log` accepts exactly one filter (`--task` | `--plan` | `--entity` | `--session` | `--claim`). The output is a discriminated union: each entry carries `.kind = "action"` or `"claim_acquired"` / `"claim_released"` / `"claim_completed"` / `"claim_aborted"` / `"claim_stale"`, with the matching ActionRow or ClaimRow payload.

### Step 4 — Plan rollup

```
planar-watch plans --in-flight-only              # plans with live work only
planar-watch plans --json --follow
```

Each row pairs a plan with its in-flight summary: `active_claims`, `active_actions`, `last_event_at`. Use `--in-flight-only` to skip idle plans.

### Step 5 — Lower-level ledgers (`claims`, `actions`)

```
planar-watch claims --status active|stale|all --json
planar-watch actions --kind coder --limit 50 --json
planar-watch actions --entity task:541 --json
```

`claims` and `actions` are the unbucketed ledgers — they return whatever rows match the filters in one flat array, suitable for downstream tools that need full table reads rather than the synthesized buckets `ps` / `plans` emit.

### Capability boundary

The verb set is enforced by `src/cmd/planar-watch/handlers/cmd.zig`: there is no `pull`, `claim`, `complete`, `fail`, `release`, `block`, `heartbeat`, `action`, `ingest`, `reconcile`, or `abort` in the tree. The strict-read-only DB handle (`runtime.ensureDbStrictReadOnly` → `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)`) refuses any write SQL with `SQLITE_READONLY` at the driver layer — verified by the `openReadOnly: write SQL is rejected at the driver layer` unit test in `src/db/sqlite.zig`. Both defenses must be in place; either failing alone is treated as a regression by `integration_tests/planar_watch_test.zig`.
