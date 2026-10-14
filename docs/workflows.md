# Planar Workflows

End-to-end recipes for common Planar operations. Where a recipe names a role (drafting, ingesting, orchestrating, reviewing, syncing), the example `planar-<role>` agents are one way to do that work; everything verb-shaped is a CLI command, and `planar <verb> --help` is the reference for its flags. One skill, `planar`, is a short reference for what Planar does, and the recipes name its references where one applies; see [skill-reference.md](skill-reference.md). These assume you have run `planar init`, followed its printed `planar assoc create` / `planar assoc add` next steps to establish a project association, and understand the core concepts (see `docs/concepts.md`). `planar plan create` refuses to infer global ownership from an initialized but unassociated project; use `--scope global` only when that ownership is intentional.

---

## The Planning Pipeline at a Glance

The planning loop goes from goal statement through spec drafting, task decomposition, execution, and operational-plane propagation. The five steps below are the whole loop; Recipes 1, 2, 3 and 6 cover each in depth.

```
# 1. Draft a feature spec and roadmap into the workbench: dispatch the
#    planar-planner agent with the goal "migrate billing service to v2 API".

# 2. Review the Markdown files under ~/.planar/workbench/, edit as needed, then ingest.
planar spec ingest 7 --strict --json   # read-only preview
planar spec ingest 7 --apply           # decomposes roadmap bullets into tasks + scenarios

# 3. Execute through the standard coder/reviewer loop: dispatch the
#    planar-orchestrator agent for plan 7.

# 4. Propagate the feature to the operational plane (Jira or GitHub Issues).
planar-ext ext propagate 7 --system my-gh

# 5. Archive the workbench tree when done (DB retains all entities).
planar workbench archive 7
```

