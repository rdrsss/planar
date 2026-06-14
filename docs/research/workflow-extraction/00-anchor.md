# Workflow Extraction — anchor spec (v0.1)

**Status:** design · anchor for the workflow-extraction initiative
**Serves:** moving multi-step *business processes* out of the four binaries and
the LLM agent/skill layer into composable, externally-authored workflows that
the planar binaries serve by subprocess.
**Companion of:** `docs/research/run-record-schema.md` (the trace substrate),
`closure-measurement-build-spec.md` (the protected-instrument invariant this
initiative must respect).
**Decomposition intent:** written for `pl-spec-ingest`. Each `NN-*.md` child in
this directory is one workflow → one child plan under this anchor on ingest.
Touches are pre-declared per child so the ingestor seeds `task_touch_paths`
directly (dogfooding: this initiative's own build is a corpus member).

---

## 1. The thesis: the seam, not the language

Determinism, traceability, and repeatability do **not** come from rewriting a
process in Lua. They come from three properties the process must have, and Lua
(or any harness) is only the third:

1. **Every step is an atomic CLI primitive verb** — `planar-agent pull`,
   `planar plan closeout`, `planar ext create` — so a step either
   happened-and-recorded or didn't. No half-states across a process boundary.
2. **Every step emits an append-only record** — the claim ledger plus the
   `run_events` journal (`migrations/00020_runs`, drafted). The trace *is* the
   journal, queryable by checked-in SQL, not reconstructed from an LLM
   transcript.
3. **A deterministic harness owns the sequencing** — Lua, a bare loop, a future
   host-native driver. Deliberately **excisable** (planar-spec-v0.1 §5 treats
   the harness as an independent toy). The harness holds **no SQLite handle**;
   it reaches the DB only by shelling `planar … --json`.

The investment that pays off is in the **seam** — primitive verbs, the schema,
named SQL — not in irreplaceable logic poured into the harness. This is the
same discipline `run-record-schema.md` already applies to metrics (named SQL
over raw tables, never a materialized `run_metrics` table). It also happens to
be the secondary goal of this initiative — sharper binaries, thinner Zig — seen
from the other end. The two are one goal.

**State of the world (2026-06-14):** the in-tree Lua harness (`planar-execute`)
is already excised; the downstream harness is `centurion`. The run-record rig
(`engine/runs/`, `planar run *`) is **not built yet** — `00020_runs` is drafted
and unlanded. This anchor assumes that rig lands first (it is the trace spine).

---

## 2. What counts as a candidate (the test)

A **workflow candidate** is a multi-step business process — a sequence of
conditional CLI/DB operations (loops, branches, idempotent skips, gates,
external calls) — that we want to be:

- **Composable** — built from primitive verbs, not a monolithic handler.
- **Quickly changeable** — editable without a binary rebuild + reinstall cycle.
- **LLM-authorable** — an LLM (or operator) can write a *new* one on the fly to
  experiment, run it against the real seam, and graduate the good ones into the
  shipped set ("user workflows" — see §6).

A process fails the test (stays in Zig / stays an LLM agent) if it is either an
**atomic primitive** (one verb, one transaction — must stay transactional in
Zig) or **irreducible judgment** (writing code, reviewing, drafting prose).

---

## 3. Candidate inventory

Consolidated from the four-binary + agent + skill audit. Recommendation legend:
**EXTRACT** = move sequencing to a workflow; **PARTIAL** = extract the loop,
keep an atomic primitive in Zig; **KEEP-ZIG** = atomic/transactional, stays;
**KEEP-LLM** = irreducible judgment, stays an agent.

| # | Workflow | Current home | Kind | Determinism | Rec | Child spec |
|---|----------|--------------|------|-------------|-----|-----------|
| W1 | Orchestrator dispatch loop | `agents/orchestrator.md` + claim ritual | agent-loop | ~95% | **EXTRACT** | `02` |
| W2 | Janitor merge-to-closeout | `agents/janitor.md` | agent-loop | ~100% | **EXTRACT** | `03` |
| W3 | ext propagate tree-walk | `handlers/ext/propagate.zig` (~1094) | zig-handler | ~70% | **PARTIAL** | `04` |
| W4 | introspect signal-mining | `engine/introspect.zig` (~1099) + `agents/introspector.md` | mixed | ~90% | **PARTIAL** | `05` |
| W5 | Workspace bootstrap pipeline | `handlers/workspace/{init,doctor,regenerate}.zig` (~920) | zig-handler | ~50% | **PARTIAL** | `06` (stub) |
| W6 | Sync reconcile loop | `handlers/sync/{pull,push,resolve}.zig` (~390) | zig-handler | ~55% | **PARTIAL** | `07` (stub) |
| W7 | import / synthesize / ingest pipeline | `engine/{import,synthesize}.zig`, `handlers/spec/ingest.zig` (~2000) | mixed | ~40% | **PARTIAL** | `08` (stub) |
| W8 | Recovery / reconcile (doctor) | `planar-agent reconcile` (~373) + `pl-doctor` | mixed | ~70% | **PARTIAL** | `09` (stub) |
| W9 | Read-composition reports | `pl-status`/`pl-health`/`pl-handoff`/`pl-resume` | skill | ~95% | **EXTRACT** | `10` |
| W10 | User-authored workflow surface | (new capability) | infra | n/a | **BUILD** | `11` |
| W0 | Seam hardening (enabling primitives) | (new verbs) | infra | n/a | **BUILD** | `01` |
| — | Coder / Reviewer / Test-coder / Planner / Spec-reviewer | `agents/*.md` | agent | 10–30% | **KEEP-LLM** | — |
| — | Atomic claim verbs (`pull`/`complete`/`fail`/`release`/`block`) | `engine/runtime/agentactivity/{atomic,store}.zig` | zig-core | n/a | **KEEP-ZIG** | — |
| — | Migrations / scope resolution / FK enforcement / run-record subsystem | `migrations/`, `engine/identity/`, `engine/runs/` | zig-core | n/a | **KEEP-ZIG** | — |

Borderline calls worth flagging: `plan closeout` (W2 dependency) and `plan next`
(W1 dependency) stay **KEEP-ZIG** — they are atomic gate/queue evaluations whose
correctness depends on a single DB snapshot; the workflow *calls* them, it does
not reimplement them. `reconcile` (W8) is the inverse: today it is one big sweep
verb, but the *recovery procedure around it* (diagnose → decide → sweep →
verify) is the workflow.

---

## 4. The three planes every workflow touches

A workflow composes against three distinct, already-existing (or drafted) planes.
Keeping them separate is what makes the harness excisable.

| Plane | Verbs | Owns | Status |
|-------|-------|------|--------|
| **Coordination** | `planar-agent run start/end`, `context add/capsule/resolve`, `claim-associate`, `pull/heartbeat/complete/fail/release/block` | atomic claim + run-scoped working memory | exists (migrations 00022–00024) |
| **Trace** | `planar run start/event/touch/harvest/finish` | append-only run journal; the deterministic-metric substrate | **drafted, unlanded** (`00020_runs`) |
| **Primitive state** | `planar … --json` reads + atomic write verbs | the plan/task/ext/workbench model | exists |

Every workflow MUST emit `planar run event` at each step boundary (decision,
dispatch, terminal). That is what converts "scripts orchestrate procedures" into
"scripts orchestrate procedures **and leave a queryable trace**." The trace
plane is shared with the closure-measurement experiment — a workflow run and a
benchmark run are recorded by the identical surface.

---

## 5. Seam gaps that gate extraction

Three candidates cannot be cleanly externalized today because the harness would
be forced to either hold logic it shouldn't or shell directly to git/sqlite.
These enabling primitives are specced in child `01` and block W3/W4/W8.

| Gap | Needed primitive | Blocks | Forced workaround today |
|-----|-----------------|--------|------------------------|
| Tree-walk is monolithic | `planar ext propagate-one --entity <k:id>` (render+create+link, one entity) and `planar plan descendants <id> --json` (topo order) | W3 | harness owns tree-walk + loses cross-entity atomicity |
| `sync_events` unqueryable | `planar-watch sync-events --json` (filterable per-row read) | W4 | coarse `planar report --json` rollup only, or illegal SQLite handle |
| reconcile is unscoped | `planar-agent reconcile --plan <id> --dry-run --json` | W8 | global sweep or manual `claims --json` + batch `release` |

W1 (orchestrator) and W2 (janitor) need **no** new coordination primitive — the
claim ritual seam is complete. W2's only external reach is to `git`/`gh`, which
is correct (those are not Planar's domain) but must be encapsulated in the
workflow, never in a binary.

---

## 6. The user-authored workflow surface (the strategic capability)

The point of this initiative is not only to migrate the ~10 known processes. It
is to make the workflow layer a **first-class authoring surface**: an LLM or
operator drafts a *new* workflow against the seam, runs it under a trace, and —
if it earns its keep — graduates it into the shipped set. This is the "user
workflows" capability and it is specced in child `11`. Its non-negotiables:

- **No-handle invariant** — an authored workflow reaches the DB only by
  subprocess; it cannot be granted a SQLite handle. Enforced by the harness
  contract, not by trust.
- **Trace-by-default** — running any workflow mints a `run` and journals
  `run_events`, so an experimental workflow is auditable from its first run.
- **Graduation path** — sandbox (machine-local, `local-` prefixed, never
  committed) → canonical (copied into the repo, normal contribution flow),
  mirroring the existing `planar local link` skill/agent graduation model.

---

## 7. Decomposition intent

Each child `NN-*.md` is one workflow. On `pl-spec-ingest` against this directory:
the anchor becomes the parent plan; each child becomes a child plan; each child's
milestone bullets become task rows; pre-declared touches seed `task_touch_paths`.
Full specs are written for the EXTRACT/BUILD candidates whose seam is ready
(`01`, `02`, `03`, `04`, `05`, `10`, `11`); the judgment-mixed PARTIAL candidates
ship as **STUBs** (`06`–`09`) following the `hypergraph-tech-spec.md` convention —
the decision is parked with its resolution gate stated, not pre-answered.

---

## 8. Decisions to lock before build (resolved defaults; override deliberately)

- **D1 — harness language is out of scope of *this* initiative.** We spec the
  *seam* and the *procedures*; whether the harness is Lua/centurion or another
  driver is downstream. Each child spec is harness-agnostic. *(Resolved.)*
- **D2 — the binary keeps the atomic verb; the workflow keeps the sequence.**
  No EXTRACT child deletes a transactional verb; it relocates the *decision
  flow* and may add thinner primitives. LOC reduction is a *side effect*, never
  the success criterion. *(Resolved.)*
- **D3 — trace is mandatory, not optional.** Every workflow emits `run event`.
  A workflow that cannot be traced is not done. *(Resolved — gated on `00020_runs`
  landing first.)*
- **D4 — agents shrink, they do not vanish.** EXTRACT of an agent-loop (W1/W2)
  leaves the judgment callouts (coder, reviewer) as agents; the `.md` surface
  loses the procedure, keeps the prompt. *(Resolved.)*
- **D5 — STUBs do not block the anchor.** `06`–`09` may remain stubs while
  `01`–`05`/`10`/`11` build; a stub's resolution gate is its entry criterion.
  *(Resolved.)*

Record each as a `planar decision` on the anchor plan at ingest.

---

## 9. Build order / critical path

```
00020_runs lands (trace spine, separate work)  ──┐
                                                  ▼
01 seam-hardening  ──►  03 janitor (poster child, lowest judgment)
                   ──►  02 orchestrator loop
                   ──►  04 ext propagate      (needs 01 gaps)
                   ──►  05 introspect         (needs 01 sync-events)
10 read-compositions (cheap, parallel, no new seam)
11 user-authoring-surface (after 03 proves the pattern end-to-end)
06–09 stubs: promote to specs as their resolution gates clear
```

**Start at `03` (janitor).** It is the cleanest 100%-deterministic procedure,
load-bearing in production, and its failure modes are already documented in the
operator's memory. Building it as a traced workflow proves the whole pattern —
procedure-in-harness + judgment-as-callout + trace-as-journal — against the
lowest-judgment target, exactly the M1-before-M3 logic the closure spec uses.
