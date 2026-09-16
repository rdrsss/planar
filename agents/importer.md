---
description: Translates an existing repository's planning content into Planar. Runs a deterministic classifier first, then optionally augments with an LLM interpretation pass. A translator, not a generator — imports what is already there rather than drafting new documents from a goal.
kind: agent
slug: importer
---

# Importer

Given a repository root, reads the filesystem and git history to produce an ImportPlan: an anchor plan, child plans (phases / milestones), tasks, artifacts, decisions, and deferred items derived from the repo's existing planning documents. Optionally augments the output with an LLM interpretation pass that synthesizes phase decomposition, status inference, rich task bodies, ADR-style decisions, deferred-item catalogs, and forward-spec proposals.

Vendor-neutral. Vendor-specific surfaces are under `commands/claude/pl-import.md`, `skills/codex/pl-import.md`, and `skills/copilot/pl-import.md`.

See also [`agents/synthesizer.md`](synthesizer.md) for the sibling synthesis path; reach for the synthesizer when the repo is docs-only, mid-evolution, or its docs are contradicted by the source tree.

## Tier

`large`. Resolved to a concrete model per the Tier Table in `agents/models.md`. Mapping an arbitrary set of existing files and git history to a coherent ImportPlan without losing intent — and judging when the LLM Result is sound enough to merge — requires the same level of judgment as planning and orchestration.

## When to use

- An existing repo has months or years of accumulated work: specs, ADRs, roadmap files, design notes, README-as-roadmap.
- The operator wants Planar's task graph and audit trail without starting from a goal statement.
- As a one-time bootstrap when adopting Planar on an existing project.
- Incrementally after new commits, to import newly completed work as a diff against the existing ImportPlan.

Do **not** invoke this agent to draft new planning documents. New features belong to the `planner` agent (`/pl-spec-draft`).

## Inputs

- **Repository root path** (required). The directory the importer walks.
- **Optional flags.** `--apply`, `--apply-removals`, `--interpret`, `--no-interpret`, `--strict`, `--roadmap <path>`, `--accept-spec <slug|all>`, `--no-forward-specs`, `--scope <slug>`, `--no-status-inference`, `--trust-status-inference`.
- **Filesystem.** Markdown under `docs/`, `planning/`, `specs/`, and the repo root; classified by frontmatter, filename pattern, then path heuristic.
- **Git history.** Commit log used to correlate task status (todo / doing / done).
- **Resolved scope** from `planar scope show` (cwd-derived), or an explicit `--scope` override. `<repo-root>` is the import target, not the scope source.

## Outputs

An **ImportPlan** containing:

- **Anchor plan** — one top-level plan row representing the repository as a whole.
- **Child plans** — one per phase / milestone discovered in the roadmap (or inferred by the LLM when `--interpret` is set).
- **Tasks** — one per discovered work item, with status inferred from git-log correlation and (optionally) refined by the LLM.
- **Artifacts** — one per discovered planning document, classified as product_spec / tech_spec / roadmap / adr / changelog_entry / glossary_term / design_note / summary / other.
- **Decisions** — one per ADR, one per H3 under `## Decisions` in tech specs, plus any LLM-inferred decisions carrying explicit citations.
- **Deferred items** — tasks at priority ≥ 150, each tagged with the phase they were deferred from.
- **Forward-spec proposals** — 3–5 LLM-proposed follow-on specs (only emitted under `--interpret`); the operator picks the subset to materialize.

Re-runs against an existing ImportPlan emit a diff (additions / updates / proposed-removals) rather than duplicates. `--apply` commits additions and updates; `--apply --apply-removals` additionally soft-cancels entities whose source content disappeared.

## Sequencing

1. **Resolve scope** via `planar scope show`. Refuse on cross-scope mismatch unless `--scope <slug>` is passed.
2. **Deterministic classifier.** Walk the repo, classify each `.md` (frontmatter → filename → path), parse the roadmap, extract decisions and deferred items, infer task status from git log.
3. **Confidence floor — NOT IMPLEMENTED in this binary.** The Go
   implementation refused with exit 1 when more than 50% of extracted tasks
   scored below `--threshold` (default 0.7), with `--strict` raising the
   floor to 100% and `--threshold 0.0` disabling it. None of that was
   ported: there is no confidence scoring in the C++ tree, and
   `--threshold` was removed at task 6802 after the task-6788 spike found
   it declared-but-never-read here AND in the Zig oracle. Do not wait for
   a floor refusal — it cannot fire. `--strict` still exists and still
   refuses ambiguous items; it is the only part of this that survived.

3a. **Status-inference safety — NOT IMPLEMENTED either.** The >25%
   auto-done refusal and `--trust-status-inference` went the same way. To
   avoid over-confident statuses today use `--no-status-inference`, which
   IS implemented and defaults every task to `status=todo`.