Steps 1 and 3 are agent dispatches; the others are verbs. The example `planar-orchestrator` agent runs explicit phases (planning / spec review / ingestion / execution / finalization / propagation / archive) with user gates after drafting, before ingestion `--apply`, and before dispatch; Planar does not require those gates. The agents are listed in [skill-reference.md](skill-reference.md#the-role-agents).

---

## Recipe 1 — Start a New Feature

Use this when you have a goal and want to produce a structured plan with tasks ready for execution.

**What happens:** The planner agent drafts **four** planning documents into the workbench in four sequential authoring phases (product → tech → roadmap → test). The user reviews and edits them. The ingestor decomposes them into plans, tasks, decisions, and scenarios in the database.

### Step 1 — Draft planning documents

Dispatch the `planar-planner` agent with the goal `add billing export to CSV`. From a
coding agent with the `planar` skill installed, ask for it from inside the project:

```text
Draft a Planar spec for adding billing export to CSV.
```

Name the agent if you want to be explicit: "use the planar-planner agent to draft …".

The planner creates:
- A draft anchor plan with a generated slug (e.g. `billing-export-csv`).
- A workbench directory at `~/.planar/workbench/<assoc>/p<id>-billing-export-csv/`.
- Four artifact files: `product-spec.md`, `tech-spec.md`, `roadmap.md`, `test-spec.md`.

Expected output:
```
plan 42 created  [draft]  billing-export-csv
workbench: ~/.planar/workbench/project_my-app/p42-billing-export-csv/
artifacts: product-spec.md, tech-spec.md, roadmap.md, test-spec.md
```

### Step 2 — Review and edit

<!-- surface-lint-ignore surface-path-missing: illustrative touches-annotation syntax, not a real path -->
Open the workbench directory. Read all four documents. Edit the `roadmap.md` section headers and bullet points — these become plan and task rows. **Annotate every testable bullet with a `[slug: foo-bar]` tag** so the test-spec can cite it by stable name even before ids are assigned. **Also annotate the files each bullet will touch** — `[touches: src/engine/x.cpp]`, or `[touches: <repo-slug>:src/engine/x.cpp]` in a polyrepo association. This is the cheapest moment to record it: you are holding the spec and already know the target files, whereas inferring them later from prose recovers only 20–44% of tasks. Touch declarations are what parallel-eligibility rule 2 reads; a task with none is treated as touching everything and never runs in parallel. Edit the `## Decisions` section in `tech-spec.md` — these become decision rows. Edit acceptance criteria in `product-spec.md` — these become scenario rows. Edit the `## Scenarios` section in `test-spec.md` — each `### Scenario:` H3 becomes a `test_scenarios` row with `verifies` edges to the cited tasks. Cite tasks via `**Verifies:** task:<slug>` referencing the `[slug: …]` annotations on the roadmap bullets. The test-spec's four return-path buckets (happy / empty-null / error / edge) are a **coverage-reasoning lens** — name the bucket in each scenario title (e.g. `### Scenario: Error return — export fails on missing header`), not as `### <bucket>` group headers; the coverage-gap checklist at the bottom is the reviewer's compliance pass.

When satisfied, signal readiness to ingest.

### Step 3 — Preview ingestion

```
planar spec ingest 42
```

The `planar-ingestor` agent runs this preview for you; you can run it directly too. It reads the workbench documents and prints a tree-shaped diff of proposed additions:

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

The `coverage:` line follows the totals. It reports how many of the tasks have a `[slug:]` annotation and how many of those slugs are cited by at least one scenario in `test-spec.md`. Uncovered slugs and orphan scenarios (missing `**Verifies:**` line) are listed inline and reported as warnings; they do not fail the preview. `--strict` adds the slug-collision refusal:

```
planar spec ingest 42 --strict
```

For the authoritative pre-ingest check, request the machine-readable preview:

```
planar spec ingest 42 --strict --json
```

The draft is not ready when the command exits non-zero, which `--strict` does
for a slug collision or a reference that resolves to no task, or when the
top-level `slug_collisions` array is non-empty. Collisions require choosing a
globally unique task slug; an unresolved `**Verifies:**` or `[depends:]` slug
needs fixing in the artifact that names it. `coverage.uncovered_task_slugs` and
`coverage.orphan_scenarios` are reported and do not block: they usually mean
adding a missing `[slug:]` to a roadmap bullet or filling in a scenario's
`**Verifies:**` line, and are worth reading before `--apply`.

After `--apply` has created task, scenario, and verifies rows, use the read-only
live-row inspector for a per-milestone view with a four-bucket breakdown by
scenario-title prefix:

```
planar test-spec status 42
```

No changes are written by either preview or `test-spec status`. Do not use
`test-spec status` before apply as proof of draft completeness: zero totals are
legitimate when live rows do not exist yet. The strict preview's `coverage`
object is the pre-ingest oracle; `test-spec status` is the post-ingest oracle.

### Step 4 — Apply

Review the diff. If it looks correct:

```
planar spec ingest 42 --apply
```

`--apply` writes the diff the preview showed. Add `--strict` to the apply call to refuse on a slug collision. Uncovered slug-bearing tasks are reported as warnings, so read the `coverage:` line before you apply.

Tasks, child plans, and decisions are now in the database. A successful apply also activates a `draft` anchor plan (the apply summary ends with `(anchor plan activated)`). Confirm before execution:

```
planar plan show 42
# status: active
```

---

## Recipe 2 — Run the Orchestrator

Use this when an anchor plan is `active` and you want to execute tasks.

**What happens:** Phase 3 runs two gates in order before dispatching any coder. Gate 1 — strategy + isolation — recommends an execution strategy (`classic`, `barrel-deferred`, `barrel-bypass`, or `parallel-fanout`) and isolation mode (`pwd` or `worktree`), then lists the tasks by wave. Gate 2 picks the dispatch shape nested under that strategy. Each coder output goes through the test-coder coverage gate when needed and then the reviewer unless the confirmed mode bypasses or defers review. The reviewer loop cap is 5 iterations per dispatch cycle.

### Invoke

Dispatch the `planar-orchestrator` agent with plan 42, naming any options from the table below in the brief:

```text
Orchestrate plan 42 with planar-orchestrator, worktree isolation.
```

The orchestrator reads claim-aware task state, then opens Gate 1:

```
Plan 42 "billing-export-csv" — 5 todo tasks

Phase 3 plan:

  wave 1
    #43  add-export-jobs-table     blocks: 45
    #44  add-billing-exports-table —
  wave 2 — unblocks when #43 is done
    #45  write-migration-0008      blocked_by: 43
    #46  implement-csv-serialiser  —
    #47  wire-up-export-endpoint   —

Recommended strategy: classic  pwd
Rationale: 5-task plan, sequential migration dependencies across 2 waves

  classic pwd       — sequential in current checkout; reviewer per cycle.
  classic worktree  — sequential cycle worktrees; reviewer per cycle.
  barrel-deferred   — back-to-back coder cycles; reviewer at boundary.
  barrel-bypass     — no reviewer; gates are the entire signal.
  parallel-fanout   — staged worktree fan-out; N coders concurrently.

Confirm strategy and isolation? [classic pwd / classic worktree / barrel-deferred pwd /
  barrel-deferred worktree / barrel-bypass / parallel-fanout]
```

After confirming the strategy, Gate 2 opens:

```
Phase 3 dispatch shape for plan 42:

Proposed dispatch shape: grouped
  Group A: tasks 43, 44, 45  (all touch migrations/ — one coder cycle)
  Group B: tasks 46, 47      (independent of Group A)

Accept this shape?
[yes / edit / strict / single]
```

Confirm (`yes`) or adjust. The orchestrator then dispatches Group A to a freshly spawned coder subagent, routes the output through Phase 3.5 when needed, and either dispatches the reviewer or records the confirmed reviewer disposition. On `approve`, tasks flip to `done` through `planar-agent complete` and Group B is dispatched.

### Brief options

These options are named in the brief the orchestrator is dispatched with.

| Option | Effect |
|------|--------|
| `--strategy <name>` | Pre-commit an execution strategy (`classic`, `barrel-deferred`, `barrel-bypass`, `parallel-fanout`); skip the strategy gate |
| `--isolation <pwd|worktree>` | For sequential strategies, run cycles in the current checkout (`pwd`) or an isolated git worktree (`worktree`); defaults to `pwd` |
| `--strict` | One coder cycle per task; skip the dispatch-shape gate |
| `--grouped` | Orchestrator picks groupings; skip the gate |
| `--batch 43,44,45 --batch 46,47` | Explicit groupings; skip the gate |
| `--finalize` | Run Phase 3.7 finalization after execution cycles complete; dispatches the janitor to merge, reconcile, and close out the plan |
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

Intersection non-empty → dispatching planar-test-coder for slugs:
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

Dispatch the `planar-test-coder` agent with a task id (that task's cited scenarios) or with a plan id (every cited scenario in the plan).

Useful after authoring a new test-spec for an older feature, or for a second-pass coverage check on a PR.

### Picking a barrel mode

The orchestrator's gate offers six shapes — three classic (`strict`, `grouped`, `single`) and three barrel modes (`barrel-grouped`, `barrel-deferred`, `barrel-bypass`). Each barrel mode trades safety for throughput at a different point. The worked examples below show the gate proposal, the audit-trail excerpt, and when to pick each.

#### barrel-grouped (alias for grouped, explicit barrel namespace)

Dispatch `planar-orchestrator` for plan 42 with `--barrel-grouped`.

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

Dispatch `planar-orchestrator` for plan 42 with `--barrel-deferred` (the default boundary is per milestone), or with `--barrel-deferred --barrel-deferred-at plan` to review once at the end of the plan.

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

Dispatch `planar-orchestrator` for plan 42 with `--barrel-bypass`.

No reviewer dispatch at all. Coder cycles run back-to-back; quality gates (`make fmt-check` + `make build` + `make test` **twice** + `make coverage` + `make cli-usage-check` + `make surface-lint` + any remaining relevant validators) are the entire signal.

Audit-trail excerpt:
```
dispatch_shape: barrel-bypass
reviewer_disposition: bypassed
cycle_scope: plan:42 milestone:43
tasks: [44, 45, 46]
```

**Pick when:** you trust the gates completely (large existing test surface, strict typing, lint/validator coverage in place) and want to ship the plan as fast as possible. Common for docs-only plans or methodology updates where the diff IS the verification.

**Trade-off:** uncaught defects must surface via runtime testing or out-of-band review. The closest retroactive surface is `git blame` + a `planar-reviewer` dispatch against a still-active task.

**Phase 3.5 still fires across all three barrel modes** when uncovered slugs intersect the cycle. Barrel-bypass bypasses the reviewer, not the coverage gate. The test-coder's `failure-surfaced` outcome halts the cycle and escalates to the operator regardless of mode.

The barrel modes are a feature of the example orchestrator, not a Planar requirement; for the dispatch-shape concept see [`docs/concepts.md` §Dispatch shapes](concepts.md#dispatch-shapes).

---

## Recipe 3 — Propagate to GitHub Issues

Use this after tasks are done (or any time you want external counterparts created).

**What happens:** The `planar-ext-sync` agent (or `planar-ext ext propagate` run directly) walks the feature tree top-down. It creates external counterparts for every entity not yet linked and records `external_links(link_role='mirror')` rows and `sync_events(outcome='ok')` rows.

### Register the external system (once per system)

```
planar-ext ext register github my-gh --project myorg/myrepo
```

### Propagate

```
planar-ext ext propagate 42 --system my-gh
```

On first propagation, the strategy is selected automatically:

- Jira → `jira-epic` (always)
- GitHub Issues → by the number of distinct repos the feature touches:
  none → `github-zero-repo`, one → `github-parent-issue`

A GitHub feature touching two or more repos is refused: the multi-repo
`github-projects-v2` strategy is permanently cut (decision 1001) and cannot
be executed. The selected
strategy is cached and reused on subsequent propagation runs. To force
re-detection:

```
planar-ext ext propagate 42 --system my-gh --restrategize
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
planar-ext ext propagate 42 --system my-gh --dry-run
```

Prints what would be created without contacting the remote.

### Verify existing counterparts

```
planar-ext ext propagate 42 --system my-gh --verify-counterparts
```

Probes the remote to confirm every already-linked entity still exists. Missing counterparts are written as `sync_events(outcome='counterpart-missing')` rows. Use `--unlink` to remove stale links or `--recreate` to recreate them immediately.

### Change an existing link's sync direction (destructive recovery only)

The current CLI has no lossless in-place direction update. `planar links
add/remove` manage internal relationships, not external bindings, and
`planar links update` is deferred. Top-level `unlink` deletes the old external
link and detaches its sync events (`link_id` becomes null); a manual `link`
replacement also loses the old `external_url`, `config_json` (including
propagation strategy), and last-sync state. The replacement cannot query or
adopt the detached event history.

Capture what the CLI can expose before deleting anything:

```sh
planar audit trail --link <link-id> --json > external-link-<link-id>-audit.json
planar-ext sync status --entity <kind:id> --system <system-slug> --json > external-link-<link-id>-status.json
planar-ext ext propagate <plan-id> --system <system-slug> --dry-run \
  --sync <read-only|write-back|two-way>
```

The first two commands preserve identity/history and last-sync evidence. They
do not expose the exact URL, config, role, or direction; stop if those values
are not independently known or their loss is unacceptable. For a
propagation-owned mirror, continue with:

```sh
planar unlink <link-id>
planar-ext ext propagate <plan-id> --system <system-slug> --dry-run \
  --sync <read-only|write-back|two-way>
planar-ext ext propagate <plan-id> --system <system-slug> \
  --sync <read-only|write-back|two-way>
```

The post-unlink dry run previews fresh creation. The final command creates a
new remote counterpart and new link state; it cannot restore the old URL,
configuration, or history. If retaining the same remote external id matters,
use top-level `planar link` instead of propagation, supplying the independently
known role and desired direction; that record-only replacement still starts
with null URL/config/last-sync and status `never`. See the
[`links update` reference](cli-reference.md#planar-links-update-link-id) for
the full caveat and command sequence.

---

## Recipe 4 — Handoff Between Agents or Sessions

Use this when an agent session is ending and a new session needs to resume the work. Also useful when switching vendors (e.g., Claude → Codex).

### Capture the handoff

At the end of the current session, follow the planar skill's `resume-handoff.md` reference. It validates that the current task is resumable, then records the handoff:

```
planar handoff 37 --note "implement the CSV writer; see tech-spec.md § Serialisation"
```

The handoff:
1. Requires that the current task has a `next_action` set.
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
planar resume 37
```

The resume packet contains:
- Current task state and body.
- The most recent `context_snapshots` row for this task.
- Open questions (unresolved questions blocking progress).
- Decisions made in prior sessions.
- The `next_action` field.

The new agent session reads it at startup, providing full context without relying on conversational history.

`planar resume validate 37` checks resume readiness without producing the packet — useful for CI or pre-handoff verification.

---

## Recipe 5 — Publish a Feature to an External System

Use this when you want to push the rendered workbench content for a
feature to an external operational system (Jira, GitHub Issues) so
non-Planar collaborators can read or comment on it.

**What happens:** `workbench publish` renders the workbench files
for a plan and pushes them to a registered external system via the
adapter layer. The destination system, credentials, and projection
template come from the system registration (see Recipe 3 for
registration and propagation).

### Publish the workbench tree

```
planar workbench publish 42 --system github
```

The `--system` flag names a registered external system slug (see
`planar-ext ext list`). For richer per-entity counterpart creation
(epics, issues, sub-issues with parent/child links) use `planar-ext ext
propagate <plan-id> --system <slug>` instead — `propagate` walks the
full plan tree and creates one external counterpart per entity, while
`workbench publish` pushes the rendered Markdown body.

### Commit local artifacts to a host repo

If the goal is "snapshot the feature spec into a git repo's `docs/`
directory", the supported path today is to copy the workbench files
manually:

```bash
cp -r ~/.planar/workbench/<assoc>/p42-<slug>/ ~/myrepo/docs/planning/billing-export/
cd ~/myrepo
git add docs/planning/billing-export/
git commit -m "snapshot billing-export spec for review"
```

The copy is static — re-run the `cp` to refresh it.

---

## Recipe 6 — Archive a Completed Feature

Use this after a feature ships and the on-disk workbench tree is no longer actively needed.

**What happens:** `workbench archive` removes the filesystem tree. The database retains every entity row, relationship, artifact, decision, and scenario. `workbench restore` recreates the tree byte-identically from the DB at any time.

### Mark the plan done

```
planar plan update 42 --status done
```

(`planar plan closeout 42` is the gated alternative: it checks that every task and descendant is terminal and no claim is live before flipping the status.)

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
archived: ~/.planar/workbench/project_my-app/p42-billing-export-csv (plan 42)
```

### Restore when needed

```
planar workbench restore 42
```

Expected output:
```
restored: ~/.planar/workbench/project_my-app/p42-billing-export-csv (plan 42)
```

The restored tree is byte-identical to the pre-archive state. All workbench sync state (`workbench_sync_state`) is restored, so `workbench status` and subsequent push/pull/sync operations work correctly.

---

Onboarding an existing repo was removed with `planar import` and `planar synthesize` (plan 1142, task 7471); the recipes are preserved at tag `archive/import-synthesize`.

---

## Recipe 8 — Reviewing Open Questions

Use this when a spec has a `## Open questions` section and you want to track those items as first-class question entities, resolve them, and confirm the spec stays in sync.

**What happens:** The `planar-planner` agent auto-registers each H3 item under `## Open questions` as a `questions` row when it seeds the workbench. The `planar-ingestor` agent (`planar spec ingest`) reconciles on re-run — it compares the live question entities against the current spec body and surfaces drift warnings for items that were added or removed from the spec without a corresponding entity update.

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

### Step 2 — Register questions automatically via planar-planner

When you dispatch the `planar-planner` agent with a goal statement, it drafts the spec files and calls `planar workbench extract-questions` internally. Any `## Open questions` H3 items found are registered as question entities via `planar question add`. Its report includes a line like:

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
planar question answer 5 --answer "ISO 8601 UTC, no timezone offset — confirmed with consumer team"
```

Mark a question as won't-fix (decided not to address):

```
planar question wontfix 7
```

Both commands flip the question's `status` out of `open`. An `answered` row also records `answer_body` and `answered_at`.

### Step 5 — Re-ingest after spec edits

If you edited the spec body after the initial draft — adding, removing, or rewriting H3 question items — run the ingest preview again to reconcile:

```
planar spec ingest 42
```

The ingest reads the current spec body, calls `planar workbench extract-questions 42`, and compares the live H3 items against the existing question entities. Drift surfaces as warnings in the preview output:

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
- Agents: `planar-planner` (auto-registers questions on draft), `planar-ingestor` (reconciles on re-ingest) — see [skill-reference.md](skill-reference.md).

---

## Recipe 9 — Working with multiple agents across repos

Use this when a planner dispatches coder agents to different repos in parallel — the polyrepo orchestrator case. The strict write-scope resolver was designed for it; this recipe walks through the call shape that keeps each coder writing to the correct association without explicit `--scope` plumbing everywhere.

**What happens:** Each coder runs in its own process with `cwd` set to the repo it owns. Step 2 of `ResolveForWrite` (cwd derivation, most-specific-wins) picks the right association from cwd alone. The orchestrator itself — whose cwd may not match any single target repo — passes `--scope` explicitly on cross-repo writes. A second layer, the [cross-scope guard](concepts.md#cross-scope-guard), then verifies the operator's resolved scope agrees with each *target entity's* stored scope before the write proceeds: a coder that drifts into the wrong cwd, or an orchestrator that passes a stale `--scope`, gets refused with exit 5 instead of silently writing under the wrong association.

### Coder pattern: rely on cwd derivation

When a coder is spawned for a task that touches `repo-a`, the dispatcher `cd`s the coder process into the repo before invoking write verbs:

```bash
cd ~/work/repo-a
planar task update 142 --next-action "rerun make test"
# … coder works, then returns; the orchestrator owns the terminal verb …
```

No `--scope` flag is needed. The resolver sees that `cwd` is inside the registered `project:repo-a` root, ranks `project` above the org association `project:repo-a` belongs to, and resolves to `project:repo-a`. The success line shows `[from cwd]`. Note what the guard does and does not cover here: `task update` is one of the seven verbs that run the cross-scope guard, so if the dispatcher accidentally `cd`-ed into `repo-b` for a task owned by `repo-a`, `task update` refuses with a multi-line error naming both scopes and pointing at `--scope` (or `cd` into the correct repo) as the remediation — there is no flag-based bypass. A verb outside the guarded set writes from any cwd. The guarded set is listed in [`docs/cli-reference.md § Guarded verbs`](cli-reference.md#guarded-verbs); everything else writes without comparing scopes.

This is the dominant pattern. Every coder, ingestor, and planner skill that runs against a known repo cwd works without scope plumbing.

For work that is intentionally stored on the repository itself rather than on its project association, pass the repo scope explicitly:

```bash
planar task add "Fix the repo-local build script" --scope repo:repo-a
planar task update 142 --next-action "rerun make test" --scope repo:repo-a
```

That keeps repo-owned work distinct from association-owned cross-repo coordination. The guard treats `repo:repo-a` and `assoc:project:repo-a` as different scopes.

### Orchestrator pattern: pass `--scope` explicitly

The orchestrator's own cwd is wherever it was invoked from — usually a workspace root or an unrelated directory. When it writes to entities in a target repo's association (e.g. updating a task in `project:repo-b` while dispatching), it must pass `--scope` explicitly:

```bash
planar task update 142 --next-action "wire up the CSV writer" \
  --scope assoc:project:repo-b
```

The success line shows `[from flag]`. The guard then verifies the explicit scope matches task 142's stored scope. This protects against the silent-wrong-scope class of bug: the explicit flag routes the write correctly regardless of the orchestrator's cwd, *and* the guard catches the case where the flag itself is wrong. New decisions logged from the orchestrator follow the same pattern:

```bash
planar decision add "Adopt strict resolver for writes" \
  --scope assoc:project:repo-b
```

`decision add` is a create verb, so the guard does not fire — the new decision's scope is whatever `--scope` resolves to. The guard fires on subsequent `decision accept` and `decision withdraw` calls; `decision edit` and `decision supersede` run no scope guard.

### Inspecting the resolved scope

`planar scope show` prints the cwd-derived scope set:

```
$ planar scope show
resolved scope (from cwd):
  project:repo-a
```

From a workspace root, it lists the org plus every member project. From outside any registered scope, it says `none` and tells you to `cd` or pass `--scope`.

### When the cwd does not match

If the orchestrator runs from a directory that is not inside any registered project, READS refuse (`error: cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global`, exit 1) while WRITES silently succeed at global scope, exit 0. That asymmetry is measured, not intended — see [Write resolution](concepts.md#write-resolution). For cross-repo planners, pass `--scope <slug>` explicitly — it makes intent legible in the command history. The active scope stack was removed in plan 153 M5, so there is no implicit fallback that could mask the cwd mismatch.

There is no flag that downgrades a cross-scope-guard refusal to a warning — `--no-scope-check` does not exist on the current binary (`planar schema` declares it on no command; passing it fails at parse time with exit 2, `error: <cmd>: The following argument was not expected: --no-scope-check`). The only remedies are `--scope <slug>` or running from the entity's owning repo. See [`./cli-reference.md#cross-scope-guard`](cli-reference.md#cross-scope-guard) for the full guarded/unguarded matrix and the exact refusal message format.

---

## Recipe 11 — Enriching a workspace routing table with LLM summaries

`planar workspace routing build` is deterministic by default: it scans
manifest files, READMEs, and dependency declarations, and writes a
`routing-table.json` containing static signals. Plan 135 M4 adds an
opt-in enrichment pass that merges LLM-derived summaries, capability
tags, and dependency hints into the same table — without making the
Planar binary depend on any LLM SDK.

### The contract

The routing-table builder never calls a model. It looks up structured
results in a content-addressed cache:

```
~/.planar/cache/workspace-enrichment/<org_id>/<slug>-<sha256>.json
```

The `<sha256>` is computed from the project's README excerpt (first
64 KiB) plus a sorted depth-2 directory listing. If the project's
content has not changed since the last run, the fingerprint is
identical and the cache hits — no tokens are spent.

### Populating the cache: an agent session

The builder only reads the cache, so something has to write the result files. An agent session with model access can: it fingerprints every project, calls the model at temperature 0, writes each result into the cache in the shape the builder validates, and then runs the builder. The planar skill's `local.md` reference covers the workspace operations (`init`, `routing build`, `routing show`, `regenerate`, `doctor`) and names `--enrich`:

```
planar workspace routing build --enrich
```

The producing session is responsible for the temperature-0 + seed discipline. The binary side validates only that each result carries a non-empty `provenance` string and that the result's `fingerprint_hash` matches the cache filename; a result whose `provenance` contains a non-zero temperature hint (e.g. `temperature=0.7`) produces a warning but is still applied.

### Populating the cache: `enrich_command`

Operators who do not want to run an agent session for it can wire a shell bridge in the workspace config:

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
treat the project as a cache miss and emit a hint to re-run the producer:

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

**Scope discipline in a workspace.** The [cross-scope guard](concepts.md#cross-scope-guard) fires on seven verbs, not only on `spec ingest` (the original lectio incident). The comparison is membership-aware: when you are working from the workspace root (`~/work/`), cwd resolves to `org:work`, which covers entities scoped to any member repo, so a `planar task update 142` against a task that belongs to repo A is accepted. The reverse direction is refused with exit 5: from inside `~/work/repo-a`, a write against an entity stored at `org:work` (or at an unrelated repo) needs `--scope` naming the entity's scope, or a `cd` into it. The same rule applies to `spec ingest --apply`, `decision accept`, `decision withdraw`, and single-target `planar-ext sync push`/`sync pull`/`sync resolve`. Every other mutating verb — including `plan update`, `decision edit`, `decision supersede`, `artifact update`, `ext create --from`, `ext propagate`, and `link`/`unlink` — writes without comparing scopes (see [`docs/cli-reference.md § Guarded verbs`](cli-reference.md#guarded-verbs)). Create verbs (`task add`, `decision add`, etc.) and entity-link verbs (`task link`, `links add`, `task touches add`) are not guarded — they are designed to cross scopes.

### Step 1 — Initialize the workspace

From the workspace root (`~/work/` with three `.git`-bearing children):

```
cd ~/work
planar workspace init
```

The verb refuses if `~/work/.git` exists (use `planar init` for a single repo) or if no child has its own `.git` (a workspace must contain repos). On success:

```
created org:work (Work)
  ├─ project:repo-a   [/home/user/work/repo-a]   (auto-created, member-of org:work)
  ├─ project:repo-b   [/home/user/work/repo-b]   (auto-created, member-of org:work)
  └─ project:repo-c   [/home/user/work/repo-c]   (auto-created, member-of org:work)

3 repos initialized as projects, all members of org:work.
Routing table refreshed (3 projects, 1 cross-repo deps).
AGENTS.md regenerated (1842 bytes).
Symlinks installed: AGENTS.md, CLAUDE.md (strategy: symlink).
```

Inspect what landed:

```
ls -la ~/work/AGENTS.md ~/work/CLAUDE.md
# both → /home/user/.planar/workspaces/1/AGENTS.md

ls ~/.planar/workspaces/1/
# AGENTS.md  routing-table.json
```

### Meta repo variant

Use this when the workspace container is itself a git repository and it contains nested repos or submodules:

```
cd ~/work/meta
planar workspace init --meta-repo --scan 2
```

Meta mode registers both the root repo and nested repos as projects under the workspace org. The org scope is for cross-repo coordination:

```
planar task add --scope assoc:meta "Coordinate root and submodule release"
```

Repository-owned work uses repo scopes:

```
planar task add --scope repo:meta "Update root build scripts"
planar task add --scope repo:submodule-a "Update submodule API"
```

Below the exact meta root, cwd-derived writes use the longest matching repo path, so a command from `~/work/meta/modules/submodule-a` lands in `repo:submodule-a`, while a command from `~/work/meta/src` lands in `repo:meta`. The exact meta root is ambiguous and refuses writes without `--scope`, showing both remediation choices.

Unlike the sibling workspace shape, meta mode does not install root `AGENTS.md` / `CLAUDE.md` links. Existing files in the root repo remain authoritative, and missing files stay missing. The canonical generated workspace content still lives under `~/.planar/workspaces/<org_id>/` and is used by `workspace routing show`, `workspace regenerate`, and `workspace doctor`.

Re-running the same command from the same meta root is idempotent. Reusing the same org slug from a different root is refused so an existing workspace cannot be silently repointed; choose a new `--slug` or run from the recorded root.

### Step 2 — Refresh as work progresses

When new plans land, new tasks are filed, repos are added to the org, or READMEs change, refresh the routing table and the AGENTS.md surface:

```
planar workspace routing build
planar workspace regenerate
```

`routing build` is deterministic and cheap (in-process, no LLM); run it whenever a shape change should be reflected in the table. `regenerate` re-renders `AGENTS.md` from the latest routing table plus live DB queries (open task counts, open question lists). The symlinks at the workspace root point at the canonical target and never need rewriting.

If the post-init pipeline fails after registration, the org, projects, and memberships remain committed. Fix the reported filesystem or template problem, then run:

```
planar workspace doctor
planar workspace routing build
planar workspace regenerate
```

For meta workspaces, this recovery path still leaves root `AGENTS.md` / `CLAUDE.md` untouched.

### Step 3 — Optional LLM enrichment

Static signals (README first paragraph, manifest-derived capability tags) produce a useful table on the first scan. For richer per-project summaries and capability inference, populate the enrichment cache (an agent session or the `enrich_command` bridge, see Recipe 11) and merge it into the table:

```
planar workspace routing build --enrich
```

The producer fingerprints every project (sha256 of README excerpt + sorted depth-2 dir listing), invokes the LLM at `temperature=0`, and writes validated results into `~/.planar/cache/workspace-enrichment/<org_id>/`; the builder then merges cached results into the table. Cache invalidation is implicit: a README edit or a new file at depth ≤ 2 changes the fingerprint so the next `--enrich` run treats the project as a cache miss. See [Recipe 11](#recipe-11--enriching-a-workspace-routing-table-with-llm-summaries) for the full enrichment contract.

### Step 4 — Inspect the workspace

```
planar workspace doctor
# org:work ok

planar workspace routing show
# Pretty-prints per-project capabilities, summary, dependencies, open-task counts.

planar workspace routing show --json | jq '.projects[] | {slug, capabilities}'
# Machine-readable dump for scripting.
```

From a workspace root, read queries such as `planar plan list`, `planar task list`, `planar question list`, `planar scenario list`, `planar decision list`, `planar artifact list`, `planar search`, and `planar tree` default to the workspace org plus every member repo. From inside a member repo, they default to the most specific registered repo; pass `--scope assoc:<workspace>` when you explicitly want cross-repo workspace work.

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

# (Optional) drop each member repo's membership in the org
planar assoc remove org:work ~/work/repo-a
```

The `associations` row, the `projects` rows, and their membership links remain in the database after the filesystem cleanup. `assoc remove <slug> <repo-path>` removes one repo's membership at a time; there is no verb that deletes the org association row itself, and the projects themselves stay (they may belong to other orgs or be referenced directly). Subsequent `planar workspace doctor` runs treat the now-absent state dir as a deleted workspace and emit no warnings for an org that is no longer registered.

### Cross-references

- Concept: [docs/concepts.md § Workspace](concepts.md#workspace).
- Architecture: [docs/architecture.md § Workspace State Directory Model](architecture.md#workspace-state-directory-model).
- CLI verbs: [docs/cli-reference.md § Domain: `workspace`](cli-reference.md#domain-workspace).
- Skill reference: the planar skill's [`references/local.md`](../skills/planar/references/local.md) (workspace operations).

---

## Common Patterns

### Check what is in scope

```
planar scope show
planar task list
planar plan list
```

### Switch repo or association context

```
cd ~/work/my-app           # cwd derivation picks the registered project association
planar task list           # reads scoped from cwd
# or, from anywhere:
planar task list --scope repo:my-app
planar task list --scope assoc:project:my-app
```

### Review the audit trail for an external ticket

```
planar audit trail --link <link-id>
```

Shows every local session, decision, commit annotation, and sync event tied to the external link.

### Check system health before a session

```
planar health
```

Reports schema version, open sessions, unresolved sync conflicts, and handoff readiness. Fix any reported issues before starting work; the planar skill's `recovery.md` reference maps each degraded contributor to its route.

### Sync local changes to the remote

```
planar-ext sync push --all --system my-jira
```

Pushes local mutations (status changes, field updates) to the external system for all entities that have an `external_links(link_role='mirror')` row.

```
planar-ext sync pull --all --system my-jira
```

Pulls remote changes into `sync_events` rows. Conflicts (both sides changed) are surfaced as `sync_events(outcome='conflict')` and require explicit resolution via `planar-ext sync resolve`.

Inspect a conflict through `planar audit trail --link <link-id> --json`. The
conflict event's `evidence` object carries exact local/remote values,
provenance, observation time, a token, and the local entity's `updated_at`.
After approving one whole-entity disposition, pass both compare guards:

```text
planar-ext sync resolve <event-id> --keep local \
  --evidence-token <approved-token> \
  --expected-local-updated-at <reviewed-updated-at> --json
```

The command refuses a non-latest event, a closed link, a changed token or local
version, an absent/empty approved or fresh provider version, and a fresh remote
read that differs from the approved evidence. Missing provider version evidence
forces `defer`. For a manual merge, edit the local entity first, review its
post-state, then use that post-edit `updated_at` only after a second explicit
keep-local confirmation. The local compare-and-swap and fresh remote read narrow
the race window but cannot eliminate the provider GET-to-write race when the
provider lacks conditional updates. If the command or adapter fails ambiguously,
inspect the link audit, sync status, and entity post-state before retrying or
seeking fresh approval.

### Operational-plane sync, end to end

`ext` and `sync` verbs run on **`planar-ext`**, the operational-plane binary (decisions 995–1001): it opens SQLite directly with read-only access to planning tables and read-write access to exactly `external_links` / `external_systems` / `sync_events`, and owns both adapters, Jira and GitHub Issues together. `link` / `unlink` and `audit` stay on `planar` (the operator binary still records and reads external links directly). Sync is on-demand only; background daemons are explicitly out of scope.

```
# Register a system. Credentials come from env vars or the gh CLI.
planar-ext ext register jira my-jira \
    --base-url https://acme.atlassian.net \
    --project PROJ \
    --auth-env JIRA_USER,JIRA_TOKEN          # email,api-token pair

planar-ext ext register github my-gh \
    --project rdrsss/planar                   # omit --auth-env to shell out to `gh auth token`

# Link a task to a remote ticket.
planar link task:1 --to my-jira:PROJ-1234 --sync two-way

# Sync on demand. planar-ext no longer writes remote values into planning
# entities (decision 996): it fetches and emits remote_title/remote_status;
# an agent verifies and calls `planar` to make the actual planning write.
planar-ext sync pull 1
planar-ext sync push 1

# Conflicts surface explicitly; never resolved silently.
planar-ext sync status                            # last_sync_status per link
planar-ext sync resolve 7 --keep local \
    --evidence-token <token> \
    --expected-local-updated-at <timestamp>       # resolves sync_event id 7

# Audit trail from either direction.
planar audit trail --link 1                   # everything for link 1
planar audit handoff-readiness --threshold 90 # CI gate
```

Auth methods supported today: `token-env` (a single variable is a bearer token; a `USER,TOKEN` pair is HTTP Basic) and `gh-cli` (shells out to `gh auth token`). OAuth-stored and OS-keychain credentials are deferred.

For whole-feature propagation (create all operational counterparts for a plan tree at once), use `planar-ext ext propagate <plan>`; [Recipe 3](#recipe-3--propagate-to-github-issues) walks through it. Use `--restrategize` to change the strategy of an already-propagated plan. The multi-repo GitHub `projects-v2` strategy is permanently cut (decision 1001): it is recognized only to be refused, with a pointer at the alternative.

---

## Recipe 14 — Author a personal skill in the sandbox

Use the local sandbox at `~/.planar/local/` to author personal, machine-local skills and agents that are copied into every present vendor's skill and agent directories, without the overhead of repo contribution. The projection rules, name rule, states and ownership are in [skill-reference.md § Operator-local skills and agents](skill-reference.md#operator-local-skills-and-agents); the planar skill's `local.md` reference is the procedure an agent follows for this recipe.

### Step 1 — Author the source file

Skills live as directory sources (`<name>/SKILL.md`); agents stay flat:

```
mkdir -p ~/.planar/local/skills/fixup-protos
cat > ~/.planar/local/skills/fixup-protos/SKILL.md <<'EOF'
---
description: "Rebuild and re-import protobuf bindings in the current repo"
---

# Fixup Protos

Steps:

1. Run `make protos`.
2. ...
EOF
```

For agents drop a flat file at `~/.planar/local/agents/<name>.md`. A name must match `^[a-z0-9]+(-[a-z0-9]+)*$` and be at most 51 characters; `My_Skill` is refused at exit 2, not renamed. Auxiliary files alongside `SKILL.md` (helper scripts, data files, icons) travel with the skill.

**Importing from an external collection.** If the operator already maintains a personal skill folder elsewhere (a separate git repo, a Dropbox directory), the entire collection can be imported in one shot:

```
planar local import ~/my-skills/                         # collection of flat and/or dir-shape skills
planar local import ~/my-skills/fixup-protos.md          # a single flat file → wrapped into <name>/SKILL.md
planar local import ~/my-skills/jira-bulk-edit/          # a single dir-shape skill (containing SKILL.md)
planar local import ~/my-agents/ --kind agent            # agents go flat to the agents/ dir
```

`planar local import` validates each input's frontmatter, materializes it into the sandbox in the correct shape, and (by default) runs the link step in the same call. Name collisions with existing sandbox entries are skipped unless `--force` is passed. Pass `--no-link` to import without linking, or `--dry-run` to preview.

**Migrating from the legacy flat sandbox.** A pre-reshape sandbox stored each skill as a flat `~/.planar/local/skills/<name>.md`. Run `planar local migrate` once to convert these to the `<name>/SKILL.md` layout. The verb is idempotent and refuses to overwrite operator content (collisions are skipped with a reason).

### Step 2 — Link the source into every present vendor

```
planar local link
```

Walks every source under `~/.planar/local/{skills,agents}/` and copies each into the vendor roots under the name `planar-local-<name>` (for the example, `planar-local-fixup-protos`), so a local install is visibly user-authored and cannot take a bundled name. A skill copy has its frontmatter `name` rewritten to match its directory; the source is never edited.

Useful flags:
- `--dry-run` previews without writing.
- `--vendor claude` (repeatable) restricts the operation to one or more vendors. Composes with the source's `vendors:` frontmatter list (intersection).
- `--reconcile` rewrites stale copies, writes missing ones, migrates legacy projections and drops records whose source is gone.

### Step 3 — Iterate

Edit the source `SKILL.md`, then run `planar local link` again: the vendor directories hold copies, so an edit reaches them only when you re-link.

```
vim ~/.planar/local/skills/fixup-protos/SKILL.md
planar local link
```

### Step 4 — Restrict to some vendors (optional)

Set `vendors:` in the source frontmatter to opt a skill out of vendors that do not apply (`claude` gates the Claude copy; `codex` and `copilot` gate the shared and Antigravity copies):

```yaml
---
description: "Claude-specific helper"
vendors: [claude]
---
```

A source that sets the retired `shadow` key is refused at exit 2; a local source cannot take a bundled name.

### Step 5 — Inspect installs

```
planar local list
```

Each recorded projection is reported with a state recomputed from disk: `live`, `stale` (the source or a copy was edited), `legacy` (an old projection waiting for `planar local migrate`), `missing`, or `broken` (the source is gone). A destination that exists, differs, and is not a prior Planar projection is refused at exit 6 before anything is written.

### Step 6 — Remove an install

```
planar local unlink fixup-protos
```

Removes every projection for `fixup-protos`. The source in `~/.planar/local/` is preserved; pass `--purge` to delete it too.

### Step 7 — Promote (manual)

There is no `planar local promote` shortcut, deliberately. Making a local skill or agent part of Planar means contributing it to the Planar repository through its normal review, where `make surface-lint` and `make cli-usage-check` gate it. After promotion, `planar local unlink fixup-protos --purge` retires the sandbox copy.

---

## Recipe 14A — Inspect installed skill and agent placements

`install.sh` stages `skills/planar/` and the agents under `~/.planar` and places
them into each vendor found on the host. Planar's own `install-manifest.json`
records every placement. Run these inspections from the repository root:

```bash
planar health
scripts/check-self-installed.sh
```

`planar health` compares each recorded placement with the staged copy (a
skill as a directory, every file) and reports `projection_freshness`:
`fresh`, `stale` (drifted) or `missing` rows, and a `legacy` manifest left by an
older installer. `scripts/check-self-installed.sh` exits non-zero unless the
manifest is current with no stale or missing row. To repair a managed
placement, run `./install.sh --prefix ~/.planar`. The nine targets and the
drift rules are in [skill-reference.md § What installs where](skill-reference.md#what-installs-where).
Personal `planar local` extensions remain outside this install manifest.

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

Same shape across kinds: `task edit`, `question edit`, `decision edit`, `scenario edit`. `plan` has `view` only.

### Step 2 — View or diff without editing

```
planar artifact view 42      # exec $PAGER on the workbench file
planar artifact diff 42      # unified diff between DB and FS
```

`view` is the read path; `diff` shows where the FS file has diverged from what the DB would render. Both auto-push if the file is missing. `diff` exists on `task` and `artifact` only.

### Step 3 — Resolve a concurrent-change conflict

`task edit` (and the other `edit` verbs) auto-pull the latest DB
state into the workbench before opening `$EDITOR`. If you already
have an in-progress edit on disk that you don't want clobbered, pass
`--no-pull`:

```
planar task edit 47 --no-pull   # skip the pre-edit pull; trust the on-disk file
```

If a concurrent `task update` or `workbench pull` lands between
editor-open and save, the resulting drift surfaces through the
normal workbench sync flow — inspect via `planar workbench status`
and settle the conflict with `planar workbench resolve <event-id>
--prefer fs|db`. For an overwrite that bypasses status guards on the
update path itself (rather than the editor flow), `planar task
update <id> --force --reason "<why>"` is the escape hatch.

### Step 4 — Report a verdict on a pending diff

```
planar task review 226 --approve
```

`task review` computes the same workbench diff as `task diff` and reports an `--approve` or `--request-changes` verdict on it. The verdict is printed (or emitted with `--json`); it is not persisted. `artifact`, `decision` and `scenario` have the same `review` verb; `plan` and `question` do not.

### Step 5 — Bulk-edit the whole feature

```
planar workbench edit 226
```

Push the plan's entities to the workbench, exec `$EDITOR` on the feature directory, pull on save. Use this when you want to edit several files in one session and let the pull machinery reconcile them.

### When to reach for which verb

- One file, one entity: `<entity> edit`.
- Read-only inspection: `<entity> view`.
- "Is the FS file in sync with the DB?": `task diff` or `artifact diff`.
- Report a verdict on that diff: `<entity> review` (`task`, `artifact`, `decision`, `scenario`).
- "I want to edit 4 files at once on one plan": `workbench edit`.
- Old `push → edit → pull` rhythm: still works; it's just no longer the default.

## Recipe 16 — Observing agent activity from the operator binary

You are an operator watching a feature in flight. One or more agents (planner, coder, reviewer, test-coder, orchestrator, ext-sync) are running against the plan you own, each holding a claim on a task and emitting actions. This recipe walks the **read** verbs on the `planar` binary that give you a complete operator-side view of what's happening, without ever calling the write side.

### What lives where

By design, the `planar` binary has **no `planar agent` subcommand**. Agent observability is split across three surfaces and they earn their separate places:

| Binary | Role | Verbs |
|--------|------|-------|
| `planar` (this binary) | Operator reads of agent state, folded into existing verbs. | `dashboard --agents`, `plan next`, `tree`, `audit trail`, `health` |
| `planar-agent` | Agent ritual + operator-recovery writes. Owns every write to `agent_work_claims` / `agent_actions`. | `pull`, `peek`, `claim`, `heartbeat`, `complete`, `fail`, `release`, `block`, `action start`/`end`, `review`, `ingest`, `reconcile`, `abort` |
| `planar-watch` | Live streaming viewer. Pure read. | `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree`, `sync-events`, `queue`, `queue history` |

This recipe covers the **planar** binary's read surface. The claim ritual for `planar-agent` is in `skills/planar/references/claim-ritual.md`; the streaming-viewer recipe for `planar-watch` is [Recipe 19](#recipe-19--live-agent-cockpit-with-planar-watch).

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
planar plan show 85                    # the plan's own row
planar plan step list 85               # its steps
planar plan descendants 85             # child plans and their tasks
planar task list --plan 85             # tasks linked to the plan
planar artifact list --plan 85         # tech specs, ADRs, etc.
planar decision list --plan 85         # accepted/proposed decisions
planar question list --plan 85         # open questions
```

`plan show` returns the plan's own row; `plan step list` and `plan descendants` return its `plan_steps` and its child plans and tasks — the structural counterpart to `plan next`. The per-kind `list --plan <id>` filters drill into entities linked to the plan via `entity_links` (relationship `derives-from`). `planar tree` (no `--scope` flag) renders the cwd-derived read set; at a workspace root that means the workspace org plus member repos, while inside a member repo it means that repo only. Pass `--scope <association-slug>` to root at a different scope. `tree` is scope-rooted, not plan-rooted — use `plan show` + the filtered list verbs when the question is "what does this one feature look like".

### Step 4 — Inspect the audit trail for one entity

```
planar audit trail --kind task 541
```

The `audit_trail` read surface stitches together `audit_log` (the operator-write log) with `entity_links` and now joins against `agent_actions` for any actions taken on the entity. You see each role's `started_at` / `ended_at` / `outcome` plus the operator-side decisions that ran around them.

### Step 5 — Confirm the database is healthy

```
planar health
```

`health` is the always-on smoke check: schema-current, integrity, in-flight
tasks resumable, pending handoffs fresh, and manifest-owned installed skill and
agent projections fresh. A stale/missing managed projection or a legacy,
invalid, or unsupported manifest degrades health and prints an exact repair or
reinstall command; health itself remains read-only. Unmanaged extensions,
unselected vendors, and a machine with no recorded Planar installation do not
degrade. It is not agent-activity-aware (the claim table is consulted by
`dashboard --agents` and `plan next`, not by health), but it is the first verb
to run when something looks wrong before you spend time chasing the wrong
layer.

### Putting it together

The full operator-side observation loop is exactly these five verbs. None of them write; they read across `plans`, `tasks`, `agent_work_claims`, `agent_actions`, `audit_log`, and `entity_links` to produce the operator view:

```
planar dashboard --agents              # who is doing what, right now
planar plan next <plan> --include-claimed --include-stale
                                        # one plan's queue, every bucket
planar plan show <plan>                # the plan's own row
planar task list --plan <plan>         # tasks under the plan
planar audit trail --kind <kind> <id> # one entity's history
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

- `planar init` has been run against the database the agent should write to. The agent and operator binaries must agree on `$PLANAR_DB` (default `~/.planar/planar.db`).
- The Planar binaries are on `$PATH`; `which planar-agent` should print a real path.

### Step 1 — Configure the Claude Code hook

Edit `~/.claude/settings.json` (the per-user Claude Code settings) and add a hook entry for each event type you want recorded. The minimal viable subset is the five event types the M4 adapter knows:

```json
{
  "hooks": {
    "session_start": [
      {
        "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/planar.db" }
      }
    ],
    "session_end": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/planar.db" } }
    ],
    "user_message": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/planar.db" } }
    ],
    "assistant_message": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/planar.db" } }
    ],
    "tool_call": [
      { "command": "planar-agent ingest --vendor claude --event @-",
        "env": { "PLANAR_DB": "/Users/you/.planar/planar.db" } }
    ]
  }
}
```

Notes on the invocation:

- `--vendor claude` selects the Claude adapter. `--vendor copilot` selects the GitHub Copilot adapter wired in M6 (see Recipe 18); `--vendor codex` parses but exits non-zero — the codex slot is reserved for a future adapter.
- `--event @-` reads the hook payload from stdin; Claude Code pipes the JSON envelope to the configured command. Use `--event @<path>` if your hook runner stages payloads on disk instead.
- `PLANAR_DB` MUST point at the same database file `planar` and `planar-watch` open. The hook subprocess does not inherit your shell's `$PLANAR_DB`, so set it explicitly in the hook's `env` block.
- Each invocation opens its own SQLite connection. WAL mode (set by the engine on connect) lets dozens of concurrent hooks coexist with `planar-watch feed --follow` readers.

### Step 2 — Smoke-test the wiring by hand

Before letting Claude Code drive it, confirm the binary round-trips a payload locally:

```sh
echo '{"event_type":"session_start","session_id":"smoke","model":"claude-opus-5-5"}' \
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
- **Unknown `event_type`** (the payload is valid JSON but the event_type field is outside the adapter's known set): `ingest` exits non-zero with `error: unknown event_type in payload…`. Add the new event type to the claude branch of `parse()` in `src/cmd/planar-agent/handlers/ingest/ingest.cpp` if you want it recorded.
- **`$PLANAR_DB` missing or unreadable**: the schema-version handshake fails at startup; `ingest` exits non-zero before parsing.

Both error paths are atomic — the surrounding `BEGIN IMMEDIATE` transaction rolls back so no partial rows land in `sessions` or `agent_actions`.

## Recipe 18 — Wiring GitHub Copilot hooks to `planar-agent ingest`

The M6 second-vendor adapter ships Copilot support behind `--vendor copilot`. GitHub Copilot — the Coding Agent and the Copilot Chat session surface — emits a namespaced event taxonomy (`session.started`, `session.completed`, `turn.user`, `turn.assistant`, `tool.invocation`) carried on a JSON envelope. Pipe those events into `planar-agent ingest --vendor copilot --event @-` and Planar opens (or reuses) a `sessions` row and records each event as an `agent_actions` row exactly as it does for Claude. The same operator surfaces from Recipe 16 (`dashboard --agents`, `tree`, `audit trail`) then show Copilot activity interleaved with Claude activity.

### Prerequisites

- `planar init` has been run against the database Copilot should write to. Copilot's hook subprocess and the operator binaries must agree on `$PLANAR_DB` (default `~/.planar/planar.db`).
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
            PLANAR_DB: "/Users/you/.planar/planar.db"
      session.completed:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/planar.db"
      turn.user:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/planar.db"
      turn.assistant:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/planar.db"
      tool.invocation:
        - command: "planar-agent ingest --vendor copilot --event @-"
          env:
            PLANAR_DB: "/Users/you/.planar/planar.db"
  ```

- **Self-hosted Copilot Chat runner**: register a webhook target that POSTs each event as JSON to a small forwarder, then `curl --data-binary @-` it into `planar-agent ingest --vendor copilot --event @-`. The forwarder is a one-liner; the payload shape is the same.

Notes on the invocation:

- `--vendor copilot` selects the Copilot adapter wired in M6. `--vendor claude` selects the Claude adapter (Recipe 17); `--vendor codex` is reserved and exits non-zero today.
- `--event @-` reads the hook payload from stdin. Use `--event @<path>` if your runner stages payloads on disk first.
- `PLANAR_DB` MUST point at the same database file `planar` and `planar-watch` open. Hook subprocesses do not inherit your shell's `$PLANAR_DB`; set it explicitly in the `env` block.
- Each invocation opens its own SQLite connection. WAL mode (set by the engine on connect) lets dozens of concurrent hooks coexist with `planar-watch feed --follow` readers.

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
- **Unknown event** (payload is valid JSON but `event` is not in the table above): `ingest` exits non-zero with `error: unknown event_type in payload (vendor=copilot)…`. Add the new event name to the copilot branch of `parse()` in `src/cmd/planar-agent/handlers/ingest/ingest.cpp` if you want it recorded.
- **`$PLANAR_DB` missing or unreadable**: the schema-version handshake fails at startup; `ingest` exits non-zero before parsing.

Both error paths are atomic — the surrounding `BEGIN IMMEDIATE` transaction rolls back so no partial rows land in `sessions` or `agent_actions`.

## Recipe 19 — Live agent cockpit with `planar-watch`

`planar-watch` is the **human-facing read-only viewer**. It opens the database in strict read-only mode (`SQLITE_OPEN_READONLY`); the SQLite driver itself refuses every write SQL string, which is the second line of defense behind the binary's "no write verbs registered" capability boundary. The first defense is the verb tree itself: it contains only read verbs — `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree`, `sync-events`, `queue` and `diagnose` — plus the conventional `version`, `completion` and `schema` helpers, and zero anything that mutates state.

This recipe walks the streaming-cockpit workflow. The companion recipe for the operator's read-fold-ins on the `planar` binary lives in Recipe 16.

### Step 1 — Stream the cross-cutting activity feed

```
planar-watch                                     # default: feed
planar-watch feed --follow                       # stream until SIGINT
planar-watch feed --follow --tail 50             # show last 50 events, then stream new ones
planar-watch feed --follow --json | jq -c .      # NDJSON, one event per line
planar-watch feed --vendor claude --plan 85      # narrow by vendor + plan
planar-watch feed --since 2026-05-27T00:00:00Z   # only newer events
```

`feed` is the lowest-friction view: every claim transition, action transition, and (where applicable) task status change ordered by occurrence time. With `--follow` the loop polls every `--interval` (default `1s`; e.g. `100ms` for tests). The JSON event shape is stable across the transport-tier ladder — see `docs/architecture.md` § "Live tail / follow implementation".

**M3 addition — `--tail N` (plan 467):** emits the most-recent N events on the initial render, then with `--follow` streams only events that arrived after the tail — no re-emit. Equivalent to `journalctl -f -n N`. N must be a positive integer; N ≤ 0 exits 2 with `error: feed: --tail must be a positive integer`. See [CLI reference: planar-watch feed](cli-reference.md#binary-planar-watch).

### Step 2 — Snapshot the active claims (`ps`)

```
planar-watch ps                                  # text table
planar-watch ps --json                           # generated_at + active + stale arrays
planar-watch ps --stale                          # also include lease-expired / stale
planar-watch ps --vendor codex --follow          # live filter
planar-watch ps --sort-by heartbeat --follow     # freshest-heartbeated first (M3 default)
planar-watch ps --sort-by lease                  # pre-M3 claimed_at ordering
planar-watch ps --group-by role                  # one section per agent role
planar-watch ps --group-by scope                 # one section per project scope
planar-watch ps --group-by vendor                # one section per vendor
```

`ps` is the operator's "what's running right now" snapshot. With `--stale` it also includes claims whose lease has expired (and that `planar-agent reconcile` would mark stale on its next sweep) — the diagnostic surface for wedged agents.

**M3 columns (plan 467):** each text row now carries `activity:"<summary>"` (most-recent action description, truncated at 80 bytes), `worktree:<basename>` (basename of the agent's worktree path), and `last_hb:<rel>` (relative heartbeat age, e.g. `15s`, `2m`). The `--sort-by` flag controls ordering; `--group-by` emits `[group: <key>]` section headers in text mode and a `groups: {key: [...]}` JSON envelope instead of the flat `active`/`stale` arrays. See [CLI reference: planar-watch ps](cli-reference.md#binary-planar-watch) for the full flag table.

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

### Step 6 — Operator observation loop (recommended two-pane layout)

For sustained observation of an active plan, open two terminal panes side by side.

**Pane A — live process list:**

```
planar-watch ps --follow
```

Refreshes every second (default interval). Columns at a glance: entity id, scope, `activity:"<summary>"` (what the agent is doing right now, sourced from its last `planar-agent heartbeat --status` call), `worktree:<basename>`, `last_hb:<rel>` (how long since the last heartbeat). The freshest-heartbeated agent surfaces at the top (`--sort-by heartbeat` is the M3 default).

To orient by role rather than by recency:

```
planar-watch ps --follow --group-by role
```

This emits `[group: coder]`, `[group: reviewer]`, etc. — a quick "how many agents of each type are alive and what are they doing".

**Pane B — event stream:**

```
planar-watch feed --follow --tail 50
```

Renders the 50 most-recent events on startup (so the pane is not blank at launch), then streams every new event without re-emitting the initial tail. Add `--plan <id>` or `--vendor <v>` to narrow when multiple plans run in parallel.

**Higher-level overview:**

```
planar dashboard --agents
```

The `planar` binary's fold-in view — rolls up active plans, claim counts, and per-plan next-available-work tallies in one snapshot. Each claim in the `--agents` JSON output carries a `latest_action` field (the same field `planar-watch ps --json` emits) for scripting dashboards. Suitable for a third monitoring pane or a status-bar widget. Does not stream; re-run on demand or wrap in `watch`.

### Step 7 — Topology view for orchestrator-heavy sessions (`tree`)

When a session runs multiple orchestrators, each dispatching several coders in parallel, `ps --group-by role` shows what each agent is doing but flattens the parent→child structure. Use `planar-watch tree` to see the dispatch fanout explicitly:

```
planar-watch tree                         # full forest snapshot
planar-watch tree --follow                # stream; re-renders on WAL change
planar-watch tree --root-session <id>     # scope to one session's subtree
```

Each root action (orchestrator dispatch, `parent_action_id IS NULL`) renders flush left; sub-agents dispatched from it render beneath with `├──` / `└──` tree characters. Example output for a single orchestrator with two coders:

```
action:12  scope:project:my-app  vendor:claude  activity:"dispatching M4 coders"  branch:feat/m4  last_hb:2s
└── scope:project:my-app  vendor:claude  activity:"writing tree.cpp"  branch:feat/m4  last_hb:5s
└── scope:project:my-app  vendor:claude  activity:"writing tests"     branch:feat/m4  last_hb:8s
```

**When to use `tree` vs `ps --group-by role`:**
- `ps --group-by role` — use when the question is "what role is each agent and how recently did it heartbeat?" (flat view, sorted by recency or role).
- `tree` — use when the question is "which orchestrator dispatched which coders?" (topology view). This is the M4-conditional view: reach for it when you are running three or more orchestrators and `--group-by` output is too dense to read at a glance.

Decision D5 (plan 467) gates the `tree` verb on an operator opting into the M4 conditional: if you have only one orchestrator and one or two coders, `ps --follow` is sufficient.

See [CLI reference: planar-watch tree](cli-reference.md#planar-watch-tree--m4-addition-plan-467) for flags and the tree-character reference.

### Capability boundary

The verb set is enforced by `root_app()` in `src/cmd/planar-watch/main.cppm`'s registered-command list: there is no `pull`, `claim`, `complete`, `fail`, `release`, `block`, `heartbeat`, `action`, `ingest`, `reconcile`, or `abort` in the tree, pinned by `src/cmd/planar-watch/capability.t.cpp`. The strict-read-only DB handle (`db::connection::open_read_only` → `sqlite3_open_v2(..., SQLITE_OPEN_READONLY, ...)` in `src/lib/db/db.cpp`) refuses any write SQL with `SQLITE_READONLY` at the driver layer — verified by the `a read-only connection refuses a write` unit test in `src/lib/db/db.t.cpp`. Both defenses must be in place; `src/cmd/planar-watch/context.t.cpp` treats either failing alone as a regression.

## Recipe 20 — Clean up terminal-status workbench files from historical or manual recovery

Current `planar spec ingest --apply` runs atomically per anchor plan: a SlugConflict or
other apply failure rolls back that anchor's derived rows, successful action audit, and
anchor status flip together. A failed apply should not create child plan, task, decision,
or scenario files that need manual cleanup.

Use this recipe only for terminal-status files already on disk, such as leftovers from
older Planar versions, manually cancelled or abandoned entities, or recovery from a
separate workflow that intentionally preserved partial work. It removes workbench files
for entities that are already terminal in the database.

```sh
# 1. Find out what's stranded. status with terminal-filter on tells you what
#    the workbench would write under the new defaults; the diff against what's
#    actually on disk is the cleanup target.
planar workbench push <plan>
#   → workbench push: plan N (slug) - X applied, 0 pending, 0 filtered (mode=failures), 0 conflict(s)
#     12 pre-existing terminal file(s) on disk — run 'planar workbench gc <plan>' to remove

# 2. Run gc to preview.
planar workbench gc <plan> --dry-run
#   → workbench gc (--dry-run): would remove 12, keep 47, drifted-skipped 0, errors 0 (mode=failures)

# 3. If anything in the workbench has hand-edited content you care about,
#    pull it FIRST so the body lands in the DB before deletion.
planar workbench pull <plan>

# 4. Apply.
planar workbench gc <plan>
#   → workbench gc: removed 12, kept 47, drifted-skipped 0, errors 0 (mode=failures)
```

If `gc` reports `drifted-skipped > 0`, the listed paths have FS content that differs from
the DB-stored hash. Either run `workbench pull <plan>` first (to land those edits as the
authoritative body) or re-run `gc --yes` to discard them. The verb deliberately refuses
silent destruction.

For a fresh-machine cleanup pass (e.g. you just cloned a repo whose `.planar/workbench/`
came from a teammate), use `planar workbench gc --all-scopes` to walk every plan's tree
in one shot. The same drift-refusal applies per-plan.

**See also:** `docs/concepts.md § Workbench § Terminal-status filter` for the underlying
model; `planar workbench push --apply-cleanup` for the in-the-moment narrow-scope variant
that runs cleanup as part of the next push.

---

## Recipe 21 — Pick an orchestration strategy for a plan

Use this when you dispatch `planar-orchestrator` on an `active` plan and want to understand the strategy gate (Phase 3's first sub-step). The strategy answers "what is the overall methodology for this plan?" — the dispatch-shape gate (nested under it) answers "within that strategy, how do I batch this cycle's work?" For the concept overview see [`docs/concepts.md §Orchestration strategy`](concepts.md#orchestration-strategy); the strategies are the example orchestrator's choices, and Planar records only the metadata a dispatcher attaches.

### Step 1 — Invoke the orchestrator

Dispatch `planar-orchestrator` with plan 297.

The orchestrator reads the plan's task graph, computes the parallel-eligible subset, and surfaces its recommendation:

```
Plan 297 "worktree-management" — 4 todo tasks, 3 parallel-eligible

Recommended strategy: parallel-fanout
Rationale: ≥3 tasks and ≥2 parallel-eligible (rule 3 of the recommendation algorithm)

Alternatives:
  classic                Sequential cycles, reviewer per cycle. Isolation: pwd or worktree.
                         [continuity guarantee — today's behavior bit-for-bit]
  parallel-fanout        Fan out to N coders on the parallel-eligible subset, staged by the
                         `blocks` graph; reviewer at fan-in.
                         [throughput + integrated review]                                 ← recommended
  barrel-deferred        Back-to-back coder cycles; reviewer at boundary on union diff.
                         Isolation: pwd or worktree.
                         [throughput + late review safety net]
  barrel-bypass          No reviewer; gates are the entire signal. Isolation: pwd or worktree.
                         [maximum throughput; trust the gates]
  isolated-sequential    Alias for classic + worktree.
                         [pwd hygiene + per-task rollback]

  --strategy custom      Per-axis flags for advanced operators.

Confirm strategy and isolation? [parallel-fanout / classic pwd / classic worktree /
  barrel-deferred pwd / barrel-deferred worktree / barrel-bypass pwd /
  barrel-bypass worktree / custom]
```

This is a hard operator gate identical in strength to the Phase 2 ingestion gate. Auto-defaulting without confirmation is **not** supported — the recommendation never silently becomes an action.

### Step 2 — Confirm or override

Type the name of the strategy you want. The orchestrator records the choice in the dispatch row's `agent_actions.metadata` (when that wiring lands — task 2939) and proceeds to the dispatch-shape gate constrained by your chosen strategy.

### Skip the gate via flag

Operators who already know which strategy fits — typically because they always want the same one for a given plan shape — can pre-commit it in the dispatch brief. Each line is one brief for plan 297:

- `--strategy parallel-fanout`
- `--strategy isolated-sequential`
- `--strategy classic` (explicit continuity)
- `--strategy classic --isolation worktree`
- `--strategy barrel-deferred`
- `--strategy barrel-deferred --isolation worktree`
- `--strategy barrel-bypass`

The strategy gate is skipped; for sequential strategies isolation defaults to `pwd` unless `--isolation worktree` is supplied. The dispatch-shape gate still runs (unless it also has a pre-committed answer via `--strict` / `--grouped` / `--batch`, or is forced by the strategy — `parallel-fanout` forces `fan-out`, the barrel strategies force their matching shape).

### Custom axis escape hatch

For shapes outside the named menu, name `--strategy custom` in the brief together with the per-axis options: `--isolation worktree`, `--branch-model epic-child`, `--concurrency sequential`, `--reviewer-cadence at-boundary` and `--test-coder-cadence at-boundary`. The example orchestrator refuses invalid axis combinations (e.g. `concurrency=fan-out` with `isolation=in-pwd` — parallel coders would clobber pwd) with a diagnostic before any dispatch runs.

### Worked example — a 4-task plan accepts `parallel-fanout`

You have plan 297 with four open tasks (`m1-foundation`, `m2-handlers`, `m3-tests`, `m4-docs`). Tasks 1 and 4 touch disjoint paths; tasks 2 and 3 each touch `src/cmd/planar/handlers/` but not the same file. None touch migrations or singleton files. Run:

```
# dispatch planar-orchestrator with plan 297
Plan 297 "feature-x" — 4 todo tasks, 3 parallel-eligible
Recommended strategy: parallel-fanout
Rationale: ≥3 tasks and ≥2 parallel-eligible (rule 3)
...
Confirm strategy? parallel-fanout
```

The orchestrator records the choice and moves on to the parallel-fanout per-cycle eligibility report — see [Recipe 22](#recipe-22--orchestrate-a-multi-task-plan-with-parallel-coders) for the full lifecycle.

---

## Recipe 22 — Orchestrate a multi-task plan with parallel coders

> **Ownership note.** The worktree lifecycle — epic-branch cut, per-lane worktree creation, fan-in merge, failed-lane retention, and full teardown — is run by the orchestrator through the host's subagent dispatch surface, a host-native workflow, or a background agent. The six eligibility rules are the engine's (`recommend-strategy`, `src/engine/planning/strategy.cpp`), consumed by the fan-out phases and never re-derived. They apply to `parallel-fanout`, not to a single sequential worktree lane. For the staged-wave (dependency-respecting) variant — proto lands first, then identity + web in parallel — see [Step 11 — Staged waves](#step-11--staged-waves-contract-lane-first) below. In-flight worktree execution is watched through `planar-watch ps --plan <id>` (claims + each claim's `worktree_path`) — there is no separate wave/barrier view.

The full `parallel-fanout` lifecycle, from strategy confirmation through fan-in and reviewer to cleanup. Use this when you have a plan in `active` status with ≥3 tasks, at least 2 of which are parallel-eligible (disjoint `task_touches`, no migration, no singleton-file touch, no blocking open question or proposed-decision dependency).

### Prerequisites

- Plan 297 is `active` with 4 todo tasks: `m1-foundation`, `m2-handlers`, `m3-tests`, `m4-docs`.
- Tasks 2 and 3 have `task_touches` declared via `planar task touches add`; the paths do not overlap.
- No open question or proposed decision blocks any of the four tasks.

### Step 1 — Strategy gate

```
# dispatch planar-orchestrator with plan 297
Plan 297 "worktree-management" — 4 todo tasks, 3 parallel-eligible
Recommended strategy: parallel-fanout
...
Confirm strategy? parallel-fanout
```

See [Recipe 21](#recipe-21--pick-an-orchestration-strategy-for-a-plan) for the full strategy-gate output.

### Step 2 — Per-cycle eligibility report

The orchestrator runs the parallelizability rules against the open task set and surfaces the parallel-eligible subset plus the serialized remainder:

```
Cycle 1 of plan 297 — parallel-fanout eligibility

Parallel-eligible (3 tasks):
  task:2930  m1-foundation       disjoint touches; no migration; no singleton; no open Q
  task:2931  m2-handlers-a       disjoint touches; no migration; no singleton; no open Q
  task:2932  m3-tests            disjoint touches; no migration; no singleton; no open Q

Serialized (1 task):
  task:2933  m4-docs             excluded by rule 4: touches docs/architecture.md (singleton)

Estimated wall-clock saving: ~(3 - 1) * avg_cycle_min = 2 cycles worth
  (rough estimate, no telemetry)

Fan out 3 tasks now? [yes / no / edit subset]
```

A `parallel-fanout` plan does not silently fan out — every cycle's batch is confirmed by the operator. The strategy gate (Step 1) was per-plan; this is per-cycle.

### Step 3 — Confirm the batch

Type `yes`. The orchestrator now executes the fan-out.

### Step 4 — Epic + cycle worktree creation, claim acquisition

You'll see the orchestrator's `git` invocations (or their effects) in your terminal:

```bash
# Once per plan (skipped on subsequent cycles):
git -C /repo branch epic/worktree-management master
git -C /repo worktree add /repo/.worktrees/epic/worktree-management/ epic/worktree-management
# .git/info/exclude appended with '.worktrees/' (per-clone, no commit)

# Per parallel-eligible task:
git -C /repo worktree add -b cycle/worktree-management/m1-foundation \
    /repo/.worktrees/cycle/worktree-management/m1-foundation/ epic/worktree-management
git -C /repo worktree add -b cycle/worktree-management/m2-handlers-a \
    /repo/.worktrees/cycle/worktree-management/m2-handlers-a/ epic/worktree-management
git -C /repo worktree add -b cycle/worktree-management/m3-tests \
    /repo/.worktrees/cycle/worktree-management/m3-tests/ epic/worktree-management

# Per cycle worktree, atomic claim acquisition:
planar-agent pull 297 --role coder --worktree /repo/.worktrees/cycle/worktree-management/m1-foundation --json
planar-agent pull 297 --role coder --worktree /repo/.worktrees/cycle/worktree-management/m2-handlers-a --json
planar-agent pull 297 --role coder --worktree /repo/.worktrees/cycle/worktree-management/m3-tests --json
```

The main checkout (`/repo/`) stays on master — neither the epic branch nor any cycle branch is ever checked out there. See [`docs/concepts.md §Worktree`](concepts.md#worktree) for the topology invariant.

### Step 5 — Concurrent coder dispatch

The orchestrator issues N Agent tool calls **in a single message** (the canonical "multiple tool uses in one message → parallel execution" pattern). Each brief carries the task ID, the claim token, the absolute cycle-worktree path, the spec citations, the locked decisions, the gates, the report shape, and the `cd <cycle-worktree-path>` first-action directive. The three coders run concurrently.

### Step 6 — Wait for terminal returns + aggregated check

The orchestrator's turn does not resume until every parallel call has returned. Each coder calls one of `planar-agent complete | fail | release | block --claim <token>` (the atomic terminal verbs flip claim status and task status in one transaction). After all N return, the orchestrator inspects each claim's status:

- **All-complete** → proceed to fan-in.
- **Any-fail / any-stale / any-block** → halt before any fan-in merge; surface the partial state with claim tokens + worktree paths; escalate to the operator.

This is the all-or-nothing terminal contract — partial fan-ins are not silently merged.

### Step 7 — Sequential fan-in merge

The orchestrator cds into the **epic worktree** (never the main checkout, never any cycle worktree) and merges each cycle branch onto `epic/<plan-slug>` in claim-arrival order with `--no-ff` (preserves the per-cycle history shape in `git log --graph`):

```bash
cd /repo/.worktrees/epic/worktree-management/

# Optional: sync master in first if it advanced since the last sync.
git fetch origin master
git merge --no-ff origin/master -m "Sync epic/worktree-management with master @ <sha>"

git merge --no-ff cycle/worktree-management/m1-foundation \
    -m "Plan 297 M1 fan-in: foundation"
git merge --no-ff cycle/worktree-management/m2-handlers-a \
    -m "Plan 297 M2 fan-in: handlers-a"
git merge --no-ff cycle/worktree-management/m3-tests \
    -m "Plan 297 M3 fan-in: tests"
```

Per-child conflict handling is conflict-tolerant: a conflict on one child halts that child's merge but does NOT halt the others. The orchestrator runs `git merge --abort`, opens a question via `planar question add --plan 297 "<conflict summary>"`, leaves the cycle worktree on disk for operator resolution, and continues to the next child. The fan-in conflict protocol (touches mis-prediction / trivial / deep-semantic) lives in plan 492's tech spec.

### Step 8 — Integrated reviewer (single pass)

After fan-in, the orchestrator dispatches **one** reviewer against the epic's consolidated state — not one reviewer per child. The reviewer reads `git diff master..HEAD` from inside the epic worktree. The verdict applies to the whole fan-out cycle as a unit.

```
Reviewer dispatched against epic/worktree-management
  cd /repo/.worktrees/epic/worktree-management/
  git diff master..HEAD
  (3 children fanned in cleanly; 0 deferred)

Verdict: approve
```

On `approve` → cleanup proceeds. On `request-changes` → another coder round, as the dispatcher decides. On `open-question` or `abort` → escalates without cleanup.

### Step 9 — Cleanup (post-success only)

After reviewer approval, the orchestrator removes every cycle worktree and force-deletes every cycle branch (`-D` because the branches are merged into the epic but NOT into master, and `git branch -d` checks merged-into-HEAD which is master):

```bash
git -C /repo worktree remove /repo/.worktrees/cycle/worktree-management/m1-foundation/
git -C /repo branch -D cycle/worktree-management/m1-foundation
# … repeated per cycle branch
```

The **epic worktree and `epic/<plan-slug>` branch persist** through cleanup. They are removed only after the operator merges the epic into master.

### Step 10 — Operator merges epic → master

The orchestrator surfaces readiness and exits:

```
epic/worktree-management is 7 commits ahead of master, reviewer-approved, ready for PR.
```

The orchestrator does **not** run the epic→master merge itself. You drive it through your project's normal PR or fast-forward path:

```bash
# PR path (recommended for review-trail):
cd /repo
git push origin epic/worktree-management
gh pr create --base master --head epic/worktree-management

# After PR merge:
git -C /repo worktree remove /repo/.worktrees/epic/worktree-management/
git -C /repo branch -D epic/worktree-management
```

### Step 11 — Staged waves (contract lane first)

The steps above fan out a **single wave** of mutually-disjoint lanes. When the plan's lanes have `blocks` dependencies — the plan-753 shape, where a `proto` contract lane must land before `identity` and `web` build against its generated stubs — the orchestrator drives **staged waves** so downstream lanes never build against a moving API. Eligibility gating stays the engine's (`recommend-strategy`); the `blocks` graph only orders and labels the waves.

1. **Present the ordered wave plan at the gate.** On the plan-753 shape, `proto` is wave 1; `identity` + `web` are wave 2 (each `blocked_by: [proto]`); and a terminal **cross-lane integration pass** is the last wave. A task the engine serialized for a non-blocks reason (migration / singleton / empty-touches / open-question / proposed-decision) is never placed in a wave. Present the full ordered picture to the operator and wait for confirmation before creating any worktree.

2. **Drive one barrier at a time.** For the current wave, take the lanes `recommend-strategy` reports as eligible, create their worktrees, and fan out their coders (Steps 4–6 above).

3. **Enforce the wave barrier before the next wave.** The barrier is on fan-in completion, not coder completion: a lane whose coder reported done but whose branch has not been merged does not satisfy it. Create no wave-N+1 worktree until every wave-N lane has fanned in. Once it has, re-run `recommend-strategy` — the just-landed lanes have dropped out and their dependents are now eligible — and repeat for the next wave.

4. **Fan-in retention and resume.** A succeeded lane's worktree is torn down at fan-in; a failed lane's worktree is retained for inspection. On a mid-wave failure, a `failed_clean` lane needs no reconcile (it is available on the next recompute); an `abandoned` lane is reclaimed immediately with `planar-agent reconcile --stale-after 0`. Then recompute and re-fan only the remainder (never restart from wave 1). On plan completion, remove every lane worktree and branch; keep the epic branch until its PR merges.

For the per-step orchestrator behavior under `parallel-fanout` and sequential worktree isolation, see `agents/planar-orchestrator.md`.

---

## Recipe 23 — Recover a dead coder from its worktree

A coder dispatched into a worktree (under sequential worktree isolation or `parallel-fanout`) died mid-cycle — its heartbeat lapsed past TTL, its claim is now stale, and the cycle worktree on disk holds whatever partial state the coder committed before dying. This recipe recovers it; it applies to any worktree-isolated coder. The persisted `agent_work_claims.worktree_path` is the recovery key.

### Step 1 — Surface the stale claim

```
planar-watch claims --plan 297 --json
```

Look for a row with `status: "stale"` (or `"active"` past its TTL — the reconcile pass below converts them to `stale`):

```json
{
  "claim_token": "9f2c1e44b8a7c3d6...",
  "entity_kind": "task",
  "entity_id": 2931,
  "status": "stale",
  "expires_at": "2026-05-29T14:23:00Z",
  "worktree_path": "/repo/.worktrees/cycle/worktree-management/m2-handlers-a",
  "branch": "cycle/worktree-management/m2-handlers-a",
  "head_sha_at_claim": "a1b2c3d4...",
  "dirty_at_claim": "clean"
}
```

The `worktree_path` is what makes recovery deterministic — without it you'd be guessing which on-disk directory belonged to which dead coder.

### Step 2 — Dry-run the reconciler

```
planar-agent reconcile --dry-run
```

Shows what reconciliation would do without writing:

```
reconcile (dry-run):
  would mark stale: claim:9f2c1e44... task:2931 (last heartbeat 47m ago, TTL 10m)
  would mark stale: 0 actions
```

If the only stale row is the dead coder you already identified, you're safe to reconcile. If unexpected claims show up, investigate before proceeding.

### Step 3 — Inspect the on-disk worktree

```
cd /repo/.worktrees/cycle/worktree-management/m2-handlers-a
git status
git log --oneline master..HEAD
```

Look for:
- **Uncommitted changes** (working tree modified, index modified). The coder died mid-edit; partial state is unrecoverable without the operator's judgement.
- **Unmerged commits on the cycle branch** that did not make it into the epic. The coder committed before dying but did not call the terminal verb. Recoverable.
- **A clean tree with no commits ahead of the epic.** The coder claimed but never made progress. Abandon-and-restart is the easy path.

### Step 4 — Pick a recovery option

Two paths. Pick based on what you found in Step 3.

#### Option A — Re-dispatch a fresh coder into the same worktree

When the coder made meaningful progress (committed work on the cycle branch) and you want a new coder to continue from where it left off.

```
# 1. Reconcile to mark the lease-expired claim stale.
planar-agent reconcile

# 2. Claim the task again, reusing the existing worktree path.
planar-agent claim --entity task:2931 --role coder \
    --worktree /repo/.worktrees/cycle/worktree-management/m2-handlers-a \
    --json
#   → returns a fresh claim_token bound to the same worktree_path

# 3. Dispatch the fresh coder. The brief tells it to cd into the worktree
#    and continue against the existing cycle branch.
```

The orchestrator's normal heartbeat-and-terminal ritual takes over from there. The new coder inherits the cycle branch's commits and works on top of them.

#### Option B — Abandon the worktree and resolve manually

When the on-disk state is unrecoverable (mid-edit garbage), or when the operator wants the partial work preserved for offline review before deciding what to do.

```
# 1. (Optional) Preserve the partial work on a forensic branch so the
#    cycle worktree's contents are not lost when the worktree is removed.
cd /repo/.worktrees/cycle/worktree-management/m2-handlers-a
git add -A && git commit -m "WIP: dead-coder partial state for task:2931 (forensic)"
git push origin cycle/worktree-management/m2-handlers-a

# 2. Abort the claim so the task returns to the pickable pool.
planar-agent abort --claim 9f2c1e44b8a7c3d6... --reason "operator: coder died, partial state preserved on origin"

# 3. Clean up the worktree and (if you don't want the cycle branch locally) the branch.
cd /repo
git worktree remove /repo/.worktrees/cycle/worktree-management/m2-handlers-a
# Keep the cycle branch locally if you want to refer to it; otherwise:
git branch -D cycle/worktree-management/m2-handlers-a
```

The task is now back in the pickable pool. You can either dispatch a fresh coder against a freshly-cut cycle branch (the orchestrator will create both), or hand-edit the recovery against the epic worktree directly if the task no longer fits the cycle shape.

### Why this recipe works

The persisted `worktree_path` on the (now stale or aborted) claim is the recovery key. Without it you'd be left scanning `/repo/.worktrees/` and guessing which directory belonged to which dead coder. With it, every operator-side recovery verb (`planar-watch claims`, `planar-agent reconcile`, `planar-agent abort`) surfaces or operates against the path deterministically, and a fresh `planar-agent claim --worktree <path>` reuses the on-disk checkout instead of creating a new one.

For the persistence-on-claim contract see [`docs/concepts.md §Worktree`](concepts.md#worktree); for the claim ritual see `skills/planar/references/claim-ritual.md`.

---

## Recipe 27 — Finalize and close out a plan (Phase 3.7)

Use this when coder/reviewer cycles are complete, commits are pushed, and you want to merge the work, reconcile Planar state, clean up branches, and formally close the plan.

**Prerequisites:**

- At least one approved coder cycle has run (`reviewer_disposition: dispatched` in the audit trail).
- Commits are pushed and a PR is open and mergeable.
- You (the operator) have explicitly opted in — finalization never runs automatically.

### Step 1 — Verify the PR is ready

```bash
gh pr view <N> --json state,mergeable,mergeStateStatus
```

All three fields must be ready: `state=OPEN`, `mergeable=MERGEABLE`, `mergeStateStatus=CLEAN` (or `HAS_HOOKS`).

### Step 2 — Check the closeout gate (dry-run)

Before merging, confirm the Planar DB state will allow the plan to close:

```bash
planar plan closeout <plan-id> --dry-run --json
```

Parse `ready` and `blocked_by`:

- `ready: true`, `blocked_by: []` — proceed.
- `ready: false`, `blocked_by` non-empty — resolve blockers first:
  - Open tasks (`todo`/`doing`/`blocked`) — finish or cancel them.
  - Open descendant plans — close child plans first (recursively).
  - Live claims — wait for the claiming agent to finish, or `planar-agent reconcile` for expired leases.

Optionally, add `--check-merge` to see whether each contributing branch is already merged to the target:

```bash
planar plan closeout <plan-id> --dry-run --check-merge --json
```

The `epic_merge` section is advisory and never blocks. Absent branches (deleted post-merge) are reported as inconclusive.

### Step 3 — Dispatch the janitor (or run manually)

**Via the orchestrator (automated):** Name `--finalize` in the `planar-orchestrator` brief or confirm the finalization prompt when the orchestrator offers it. The orchestrator dispatches the `planar-janitor` agent, which executes the six-step finalization flow.

**Manually (standalone):**

```bash
# Merge the PR (remote branch deleted automatically via --delete-branch).
gh pr merge <N> --merge --delete-branch

# Wait for confirmed merge.
gh pr view <N> --json state,mergedAt

# Reconcile stale Planar claims.
planar-agent reconcile --dry-run   # preview
planar-agent reconcile             # apply

# Clean up local branch and update main.
git fetch --prune origin
git pull --ff-only origin main

# Apply closeout.
planar plan closeout <plan-id> --json
```

### Step 4 — Verify the plan is done

```bash
planar plan show <plan-id> --json
# → "status": "done"
```

### Step 5 — Optional archive

```bash
planar workbench archive <plan-id>
```

Removes the workbench FS tree for the plan (`~/.planar/workbench/<assoc>/p<id>-*/`). Archive is a separate, explicit step after closeout; finalization alone does not archive.

### Step 6 — Optional reinstall

If the merged change modified binary behavior (engine code, CLI handlers, migrations):

```bash
cd /path/to/planar-repo && ./install.sh
```

**Finalization tasks convention:** The janitor (and orchestrator) create tasks with slug prefixes `finalize-`, `merge-`, or `reconcile-` to track discrete merge/reconciliation work items. `planar plan closeout --json` reports these as `hard_evidence.finalization_tasks` — an advisory count that identifies finalization work in the audit output without changing gate logic.

**Capability boundary reminder:** Coders close tasks via `planar-agent complete`. The janitor is the only agent role that runs `planar plan closeout`. The orchestrator dispatches the janitor; it never calls closeout directly. See `docs/concepts.md § Closeout gate` and `agents/planar-janitor.md` § Capability and hard boundary for the full detail.

## Recipe 29 — Orient, diagnose, and choose work

Use the skill's references instead of reconstructing state from unrelated
lists:

- **Scope status** (what needs attention, and the next work that is safe to
  claim): the planar skill's `status.md` reference. Its reads are
  `planar scope show`, `planar dashboard --agents`,
  `planar task list --status blocked`, `planar question list --status open`,
  `planar handoff list --status pending,validated` and `planar-ext sync status`.
- **One plan's live activity** (actions, claims, failures, sync, handoffs): the
  same reference, using `planar-watch actions --plan <plan-id>`,
  `planar-watch ps --plan <plan-id> --stale`, `planar-watch feed --plan <plan-id>`
  and `planar-watch sync-events --plan <plan-id>`.
- **Global health**: the `recovery.md` reference, starting from
  `planar health --json`.

Scope status suppresses empty sections and orders conflicts, stale
coordination state, blockers and questions, active claims, and claim-aware
next work. It never recommends a claimed, stale, or blocked task. The activity
views are read-only and distinguish an empty interval from unavailable
telemetry. Health never repairs: `recovery.md` explains each degraded
contributor and routes to the doctor flow, `resume-handoff.md`, a reconciliation
preview (`planar-agent reconcile --dry-run`), configuration validation, or the
`repair_command` that `projection_freshness` reports for a stale or missing
skill or agent placement. Use the `planar plan` and `planar task` verbs
(`planar plan --help`, `planar task --help`) for the full lifecycle once a
target is selected; read the post-state after every mutation, and let claimed
work terminate atomically through `planar-agent`.

---

## Recipe 30 — Capture a durable knowledge chain

Follow the planar skill's `knowledge.md` reference when an intent spans
decisions, artifacts, annotations, and typed relationships:

```bash
planar decision add "Adopt SQLite WAL" --plan 42 --rationale "Readers do not block the writer"
planar annotate add --anchor-path src/lib/db/db.cpp --line-start 88 --plan 42 \
  --title "Retry boundary" --body "Explain the retry boundary"
planar links add decision:9 artifact:17 --relationship cites
```

Resolve every natural-language target to exactly one typed entity before
writing. If resolution is ambiguous, show the candidates and stop with
`applied=0`; never guess. Each approved mutation uses the normal scope-guarded
entity verb, then reads the entity and relationship back. A multi-target
partial result keeps verified independent changes and supplies an exact retry
or inspection command for each failure.

---

## Recipe 32 — Reconcile a sync conflict safely

Start from current link state and durable audit evidence (the planar skill's
`external-sync.md` reference):

```bash
planar-ext sync status
planar-ext sync pull task:<task-id>
planar audit trail --link <link-id> --json
```

With no current conflicts, this is a verified no-op: do not dispatch the
reconciler or call resolve. For a conflict, dispatch `planar-sync-reconciler`;
it reads and recommends, and must show both observable values, provenance,
observation times, and provider version. Missing or stale evidence forces
`defer` and a fresh guarded pull. `keep-local` and `keep-remote` are
whole-entity choices and require explicit approval for the displayed event and
evidence, then:

```bash
planar-ext sync resolve <conflict-event-id> --keep local \
  --evidence-token <approved-token> \
  --expected-local-updated-at <reviewed-updated-at> --json
```

`manual-merge` is two-gated: first edit the local entity through its ordinary
scope-guarded workflow and review its post-state; then separately approve
`keep-local` for that conflict event. Approval of merge text never authorizes
either mutation. Multi-event application preserves independently resolved
events and reports failed or deferred events with exact recovery commands.

---

## Recipe 34 — Review recurring design hazards before ingestion

Dispatch `planar-spec-reviewer` for the plan and inspect all four hazard-lens rows:

1. resource lifecycle and cleanup;
2. deterministic ordering and replay;
3. concurrency, ownership, cancellation, and races;
4. shell, build, and template escaping across interpreter boundaries.

Each row must be `finding`, `covered`, or `not applicable` with artifact
evidence. Resolve applicable findings in the draft and rerun the read-only
review before ingestion; make draft edits only as separately
operator-approved changes.
