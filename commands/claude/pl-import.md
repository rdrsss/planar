---
description: Import an existing repo's planning content into Planar — deterministic classifier plus opt-in LLM interpretation.
argument-hint: <repo-root> [--apply] [--apply-removals] [--interpret|--no-interpret] [--accept-spec <slug>|all] [--no-forward-specs] [--strict] [--threshold N] [--roadmap <path>] [--scope <slug>] [--no-status-inference] [--trust-status-inference]
model: claude-opus-4-7
source: agents/importer.md
---

# Planar Import (Claude)

Claude skill surface for the vendor-neutral `importer` agent. See [`agents/importer.md`](../../agents/importer.md) for the full role spec, input/output contract, and workflow steps.

Vendor-neutral skill that imports an existing repo's planning content into Planar. Combines a deterministic Go-side classifier with an opt-in LLM interpretation pass.

## What It Does

Scans the repo, classifies its docs, extracts decisions plus deferred items, and produces an ImportPlan that creates an anchor plan plus child plans plus tasks plus artifacts in the database. With `--interpret`, additionally invokes the LLM to synthesize rich task bodies, infer phase statuses, propose forward specs, and surface ADR-style decisions. The deterministic floor is the contract; the LLM is augmentation.

## Sibling Verb: pl-synthesize

`pl-synthesize` is the synthesis counterpart to pl-import's transcription. Use `pl-import` for clean, structured, current docs that you want transcribed as-is; reach for `pl-synthesize` instead when the repo is docs-only / greenfield, when docs are mid-evolution, when multiple roadmaps of different eras coexist, or when the docs claim done but the code shows incomplete. Both verbs land in the same downstream `pl-spec-ingest` pipeline. See [`docs/concepts.md#transcription-vs-synthesis`](../../docs/concepts.md#transcription-vs-synthesis) for the decision matrix and [`agents/synthesizer.md`](../../agents/synthesizer.md) for the sibling role spec.

## When To Invoke

When you have an existing repository with planning material — README, design docs, roadmap files, ADRs, status notes — and you want to land it in Planar without manually creating dozens of entities.

This is NOT the right tool for:

- Greenfield repositories with no planning content yet — use `pl-spec-draft` instead.
- Adopting just the SCOPE (creating the `assoc:` row) — use `planar workspace init` or `planar init`.
- Re-importing after the source has changed — re-running pl-import produces a diff, not duplicates (the import is idempotent).

## How It Classifies

Resolution order, first-match wins:

1. **Frontmatter.** Files whose YAML frontmatter declares `artifact_kind: <kind>` are classified directly. Trumps everything else.
2. **Filename patterns (case-insensitive).** `*product_spec*.md` → product_spec, `*tech_spec*.md` → tech_spec, `*roadmap*.md` → roadmap, `*adr*.md` → adr, `changelog.md` → changelog_entry, `glossary.md` → glossary_term.
3. **Path heuristics.** `CLAUDE.md`, `AGENTS.md`, `copilot/*.md`, `.claude/*.md`, `.codex/*.md` → kind=guide (NEVER mined for backlog content; they are guides, not work).
4. **Default.** kind=other, classified as path-only.

The classifier also auto-discovers roadmap files by walking `docs/`, `planning/`, `specs/`, and the repo root for case-insensitive `*roadmap*.md`.

## What It Detects

Two visually distinct sections in the preview:

**Detected artifacts** — product_spec / tech_spec / roadmap / design_note / changelog_entry / glossary_term / summary entries created from classified files. Each shows its source: `[from frontmatter]`, `[from filename]`, or `[from path]`.

**Extracted tasks** — bullet items lifted from the roadmap's H2 milestone bodies. Each task carries a confidence score (0.0–1.0) from the deterministic heuristic and a status (todo by default; doing/done inferred via git-log correlation; the LLM may upgrade).

**Skipped** — guide files (CLAUDE.md etc.) and "other" .md files surfaced as a count, never mined for backlog.

## Roadmap Discovery

pl-import walks `docs/`, `planning/`, `specs/`, and the repo root for `*roadmap*.md`. When multiple roadmaps match, the shortest path wins; the rest are listed in the preview as a warning. Pass `--roadmap <path>` to override discovery.

## Confidence Floor

