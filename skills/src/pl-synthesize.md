---
description: Synthesize fresh planning artifacts for a repo from existing docs + git log + source code via an LLM pass. Use for messy / docs-only / mid-evolution repos.
origin: agents/synthesizer.md
shared_notes:
    - Synthesis cache state and deterministic baselines come from the CLI; the skill must not write planning rows directly.
slug: pl-synthesize
vendor:
    claude:
        argument_hint: <repo-root> [--apply] [--scope <slug>] [--accept-spec <slug>] [--code-layout <name>] [--no-forward-specs] [--literal] [--treat-as-greenfield] [--treat-as-nongreenfield] [--threshold N]
        invocation_examples: |
            /pl-synthesize .                          # preview the current repo
            /pl-synthesize . --apply                  # commit the synthesis
            /pl-synthesize /path/to/other-repo --dry-run
            /pl-synthesize . --code-layout go --apply
            /pl-synthesize . --scope assoc:project:my-app --apply
            /pl-synthesize . --literal                # delegate to pl-import
---

# Planar Synthesize ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `synthesizer` agent. See [`agents/synthesizer.md`](../../agents/synthesizer.md) for the full role spec, input/output contract, and sequencing.

Vendor-neutral skill that imports an existing repo by SYNTHESIZING fresh planning artifacts from the repo's docs + git log + source code, rather than transcribing the existing docs verbatim.

## What It Does

Reads everything in a repo — READMEs, planning docs, git log, the source tree via the deterministic codeprobe EvidenceMap — and produces a fresh product-spec / tech-spec / roadmap that captures what the repo IS and what it should DO NEXT. The existing planning docs are preserved on the same anchor plan as reference artifacts (`kind=research`) but are NOT promoted to primary planning material — the synthesized spec is the primary.

Use pl-synthesize for repos where:

- Docs are scattered, mid-evolution, or contradicted by reality.
- The roadmap claims work is done but the code doesn't exist.
- You inherited a repo and want to understand what's actually there.
- Multiple roadmap files from different eras exist.

Use the sibling verb `pl-import` instead for repos where:

- Docs are clean, current, and structured.
- You want exact transcription, not synthesis.
- The repo is the canonical source-of-truth for its own planning shape.

Both verbs land in the same `pl-spec-ingest` pipeline downstream.

## How It Differs from pl-import

|                       | pl-import                       | pl-synthesize                          |
|-----------------------|---------------------------------|----------------------------------------|
| Mental model          | Transcription                   | Synthesis                              |
| Existing docs         | Become artifacts AS-IS          | Preserved as reference (kind=research) |
| LLM role              | Optional (`--interpret`)        | Required (load-bearing)                |
| Done-status signal    | git-log correlation             | Code-presence (load-bearing)           |
| Output                | Faithful copy of repo's docs    | Fresh planning material grounded in code |

