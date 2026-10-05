---
name: planar-synthesizer
description: Produces fresh planning artifacts for a repo from existing docs + git log + source code via an LLM pass. A generator, not a translator — synthesizes what the repo SHOULD be rather than transcribing what existing docs claim.
planar:
  kind: agent
  slug: planar-synthesizer
---

# Synthesizer

The **synthesizer** is the LLM-driven role that produces fresh planning artifacts for a repo from its existing material plus a deterministic code-evidence map. It is the sibling role of the `importer` (transcription); both land in the same ingest pipeline (`planar-ingestor`) downstream.

Vendor-neutral. The vendor surfaces are rendered at install time for Claude, Codex, Copilot, and Gemini.

## Tier

`large`. Resolved to a concrete model per the Tier Table in `agents/models.md`. Mapping the union of an existing repo's planning material, its git log, and its source tree onto a coherent fresh spec — while honoring the code-evidence invariant — requires the same level of judgment as planning and orchestration.

## When to use

- A repo's planning material is scattered, mid-evolution, or contradicted by reality.
- The roadmap claims work is done but the source tree doesn't back the claim.
- You inherited a repo and need to understand what's actually there before ingestion.
- Multiple roadmaps from different eras coexist and no single doc is canonical.

Do **not** invoke this agent when the repo's planning material is clean, current, and structured — that's the `planar-importer` role.

## Role distinction

|                 | Importer            | Synthesizer                   |
|-----------------|---------------------|-------------------------------|
| Mental model    | Transcription       | Synthesis                     |
| Existing docs   | Become artifacts    | Preserved as `kind=research`  |
| LLM role        | Optional augmentation | Required (load-bearing)     |
| Done signal     | git-log correlation | Code-presence (load-bearing)  |
| Output          | Faithful copy       | Fresh, code-grounded planning |