The preview refuses (exit 1) when more than 50% of extracted tasks score below `--threshold` (default 0.7). The refusal names the threshold, lists the low-confidence tasks, and points at `--threshold 0.0` to relax or `--strict` to require every task clear the threshold. The floor runs before the LLM merge so a noisy preview cannot be rescued by good LLM output.

## Status-Inference Safety (Greenfield + Docs-Only Repos)

Status inference layers 2-3 (branch name and git-log correlation) produce false positives in repos where the git history records doc-authoring commits but no implementation. The literal example: a docs-only repo with `add v2 technical roadmap` in the log will match task titles inside that roadmap and auto-mark them as `status=done`.

Three knobs cover the cases:

- `--no-status-inference` defaults every task to `status=todo`, `signal=no-inference`, `confidence=0`. Skips layers 2-3 entirely; layer 1 (operator-explicit checkbox state) still runs. Use this for greenfield repos, docs-only repos, fresh forks, or any case where you do not trust the git log as a signal of implementation status.
- `--trust-status-inference` is the explicit opt-in to the >25% auto-done bypass below. Use this only when you have a clean repo with a genuinely high done-count and have manually verified the inferred done marks.
- `--threshold 0.0` disables the confidence floor and is the legacy escape hatch. It now triggers the safety net described next.

### >25% Auto-Done Refusal

When `--threshold 0.0` disables the confidence floor AND more than 25% of inferred tasks would land as `status=done` via git-log correlation, pl-import refuses the import with:

```
warning: --threshold 0.0 would auto-mark <N>/<TOTAL> tasks (<P>%) as `status=done`
         based on git-log correlation. In docs-only or fresh repos this is
         almost always wrong. Refusing the import.

Options:
  --no-status-inference         skip inference entirely; default every task
                                to status=todo
  --trust-status-inference      explicit bypass; commit the done-marks (only
                                when you've verified them)
```

`--no-status-inference` short-circuits the check (no git-log signal means no done marks to refuse over). `--trust-status-inference` bypasses the refusal so a clean repo with a legitimate high done-count can still apply.

### Recovery: `planar task reopen <id>`

If an earlier import landed wrong done marks before this safety net existed, use `planar task reopen <task-id> [--status todo] [--reason "..."]` to walk individual tasks back to a non-terminal status. The transition is recorded in the `task_reopens` audit table so the lifecycle remains reconstructible. `planar task update --force --status todo` is the lower-level escape hatch; the dedicated `task reopen` verb is the documented entry point.

## --interpret Pass

`pl-import . --interpret` opts into the LLM interpretation pass on top of the deterministic floor. The skill body is the LLM engine; the Go side validates whatever the skill produces. Workflow:

1. Go runs the deterministic classifier, builds an interpretation Request from the resulting Corpus, and computes the Request's sha256 `fingerprint`.
2. On a cache miss, Go writes the Request to `$PLANAR_HOME/cache/import-interpretation/<repo-slug>/_pending.json`, prints an "Awaiting LLM interpretation" notice naming the pending and target paths, and exits 0.
3. The vendor skill (this skill body) reads the Request, runs the LLM at temperature 0, and writes a Result to `$PLANAR_HOME/cache/import-interpretation/<repo-slug>/<fingerprint>.json`.
4. The operator re-runs `planar pl-import <repo> --interpret`. Go finds the cached Result, validates it via the Validate rules below, and merges it with the deterministic Corpus per the four merge rules below.

The Request payload carries: README + each `docs/*` body + git log (last ~500 commits) + guide files (CLAUDE.md as CONTEXT, never backlog) + a tree summary + the detected_artifacts produced by the classifier. The skill must NOT mine guide files for tasks.

## LLM Result Contract

The skill writes a JSON Result matching this schema. The canonical Go types live in [`src/internal/adopter/interpretation/result.go`](../../src/internal/adopter/interpretation/result.go); the schema below mirrors the field set.

