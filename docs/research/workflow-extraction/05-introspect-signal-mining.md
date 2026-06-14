# W4 — introspect signal-mining workflow

**Status:** spec · child of `00-anchor.md` · PARTIAL · **depends on `01`/P2**
**Current home:** `engine/introspect.zig` (~1099 LOC) + `agents/introspector.md`
+ `pl-introspect`
**Determinism:** ~90% — read observability signals, match a closed taxonomy,
dedup, file findings. No prose generation.

The introspector mines the diagnostic bundle, the always-on observability
tables, and local vendor transcripts for friction patterns, then files
structured findings on a per-association feedback plan. The mining is a
deterministic signal→pattern→finding pipeline; the only reason it lives in a big
Zig module + an LLM agent is that the signals weren't independently queryable.
`01`/P2 (`planar-watch sync-events --json`) closes the last gap.

---

## 1. The procedure

```
Step 0 — Open run; resolve feedback plan (bootstrap if absent)
  planar plan list --scope <s> --json  → find|create feedback plan
  ‹trace: run open›

Step 1 — Pull signals (all reads, --json)
  planar report --days <n> --json                 # invocation aggregates + failure tail
  planar-watch actions --json ; claims --json     # per-row activity
  planar-watch sync-events --json                 # NEW (01/P2) — stuck propagations, strategy flips
  planar-watch run list/show --json               # workflow-run outcomes
  (+ local vendor transcripts via fs read)
  ‹trace: kind=signals, payload={counts}›

Step 2 — Classify against the closed taxonomy (deterministic)
  for each signal cluster → one of {failure-cluster, retry-pattern,
                                     abandoned-workflow, gap-feature}
  finding title = "<taxonomy-key>: <signal-key>"   (deterministic, dedup-able)
  ‹trace: kind=classified, payload={taxonomy, signal}›

Step 3 — Dedup + file findings
  planar question/decision list --plan <fb> --json → skip existing titles
  for each new finding: planar question add --plan <fb> ...   ‹trace: kind=filed›

Step 4 — Close run
  planar run finish ... ; ‹trace: run close, payload={filed, skipped}›
```

---

## 2. Judgment edge

The four finding types are matched by **mechanical pattern rules**, not LLM
judgment — `agents/introspector.md` describes a fixed taxonomy, not a reasoning
task. The one soft option is an *optional* enrichment pass that writes a
human-readable description for a filed finding; keep it a flagged callout (like
`pl-synthesize --enrich`), default off. The deterministic floor files findings
with taxonomy-keyed titles and needs no LLM.

---

## 3. What stays in Zig

The observability **tables and their capture** (migration 00020 `cli_invocations`,
`agent_actions`, `agent_work_claims`, `sync_events`, `workflow_runs`) and the
read verbs over them. The workflow only *reads* via `--json`. Finding *creation*
is the existing atomic `question add` / `decision add` verb.

---

## 4. Milestones

1. **Consume `01`/P2.** Replace the in-engine `sync_events` access with the
   `planar-watch sync-events --json` read; drive the classify→dedup→file
   pipeline from the workflow. *Accept:* mining a fixture DB files the same
   findings the engine module produces today, with identical taxonomy titles.
2. **Dedup idempotency.** *Accept:* a second run on the same signals files zero
   new findings (titles already present).
3. **Optional enrich callout.** *Accept:* `--enrich` adds a description without
   changing which findings are filed; default off path is LLM-free.

**Exit:** introspection is a traced, read-only mining workflow over independently
queryable signals; the heavy `engine/introspect.zig` thins to the
classification rules (or moves out entirely), and the agent becomes the optional
enrichment callout.

---

## 5. Open questions

- **OQ-1.** Does the friction taxonomy belong in the workflow (editable, fast) or
  stay a Zig constant (parity-checked)? Leaning workflow, since "quickly
  changeable taxonomy" is exactly the composability win. (Gate: after `01`/P2.)
- **OQ-2.** Transcript mining reads vendor files directly — keep that as a fs
  read in the workflow, or add a `planar` verb that surfaces transcript-derived
  signals so the no-handle/no-fs-coupling line stays clean? (Default: fs read in
  workflow is acceptable — transcripts are not Planar state.)