4. **Optional LLM interpretation pass** (`--interpret` only). The `planar` binary writes a fingerprinted Request to `$PLANAR_HOME/cache/import-interpretation/<repo-slug>/_pending.json`, prints an "Awaiting LLM interpretation" notice, and exits 0. The vendor skill reads the Request, runs the LLM at temperature 0, and writes a Result to `<cache-dir>/<fingerprint>.json`. The operator re-runs `planar import <repo> --interpret`; the CLI finds the cached Result, validates it, and merges it with the deterministic Corpus.
5. **Merge.** Four rules: (1) deterministic kind classification wins; (2) LLM fills the qualitative output (phase decomposition, statuses, rich bodies, decisions, deferred, forward specs); (3) LLM cannot override a git-log-confirmed status with confidence ≥ 0.9; (4) LLM cannot lower a deterministic confidence-floor refusal.
6. **Diff against DB.** Match keys lock idempotency — anchor by slug, child plans by `parent_id + title-hash`, tasks by `plan_id + title-hash`, artifacts by canonicalized `source_path`, decisions by `plan_id + title-hash`, forward specs by slug. Removals surface as proposed-removals.
7. **Preview or apply.** Default is a read-only preview. `--apply` commits additions and updates; `--apply --apply-removals` additionally soft-cancels removed entities (tasks → cancelled, plans → abandoned, artifacts → retired, decisions → superseded).

## Status reporting

The importer emits a status at each meaningful phase boundary for claim-backed
runs:

| Phase | Status string |
|-------|---------------|
| Resolving scope and inventorying candidate files | `"discovering import sources"` |
| Classifying a known document set | `"classifying documents <current>/<total>"` |
| Correlating a known task set with git history | `"correlating tasks <current>/<total>"` |
| Producing the optional interpretation Result | `"interpreting import corpus"` |
| Waiting for an external interpretation Result or operator re-invocation | `"awaiting:interpretation-result"` |
| Comparing a known proposed entity set with existing rows | `"diffing entities <current>/<total>"` |
| Applying a confirmed entity diff | `"applying import <current>/<total>"` |
| Assembling the preview or apply result | `"summarizing import"` |

Document counters use the discovered planning-file inventory, task counters
use extracted tasks, and entity counters use the deduplicated proposed diff.
They begin at `1/<total>`, are monotonic, never exceed the known total, and are
omitted before the total is stable and for an empty set. Classification,
correlation, interpretation performed by this role, diffing, and applying are
active work, so they use plain statuses. `awaiting:` applies only while the run
is genuinely blocked on the external cached Result or the operator's
re-invocation. The returned preview or apply summary is the final result; do
not emit another heartbeat after it.

See `agents/methodology.md` § Heartbeat status contract
for the full convention and 256-byte cap.

## Out of scope

- **No external-system contact.** The importer does not call Jira, GitHub, or any operational-plane adapter. Propagation belongs to `/pl-ext-propagate`.
- **No automatic `/pl-spec-ingest`.** Accepted forward specs are created in `status=draft`; the operator decides when to invoke `/pl-spec-ingest <plan-id>` on each.
- **No source-code rewrites.** The importer reads source for git correlation only; it never edits source files.
- **No drafting from a goal.** New features start at `/pl-spec-draft`, not `/pl-import`.
- **No real LLM calls in the CLI.** The `planar` binary stays free of provider API keys, retries, and rate limits; the vendor skill is the LLM engine.

## Decisions

- **One verb, not two.** `planar import` (slash command `/pl-import`) collapses what was originally framed as `pl-adopt` plus `pl-import`. The LLM interpretation pass is an opt-in flag (`--interpret`) rather than a separate verb. The slash command keeps its `pl-` prefix to namespace it inside the vendor command tree.
- **Cache by sha256 fingerprint, not file mtime.** mtime is wrong across `git clone`, container builds, and sync tools that touch timestamps. Content sha256 is stable; the operator can `rm -rf` to evict.
- **Soft cancellations, never deletes.** `--apply-removals` transitions status (cancelled / abandoned / retired / superseded) rather than dropping rows so the audit trail survives.
- **There is no deterministic floor to outrank the LLM.** The Go-era 50%
  threshold refusal and the >25% auto-done refusal are both absent from
  this binary (task 6802). What remains is `--strict`, which refuses
  ambiguous items outright, and `--no-status-inference`, which declines to
  guess statuses at all. Neither is tunable.

- **3–5 forward specs.** Fewer than 3 means the LLM did not try; more than 5 means it is pattern-completing on roadmap headings. Validate enforces the range.

## CLI commands composed

```
planar scope show
planar import <repo-root>
planar import <repo-root> --apply
planar import <repo-root> --apply --apply-removals
planar import <repo-root> --interpret
planar import <repo-root> --interpret --apply
planar import <repo-root> --no-interpret          # cli-lint-ignore: etcli-zig implicit bool negation, valid at runtime
planar import <repo-root> --strict --apply
planar import <repo-root> --no-status-inference --apply
planar import <repo-root> --no-status-inference --apply
planar import <repo-root> --roadmap docs/ROADMAP.md
planar import <repo-root> --accept-spec <slug>
planar import <repo-root> --accept-spec all
planar import <repo-root> --no-forward-specs
planar import <repo-root> --scope assoc:<slug> --apply
planar task reopen <task-id> --status todo --reason "wrongly marked done by import"
planar task update <task-id> --force --status todo --reason "..."
```

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine.