```json
{
  "schema_version": 1,
  "fingerprint": "<sha256 hex matching Request.Fingerprint>",
  "anchor_title": "Lectio",
  "phases": [
    {
      "slug": "phase-1-foundation",
      "title": "Phase 1 — Foundation",
      "status": "done",
      "summary": "W1. SwiftPM package scaffold, SwiftData core models, FTS sidecar migration.",
      "tasks": [
        {
          "slug": "wal-replay",
          "title": "Implement WAL replay loop",
          "body": "Multi-sentence rich body referencing tech-spec sections.",
          "status": "done",
          "priority": 100,
          "next_action": "",
          "citations": [{"path": "docs/tech_spec.md", "section": "§2"}]
        }
      ]
    }
  ],
  "decisions": [
    {
      "slug": "module-split",
      "title": "Why a five-target module split",
      "body": "Five-target SwiftPM layout. ...",
      "rationale": "Keeps Mac/iPad parity additive. See tech spec §2.",
      "source": "tech-spec",
      "citation": {"path": "docs/tech_spec.md", "section": "§2"}
    }
  ],
  "deferred_items": [
    {
      "slug": "apple-translation-bridge",
      "title": "Apple Translation runtime bridge",
      "body": "Origin: Deferred from W10. Replace the deterministic stub once available in SwiftPM.",
      "phase_slug": "phase-4-polyglot",
      "priority": 150,
      "origin_pattern": "Deferred from"
    }
  ],
  "forward_specs": [
    {"slug": "v1-1-polish", "title": "v1.1 — Translation polish", "goal": "Triage post-v1 polish."},
    {"slug": "v2-multi-lang", "title": "v2 — Multi-language support", "goal": "Add Spanish + French."},
    {"slug": "research-mcp", "title": "Research — MCP integrations", "goal": "Evaluate MCP server surface."}
  ],
  "provenance": "claude-opus-4-7 temperature=0",
  "generated_at": "<RFC3339 UTC>"
}
```

Hard contract rules (Validate enforces; the skill MUST produce conformant output):

- `schema_version` is exactly `1`.
- `fingerprint` matches the Request's fingerprint byte-for-byte (Validate rejects mismatches, which prevents stale-cache poisoning).
- `anchor_title` is non-empty.
- Each phase carries a unique slug; phase status is one of `draft`, `active`, `paused`, `done`, `abandoned`.
- Each task within a phase carries a unique slug; task status is one of `todo`, `doing`, `blocked`, `done`, `cancelled`; **at most ONE task per phase has status=doing**.
- Task priority is in `[0, 1000]`.
- Each decision with `source: "llm-inferred"` carries a non-empty `citation.path`.
- Each deferred item has priority ≥ 150 and a `phase_slug` matching an existing phase.
- Forward specs count is 3–5 (the operator picks the subset to materialize; Validate rejects 2 or 6).
- `provenance` is non-empty.

## Merge Rules

When Go re-reads the Result and merges with the deterministic Corpus:

1. **Deterministic kind classification wins.** If the classifier said a file is `product_spec`, the LLM cannot reclassify it.
2. **LLM fills the qualitative output.** Phase decomposition, statuses, rich task bodies, decisions, deferred items, and forward specs all come from the LLM Result when present; otherwise deterministic-only.
3. **LLM cannot override git-log-confirmed status with confidence ≥ 0.9.** When the deterministic side has high-confidence status from commit correlation, the LLM's status field is ignored for that task.
4. **LLM cannot lower a deterministic confidence-floor refusal.** The floor runs before the LLM merge; a noisy preview cannot be rescued by good LLM data.

## Forward Specs

The LLM proposes 3–5 forward specs. The operator picks the subset to materialize:

- Default (interactive). pl-import prompts "Accept forward spec `<slug>`? [y/N/q]" per proposal. `y` accepts, Enter or `n` skips, `q` skips all remaining.
- `--accept-spec <slug>` (repeatable). Accept by slug non-interactively.
- `--accept-spec all`. Accept every proposal.
- `--no-forward-specs`. Skip the phase entirely.

Each accepted forward spec creates an anchor plan in `status=draft`, product_spec / tech_spec / roadmap artifacts seeded from inline templates, and a workbench tree at `~/.planar/workbench/<assoc>/p<id>-<slug>/`. The operator's next move is `pl-spec-ingest <plan-id>` to decompose each draft into milestones plus tasks. **pl-import does NOT auto-call `pl-spec-ingest`.**

## Idempotent Re-runs

Running pl-import twice on the same repo produces a diff, not duplicates:

- **Additions.** New entities the proposed ImportPlan would create.
- **Updates.** Existing entities whose proposed shape differs (title change, status drift).
- **Proposed-removals.** Existing entities the new ImportPlan no longer references (a phase removed from the roadmap, a task whose source bullet is gone).