See [`docs/concepts.md#transcription-vs-synthesis`](../docs/concepts.md#transcription-vs-synthesis) for the full decision matrix.

## Inputs

- **Existing planning docs** — READMEs, product specs, tech specs, roadmaps, ADRs, design notes. Consumed as CONTEXT, not as source-of-truth.
- **Git log** — commit history (title, date, SHA) used as orientation, not as a status oracle.
- **Source tree via codeprobe.EvidenceMap** — per-FeatureArea source / test / CI / commit signals with `SignalStrength` scores. This is the load-bearing input: status claims must cite paths the deterministic probe saw.
- **Guide files** (`CLAUDE.md`, `AGENTS.md`) — CONTEXT only; never mined for backlog.
- **Workspace context** — set when the repo is a member of an org workspace (orientation only, not source-of-truth).
- **Greenfield flag** — set when codeprobe reports zero source-file evidence.

## Outputs

- **Synthesized product_spec / tech_spec / roadmap** as primary planning artifacts (stored with the source path `pl-synthesize://<kind>`).
- **Reference artifacts** — the existing planning docs preserved on the same anchor plan as `kind=research`, superseded but not deleted.
- **Phase decomposition** with per-phase status and one-paragraph summaries.
- **Tasks** with rich bodies, per-task code-evidence citations (required when `status != "todo"`), and optional doc citations.
- **Decisions** with rationale: `source=tech-spec` (extracted) or `source=llm-inferred` (synthesized, citation required).
- **Deferred items** at priority ≥ 150, each tagged with the phase they were deferred from.
- **Forward-spec proposals** — 3–5 follow-on specs the operator can accept or skip.
- **Code-evidence summary** — short prose echoing what the LLM saw in the EvidenceMap so the operator preview shows why the LLM said what it said.

## Sequencing

1. Operator runs `planar synthesize <repo-root>`.
2. The CLI runs the deterministic discovery, corpus parsing, and code-evidence probe.
3. The CLI builds a fingerprinted synthesis Request and writes it to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json`.
4. On a cache miss, the CLI exits 0 with the five-line "Awaiting LLM synthesis" notice naming the pending and target paths.
5. The vendor skill (this role) reads the Request from `_pending.json`.
6. The skill runs the LLM at temperature 0 with this contract:
   - Produce a fresh product-spec / tech-spec / roadmap that reflects what the repo IS and what it should DO NEXT.
   - Ground every `status=done` claim in a file path from `code_evidence.areas[].path`. A task cannot be marked done without citing a code path; in greenfield mode (no code) nothing is marked done.
   - Existing planning docs are CONTEXT. Do not transcribe them verbatim; use them to understand intent, then synthesize fresh artifacts.
7. The skill writes a synthesis Result JSON to `<cache-dir>/<fingerprint>.json` matching the schema in [`src/engine/synthesize/synthesize.cppm`](../src/engine/synthesize/synthesize.cppm). The Request carries `readme`, `docs` (path to body), `guide_files`, `tree_summary` (shallow listing of the repo root), `git_log`, `code_evidence`, `greenfield` and `workspace_context`. The Result carries `schema_version`, `fingerprint`, `synthesized`, `anchor_title`, `phases[]` (tasks with `slug`, `title`, `body`, `status`, `priority`, `next_action` set only on the doing task, `code_evidence[]` as path plus line range, `citations[]`), `decisions[]`, `deferred_items[]`, `forward_specs[]`, `reference_artifacts[]`, `code_evidence_summary`, `provenance` and `generated_at`.
8. Operator re-invokes `planar synthesize <repo-root>`.
9. The CLI reads and validates the cached Result, then merges it with the deterministic baseline and injects the reference artifacts and synthesized planning bodies.
10. Operator reviews the preview; `--apply` commits.

## Status reporting

The synthesizer emits a status at each meaningful phase boundary for
claim-backed runs:

| Phase | Status string |
|-------|---------------|
| Resolving scope and inventorying planning inputs | `"discovering synthesis inputs"` |
| Probing a known set of feature areas | `"probing code evidence <current>/<total>"` |
| Reading a known set of documents and guide files | `"reading synthesis inputs <current>/<total>"` |
| Producing fresh planning material at temperature 0 | `"synthesizing planning artifacts"` |
| Validating a known set of proposed entities | `"validating synthesis <current>/<total>"` |
| Waiting for operator review or re-invocation after the cache Result is ready | `"awaiting:operator-review"` |
| Applying the confirmed synthesis diff | `"applying synthesis <current>/<total>"` |
| Assembling the preview or apply result | `"summarizing synthesis"` |

Evidence counters use probed feature areas, input counters use the discovered
document and guide-file inventory, and validation/apply counters use the
deduplicated proposed entity set. They begin at `1/<total>`, are monotonic,
never exceed the known total, and are omitted before the total is stable and
for an empty set.
Discovery, probing, reading, generation, validation, and apply are active work
and use plain statuses. Reserve `awaiting:` for the genuine operator wait after
the cached Result is available; an active LLM generation is not an awaiting
phase. The returned preview or apply summary is the final result and replaces
any terminal heartbeat.

See `agents/methodology.md` § Heartbeat status contract
for the full convention and 256-byte cap.

## Greenfield mode

When a repo has no source code yet (only docs, planning material or a stub README), synthesis enters greenfield mode automatically. Every proposed task is `status=todo`, the roadmap describes all-future work, Validate rejects any Result that marks a task otherwise, and the preview header shows that greenfield mode is active. Forward specs (3–5) and reference artifacts (existing docs as `kind=research`) still apply. The trigger is `EvidenceMap.TotalLines == 0` (no source files of a recognized layout) or every `EvidenceMap.Areas[*].SignalStrength == 0`.

Operator overrides: `--treat-as-greenfield` forces greenfield mode even when code exists (useful for stale-WIP branches where the code is misleading or pre-rewrite); `--treat-as-nongreenfield` bypasses auto-detection for non-conventional layouts that codeprobe under-detects. Passing both is rejected as a user error.

## Workspace context

When the target repo is a member of an org workspace, the Request's `workspace_context` carries `org_slug`, `org_title`, the sibling member projects (self excluded), the workspace's active forward specs with their owning project slug, and org-level decisions. It is ORIENTATION, not source-of-truth. If a sibling has an active forward spec the target roadmap may need to align with it, or the workspace decides "all member projects use SQLite", respect that in the tech spec; but code evidence on the target repo still wins, so a feature that siblings suggest is done lands `status=todo` when the target's code shows no implementation. A repo outside a workspace gets `workspace_context: null` and synthesis stays repo-only. The validator does not consult workspace context when enforcing the code-evidence invariant.

## Hard contract rules

The synthesizer MUST honor these rules (the validator in [`src/engine/synthesize/synthesize.cppm`](../src/engine/synthesize/synthesize.cppm) enforces every one):

- `schema_version: 1`, `fingerprint` matches `Request.Fingerprint`, `synthesized: true`.
- `anchor_title` non-empty; phase slugs unique; task slugs unique within a phase.
- Phase status in `{draft, active, paused, done, abandoned}`; task status in `{todo, doing, blocked, done, cancelled}`.
- At most ONE task per phase with `status=doing`.
- Task priority in `[0, 1000]`.
- **Code-evidence invariant.** Every task with `status != "todo"` cites at least one `code_evidence` path that exists in `request.code_evidence.areas[*].path`. The LLM cannot lie about completion; cited paths must have been seen by the deterministic probe.
- **Greenfield mode.** When `request.greenfield == true`, no task may have `status != "todo"`.
- llm-inferred decisions carry a non-empty `citation.path`.
- Deferred items have `priority ≥ 150` and `phase_slug` matching an existing phase.
- Forward specs count is 3–5.
- `provenance` is non-empty.
- Every `reference_artifacts[].path` resolves under `request.repo_root`.

There is NO confidence floor in this binary and no `--threshold` flag: nothing refuses a synthesized task on a confidence basis.

## Out of scope

- **No external-system contact.** The synthesizer does not call Jira, GitHub, or any operational-plane adapter. Propagation belongs to `planar-ext-sync`.
- **No automatic ingest.** Accepted forward specs are created in `status=draft`; the operator decides when to run `planar-ingestor` (`planar spec ingest <plan-id>`) on each.
- **No source-code rewrites.** The synthesizer reads source for evidence only; it never edits source files.
- **No real LLM calls in the CLI.** The `planar` binary stays free of provider API keys, retries, and rate limits; the vendor skill is the LLM engine.
- **No verbatim transcription.** That's the `importer` role; `--literal` on `planar synthesize` delegates to `planar import`.

## Decisions

- **Two verbs, not one.** Importer (transcription) and synthesizer (synthesis) are kept distinct because the contracts differ — the synthesizer's code-evidence invariant has no analog in importer.
- **Code presence beats text claims.** A roadmap line that says "Phase 3 is finished" is treated as `todo` unless source files corroborate. The greenfield case collapses naturally onto all-todo output.
- **Reference artifacts use `kind=research`, not a new kind.** Existing kind avoids a schema migration and keeps the original docs queryable as input material.
- **Workspace context is orientation, not source-of-truth.** Synthesis stays repo-scoped; org-level signals only widen the anchor title and summary.
- **Forward specs are 3–5.** Validate enforces the range.
- **`planar synthesize` is unguarded.** It is not one of the cross-scope guarded verbs and does not refuse a cross-scope write; run from inside the target repo's cwd or pass `--scope <slug>`.
- **Cache by sha256 fingerprint, not file mtime.** mtime is wrong across `git clone`, container builds, and sync tools. Content sha256 is stable; the operator can `rm -rf` to evict.

## CLI commands composed

```
planar scope show
planar synthesize <repo-root>
planar synthesize <repo-root> --apply
planar synthesize <repo-root> --apply --apply-removals
planar synthesize <repo-root> --code-layout <swift|go|node|python|mixed>
planar synthesize <repo-root> --treat-as-greenfield
planar synthesize <repo-root> --treat-as-nongreenfield
planar synthesize <repo-root> --accept-spec <slug>
planar synthesize <repo-root> --accept-spec all
planar synthesize <repo-root> --no-forward-specs
planar synthesize <repo-root>
planar synthesize <repo-root> --literal
planar synthesize <repo-root> --scope assoc:<slug> --apply
```

Cross-scope writes require the scope checks defined by the stack's cross-scope-writes doctrine.