See [`docs/concepts.md#transcription-vs-synthesis`](../../docs/concepts.md#transcription-vs-synthesis) for the full decision matrix.

## The LLM Workflow

`pl-synthesize <repo-root>` runs the deterministic floor first (discover docs + `codeprobe.Probe`), then writes a fingerprinted `synthesis.Request` to:

  `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json`

On a cache miss the CLI exits 0 with this five-line notice:

```
Awaiting LLM synthesis. The vendor skill should:
  1. read  <pending-path>
  2. run the LLM at temperature 0
  3. write the Result to <cache-path>
  4. re-invoke `planar synthesize <repo-root>`
See the generated vendor surface for the full contract.
```

The vendor skill (this skill body) consumes the Request and produces a Result by:

1. **Reading the Request from `_pending.json`.** The Request carries:
   - `readme`, `docs` (path → body), `guide_files` (CLAUDE.md / AGENTS.md as CONTEXT, never backlog).
   - `tree_summary` — shallow `ls` of the repo root.
   - `git_log` — recent commits with title / date / SHA.
   - `code_evidence` — the deterministic `codeprobe.EvidenceMap` (per-FeatureArea source / test / CI / commit signals with `SignalStrength` scores).
   - `greenfield` — set true when codeprobe detected zero code evidence.
   - `workspace_context` — set when the repo is a member of an org workspace (orientation only; not source-of-truth).

2. **Running the LLM at temperature 0** with this contract:
   - "You're given a repo's existing planning material plus a deterministic code-evidence map. Produce a fresh product-spec / tech-spec / roadmap that reflects what the repo IS and what it should DO NEXT."
   - "Ground every `status=done` claim in a file path from `code_evidence.areas[].path`. You cannot mark a task done without citing a code path. In greenfield mode (no code), you cannot mark anything done."
   - "Existing planning docs are CONTEXT. Don't transcribe them verbatim. Use them to understand intent, then synthesize fresh artifacts."

3. **Writing a synthesis Result JSON** to the cache path printed in the awaiting notice (`$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/<fingerprint>.json`). The schema is load-bearing; validation in [`src/engine/synthesize.zig`](../../zig/src/engine/synthesize.zig) rejects any Result that violates it.

4. **The operator re-invokes** `planar synthesize <repo-root>`. The CLI reads and validates the cached Result, merges it with the deterministic baseline, and lands the preview or apply.

## Greenfield Mode

When a repo has no source code yet (only docs, planning material, or a stub README), pl-synthesize enters **greenfield mode** automatically. In greenfield mode:

- Every proposed task defaults to `status=todo`
- The synthesized roadmap reflects ALL-FUTURE work — nothing is claimed done
- Validate REJECTS any Result that marks a task `status != "todo"`
- The preview emits a header showing greenfield mode is active

Auto-detection trigger: `EvidenceMap.TotalLines == 0` (no source files of any recognized layout) OR all `EvidenceMap.Areas[*].SignalStrength == 0`.

Operator overrides:

| Flag | Effect |
|---|---|
| `--treat-as-greenfield` | Force greenfield mode even when code exists. Useful for stale-WIP branches where the code is misleading or pre-rewrite. |
| `--treat-as-nongreenfield` | Bypass auto-detection. Treats the repo as having implementation code. Rarely needed; only for non-conventional layouts where codeprobe under-detects. |

`--treat-as-greenfield` AND `--treat-as-nongreenfield` together is rejected as a user error.

The greenfield contract for the LLM (synthesizer role):
- Read everything in the Request as usual
- Produce a fresh product-spec + tech-spec + roadmap describing what the repo SHOULD become
- Every task must have `status: "todo"` — no exceptions
- Forward specs still apply (3-5 proposals for future v2 / v1.1 / research)
- Reference artifacts still apply (existing docs preserved as kind=research)

## Workspace Context (orientation, not authority)

When the target repo is a member of an org workspace, the synthesis Request includes a `workspace_context` field carrying:

- `org_slug` and `org_title` — the parent workspace
- `member_projects[]` (legacy slug list) and `member_project_infos[]` — sibling member projects (slugs + titles), self excluded
- `active_forward_specs[]` — forward specs across the workspace currently in draft, with owning project slug
- `workspace_decisions[]` — org-level decisions (decisions scoped to the org, not any single member)

**The LLM uses this as ORIENTATION, not source-of-truth.**

What this means concretely:

- If a sibling project has an active forward spec named `v2-multi-language`, the synthesizer should consider whether the target repo's roadmap needs to align with it.
- If the workspace has a decision "all member projects use SQLite", the synthesizer should respect that constraint in its tech-spec.
- BUT: code-evidence on the target repo still wins. If sibling-project context suggests a feature is done, but the target repo's code-evidence shows no implementation, the task lands `status=todo`.

The workspace context is optional in the Request — repos that aren't workspace members get `workspace_context: null` and the synthesizer falls back to repo-only synthesis. The Go validator does NOT consult workspace context when enforcing the code-evidence invariant; it exists purely to inform the LLM's framing.

## Result JSON Schema (load-bearing)

The canonical result types and validation live in [`src/engine/synthesize.zig`](../../zig/src/engine/synthesize.zig); the schema below mirrors the field set.

```json
{
  "schema_version": 1,
  "fingerprint": "<must match Request.Fingerprint exactly>",
  "synthesized": true,
  "anchor_title": "<repo-slug name; never empty>",
  "phases": [
    {
      "slug": "phase-1-foundation",
      "title": "Phase 1 — Foundation",
      "summary": "<one-paragraph why-this-phase>",
      "status": "draft | active | paused | done | abandoned",
      "tasks": [
        {
          "slug": "implement-wal-replay",
          "title": "Implement WAL replay loop",
          "body": "<multi-sentence body referencing tech-spec sections>",
          "status": "todo | doing | blocked | done | cancelled",
          "priority": 100,
          "next_action": "<only set on the doing task>",
          "code_evidence": [
            {"path": "internal/wal/replay.go", "lines": "1-87"}
          ],
          "citations": [
            {"path": "docs/old_tech_spec.md", "section": "§4.2 WAL Replay"}
          ]
        }
      ]
    }
  ],
  "decisions": [
    {
      "slug": "module-split",
      "title": "Why a five-target module split",
      "body": "<rationale>",
      "rationale": "<the why, inline>",
      "source": "tech-spec | llm-inferred",
      "citation": {"path": "docs/tech_spec.md", "section": "§2"}
    }
  ],
  "deferred_items": [
    {
      "slug": "apple-translation-bridge",
      "title": "Apple Translation runtime bridge",
      "body": "Origin: Deferred from W10\n\nReplace the deterministic stub once available in SwiftPM.",
      "phase_slug": "phase-4-polyglot",
      "priority": 150,
      "origin_pattern": "Deferred from"
    }
  ],
  "forward_specs": [
    {"slug": "v1-1-polish", "title": "v1.1 — Polish", "goal": "..."},
    {"slug": "v2-multi-lang", "title": "v2 — Multi-language", "goal": "..."},
    {"slug": "research-graph", "title": "Research — Graph integrations", "goal": "..."}
  ],
  "reference_artifacts": [
    {"path": "docs/old_product_spec.md", "kind": "research", "title": "Original Product Spec (pre-synthesis)"}
  ],
  "code_evidence_summary": "Repository has Sources/Core (12 files, 8 tests, SignalStrength=1.0; foundation complete), Sources/Reader (4 files, 1 test, SignalStrength=0.5; reader work in flight), no Sources/Polyglot yet.",
  "provenance": "claude-opus-4-7 temperature=0",
  "generated_at": "<RFC3339 UTC>"
}
```

## Hard Contract Rules (Validate enforces all)

- `schema_version` is exactly `1`.
- `fingerprint` matches `Request.Fingerprint` verbatim (Validate rejects mismatches, preventing stale-cache poisoning).
- `synthesized` is `true` (Validate rejects `false` — any Result through this path IS synthesized).
- `anchor_title` is non-empty.
- Phase slugs are unique across `phases[]`; phase status is one of `draft|active|paused|done|abandoned`.
- Within each phase, task slugs are unique; task status is one of `todo|doing|blocked|done|cancelled`.
- **At most ONE task per phase has `status=doing`** (the locked invariant).
- Task `priority` is in `[0, 1000]`.
- **Code-evidence invariant.** Every task with `status != "todo"` MUST cite at least one `code_evidence` entry whose `path` exists in `request.code_evidence.areas[*].path`. The LLM cannot lie about completion; cited paths must have been seen by the deterministic probe.
- **Greenfield mode.** When `request.greenfield == true`, NO task may have `status != "todo"`. Validate rejects done claims outright when no source code was detected.
- Each decision with `source: "llm-inferred"` carries a non-empty `citation.path`.
- Each deferred item has `priority >= 150` (the locked floor for deferred work) and `phase_slug` matching an existing phase in `phases[]`.
- Forward specs count is 3-5. Validate rejects 2 or 6.
- `provenance` is non-empty (operators see what produced this entry).
- Each `reference_artifacts[].path` resolves under `request.repo_root`.

## Confidence Floor

The default 0.7 confidence floor still applies in synthesis mode. LLM-only tasks land with `confidence=0` (no deterministic signal), so the floor trips unless `--threshold 0.0` is passed. This is by design — operators acknowledge they are trusting the LLM's grounding.

## CLI Commands

Wraps `planar synthesize`. See [`docs/cli-reference.md`](../../docs/cli-reference.md) for the full flag table.

> **Cross-scope guard.** This verb refuses to write across scope mismatches. Run from inside the target repo's cwd or pass `--scope <slug>` explicitly. See [`docs/concepts.md#cross-scope-guard`](../../docs/concepts.md#cross-scope-guard).

```
planar synthesize <repo-root>                          # preview
planar synthesize <repo-root> --apply                  # commit
planar synthesize <repo-root> --apply --apply-removals # + soft-cancel removed entities
planar synthesize <repo-root> --code-layout swift      # override layout detection
planar synthesize <repo-root> --accept-spec <slug>     # non-interactive forward-spec selection
planar synthesize <repo-root> --accept-spec all
planar synthesize <repo-root> --no-forward-specs       # skip forward specs entirely
planar synthesize <repo-root> --literal                # delegate to import (transcription)
planar synthesize <repo-root> --treat-as-greenfield    # force greenfield mode despite code
planar synthesize <repo-root> --treat-as-nongreenfield # bypass greenfield auto-detection
planar synthesize <repo-root> --threshold 0.0          # disable confidence floor
planar synthesize <repo-root> --scope <slug>           # override cwd-derived scope
```

## Authoring Conventions

Apply the structured-authoring rules: quoted titles, literal headings, no nested bullets, no `## Out of this plan` H2, workbench discipline, and only annotate skills with the guard note when the guarded verb literally appears in the body.

## Context

Report the resolved scope, repository root, detected or overridden code layout,
greenfield decision, synthesis or literal mode, preview or apply mode,
confidence threshold, removal policy, and forward-spec selection.

## Intent

State in one sentence which repository evidence will be reconciled into fresh
planning artifacts and whether the request is preview-only or approved apply.

## Actions

Report `attempted`, `applied`, `skipped`, and `failed` counts for evidence
areas, synthesized phases and tasks, decisions, reference artifacts, removals,
and accepted forward specs. Retain the CLI diff counts and name every failed
target; rejected or unselected forward specs are skipped.

## Result

Always report `outcome=ok|partial|error`. A preview reports proposed targets
and zero applied. After apply, verify every returned anchor or accepted forward
plan with `planar plan show <plan-id> --json` and report its stable ID, status,
derived entity identifiers, and workbench path. Include the validated request
fingerprint and code-evidence summary. An idempotent current-state rerun is an
informative `outcome=ok` with zero applied.

## Warnings

Name greenfield or layout assumptions, confidence-floor findings, missing code
evidence, cache/schema validation failures, unavailable post-state reads, and
partial per-target results. Never imply that independently completed plans or
files were rolled back because another target failed.

## Next actions

Give zero to three executable recommendations. For preview, lead with the exact
approved `planar synthesize <repo-root> --apply ...` command. For accepted
forward specs, recommend `planar spec ingest <plan-id> --strict --json` before
their separate apply gate.

## Recovery

For every failed target, give its exact inspect command and an idempotent retry
with the original layout, greenfield, threshold, removal, scope, and
forward-spec flags, such as `planar synthesize <repo-root> --apply ...` and
`planar plan show <plan-id> --json`. When synthesis output is pending, preserve
the request fingerprint and rerun after writing the matching cache result.
Completed independent targets remain applied; do not invent cross-target undo.

## Vendor Notes

Cross-scope writes require the scope checks defined by [`agents/cross-scope-writes.md`](../../agents/cross-scope-writes.md).