Match keys: anchor plan by slug; child plans by `parent_id + title-hash`; tasks by `plan_id + title-hash`; artifacts by canonicalized `source_path`; decisions by `plan_id + title-hash`; forward specs by slug.

Removal semantics are soft — status transitions only, no row deletes:

- Task → cancelled
- Plan → abandoned
- Artifact → retired
- Decision → superseded

`--apply` honors additions and updates only. `--apply --apply-removals` additionally commits the soft-cancellations.

## CLI Commands

Wraps [`pl-import`](../../docs/cli-reference.md#domain-pl-import).

> **Scope.** Reads use the caller's cwd-derived scope; writes refuse on cross-scope mismatch (see [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard)). `<repo-root>` is the import target, not the scope source. When invoking against a repo that is not the caller's cwd, pass `--scope <slug>` explicitly or `cd` into the target first. The active scope stack was removed in plan 153 M5; there is no `scope use` to push.

> **Cross-scope guard.** This verb refuses with exit 1 when the operator's resolved write scope disagrees with the target entity's stored scope. Run from inside the entity's owning repo, pass `--scope <slug>` explicitly, or use `--no-scope-check` for legacy escape (not for routine use). See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard) for the full guarded/unguarded matrix.

```
planar pl-import <path>                              # preview, deterministic only
planar pl-import <path> --apply                      # commit additions + updates
planar pl-import <path> --apply --apply-removals     # commit + soft-cancel removed entities
planar pl-import <path> --interpret                  # opt into LLM pass
planar pl-import <path> --interpret --apply
planar pl-import <path> --no-interpret               # explicitly deterministic-only
planar pl-import <path> --strict --apply             # every task must clear --threshold
planar pl-import <path> --threshold 0.0 --apply      # disable the confidence floor
planar pl-import <path> --no-status-inference --apply # docs-only / greenfield: all tasks land todo
planar pl-import <path> --threshold 0.0 --trust-status-inference --apply # bypass >25% refusal
planar pl-import <path> --roadmap <path>             # override roadmap auto-discovery
planar pl-import <path> --accept-spec <slug>         # non-interactive forward-spec selection
planar pl-import <path> --accept-spec all
planar pl-import <path> --no-forward-specs
planar pl-import <path> --scope <slug>               # override cwd-derived scope
```

## Output Shapes

Default (text):

```
project:planar:lectio/

Detected artifacts (4):
  + docs/lectio_product_spec.md   → product_spec   [from frontmatter]
  + docs/lectio_tech_spec.md      → tech_spec      [from frontmatter]
  + docs/lectio_roadmap.md        → roadmap        [from filename]
  + docs/lectio_implementation_status.md → summary [from filename]

Extracted tasks (5):
  [0.81] [ ] Implement WAL replay loop      [plan: Phase 1 — Foundation]
  [0.74] [ ] Add --json flag to lectio status
  ...

Skipped (2 files):
  CLAUDE.md (classified as guide; not mined for backlog)
  1 other .md file

Proposed plan additions:
  + plan: Lectio (anchor)
    + plan: Phase 1 — Foundation (3 tasks from roadmap)
    + plan: Phase 2 — Stable     (2 tasks from roadmap)

7 additions, 0 updates, 0 proposed-removals.
Run with --apply to commit.
```

`--json` emits the same shape as machine-readable JSON.

## Authoring Conventions

Apply the six structured-authoring rules from task 602: quoted titles, literal headings, no nested bullets, no `## Out of this plan` H2, workbench discipline, and only annotate skills with the guard note when the guarded verb literally appears in the body.

## Vendor Notes

- Installed to `~/.claude/commands/pl-import.md`.
- Invoked as `/pl-import <subcommand> [args]`.
- Import state and interpretation cache entries come from the CLI; the skill must not invent direct DB writes or repo-local scaffolding.

## Invocation

```
/pl-import .                                # preview the current repo
/pl-import . --apply                        # commit the import
/pl-import . --interpret                    # opt into the LLM pass
/pl-import . --interpret --apply
/pl-import /path/to/other-repo --dry-run
/pl-import . --strict --threshold 0.85 --apply
/pl-import . --scope assoc:project:my-app --apply
```
