---
name: synthesizer
description: Produces fresh planning artifacts for a repo from existing docs + git log + source code via an LLM pass. A generator, not a translator — synthesizes what the repo SHOULD be rather than transcribing what existing docs claim.
tier: large
role: synthesizer
---

# Synthesizer

The **synthesizer** is the LLM-driven role that produces fresh planning artifacts for a repo from its existing material plus a deterministic code-evidence map. It is the sibling role of the `importer` (transcription); both land in the same `pl-spec-ingest` pipeline downstream.

Vendor-neutral. Vendor-specific surfaces are under `commands/claude/pl-synthesize.md`, `skills/codex/pl-synthesize.md`, and `skills/copilot/pl-synthesize.md`.

## Tier

`large`. Resolved to a concrete model per [`agents/models.md`](models.md). Mapping the union of an existing repo's planning material, its git log, and its source tree onto a coherent fresh spec — while honoring the code-evidence invariant — requires the same level of judgment as planning and orchestration.

## When to use

- A repo's planning material is scattered, mid-evolution, or contradicted by reality.
- The roadmap claims work is done but the source tree doesn't back the claim.
- You inherited a repo and need to understand what's actually there before pl-spec-ingest.
- Multiple roadmaps from different eras coexist and no single doc is canonical.

Do **not** invoke this agent when the repo's planning material is clean, current, and structured — that's the `importer` role (`/pl-import`).

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
- **Guide files** (`CLAUDE.md`, `AGENTS.md`, `copilot/*.md`) — CONTEXT only; never mined for backlog.
- **Workspace context** — set when the repo is a member of an org workspace (orientation only, not source-of-truth).
- **Greenfield flag** — set when codeprobe reports zero source-file evidence.

## Outputs

- **Synthesized product_spec / tech_spec / roadmap** as primary planning artifacts (`Source: pl-synthesize`).
- **Reference artifacts** — the existing planning docs preserved on the same anchor plan as `kind=research`, superseded but not deleted.
- **Phase decomposition** with per-phase status and one-paragraph summaries.
- **Tasks** with rich bodies, per-task code-evidence citations (required when `status != "todo"`), and optional doc citations.
- **Decisions** with rationale: `source=tech-spec` (extracted) or `source=llm-inferred` (synthesized, citation required).
- **Deferred items** at priority ≥ 150, each tagged with the phase they were deferred from.
- **Forward-spec proposals** — 3–5 follow-on specs the operator can accept or skip.
- **Code-evidence summary** — short prose echoing what the LLM saw in the EvidenceMap so the operator preview shows why the LLM said what it said.

## Sequencing

1. Operator runs `planar synthesize <repo-root>`.
2. Go side runs the deterministic floor: `adopter.Discover` + `adopter.ParseCorpus` + `codeprobe.Probe`.
3. Go side builds a fingerprinted `synthesis.Request` and writes it to `$PLANAR_HOME/cache/bootstrap-synthesis/<repo-slug>/_pending.json`.
4. On a cache miss, Go exits 0 with the five-line "Awaiting LLM synthesis" notice naming the pending and target paths.
5. The vendor skill (this role) reads the Request from `_pending.json`.
6. The skill runs the LLM at temperature 0 with the synthesis prompt: produce fresh planning material, ground done-status claims in `code_evidence.areas[].path`, treat existing docs as CONTEXT not transcription source.
7. The skill writes a `synthesis.Result` JSON to `<cache-dir>/<fingerprint>.json` matching the schema in [`src/internal/bootstrap/synthesis/result.go`](../src/internal/bootstrap/synthesis/result.go).
8. Operator re-invokes `planar synthesize <repo-root>`.
9. Go side reads the cached Result, runs `synthesis.Validate` (hard reject on any issue), adapts to `interpretation.Result`, and merges with the deterministic baseline. Reference artifacts and synthesized planning bodies are injected by `appendSynthesisArtifacts`.
10. Operator reviews the preview; `--apply` commits.

## Hard contract rules

The synthesizer MUST honor (Validate enforces every one — see [`src/internal/bootstrap/synthesis/validate.go`](../src/internal/bootstrap/synthesis/validate.go)):

- `schema_version: 1`, `fingerprint` matches `Request.Fingerprint`, `synthesized: true`.
- `anchor_title` non-empty; phase slugs unique; task slugs unique within a phase.
- Phase status in `{draft, active, paused, done, abandoned}`; task status in `{todo, doing, blocked, done, cancelled}`.
- At most ONE task per phase with `status=doing`.
- Task priority in `[0, 1000]`.
- **Code-evidence invariant.** Every task with `status != "todo"` cites at least one `code_evidence` path that exists in `request.code_evidence.areas[*].path`. The LLM cannot lie about completion; cited paths must have been seen by the deterministic probe.
- **Greenfield mode.** When `request.greenfield == true`, no task may have `status != "todo"`.
- llm-inferred decisions carry a non-empty `citation.path`.
- Deferred items have `priority ≥ 150` and `phase_slug` matching an existing phase.
- Forward specs count is 3–5 (preserved from plan 179 M6).
- `provenance` is non-empty.
- Every `reference_artifacts[].path` resolves under `request.repo_root`.

## Out of scope

- **No external-system contact.** The synthesizer does not call Jira, GitHub, or any operational-plane adapter. Propagation belongs to `/pl-ext-propagate`.
- **No automatic `/pl-spec-ingest`.** Accepted forward specs are created in `status=draft`; the operator decides when to run `/pl-spec-ingest <plan-id>` on each.
- **No source-code rewrites.** The synthesizer reads source for evidence only; it never edits source files.
- **No real LLM calls in Go.** The Go binary stays free of provider API keys, retries, and rate limits; the vendor skill is the LLM engine.
- **No verbatim transcription.** That's the `importer` role; `--literal` on `planar synthesize` delegates to `planar import`.

## Decisions

- **Two verbs, not one.** Importer (transcription) and synthesizer (synthesis) are kept distinct because the contracts differ — the synthesizer's code-evidence invariant has no analog in importer.
- **Code presence beats text claims.** A roadmap line that says "M3 is finished" is treated as `todo` unless source files corroborate. The greenfield case collapses naturally onto all-todo output.
- **Reference artifacts use `kind=research`, not a new kind.** Existing kind avoids a schema migration and keeps the original docs queryable as input material.
- **Workspace context is orientation, not source-of-truth.** Synthesis stays repo-scoped; org-level signals only widen the anchor title and summary.
- **Forward specs are 3–5.** Preserved from the locked plan 179 M6 decision; Validate enforces the range.
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
planar synthesize <repo-root> --threshold 0.0
planar synthesize <repo-root> --literal
planar synthesize <repo-root> --scope assoc:<slug> --apply
```
