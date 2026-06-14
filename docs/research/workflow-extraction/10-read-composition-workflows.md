# W9 — Read-composition workflows (the cheap proving ground)

**Status:** spec · child of `00-anchor.md` · EXTRACT · **no new seam**
**Current home:** `pl-status`, `pl-health`, `pl-handoff`, `pl-resume` (skills)
**Determinism:** ~95% — fixed compositions of read verbs an LLM currently
assembles by hand.

These are the lowest-stakes, highest-count candidates: a skill tells an LLM to
run three or four `--json` reads in a fixed order and format the result. There is
no judgment and no write (except handoff's snapshot). They are the ideal place to
*prove the harness ergonomics* — trace emission, `--json` parsing, output
shaping — before touching the load-bearing loops, and they pay off immediately by
removing LLM latency from routine status checks.

---

## 1. The compositions

| Workflow | Reads (all exist, `--json`) | Output |
|----------|------------------------------|--------|
| **status** | `scope show`, `plan list --status active`, `task list --status doing`, `question list --status open` | scope state summary |
| **health** | `planar health --json` (single verb; near-trivial) | health verdict |
| **resume** | `planar resume <task> --json` (8-section packet) + `resume validate` | resume-readiness packet |
| **handoff** | `planar handoff create` (write), `planar capture note`, then `resume validate` | validated handoff record |

`status` is the canonical example: today an LLM reads four endpoints and writes
prose; as a workflow it is `four reads → one template → one trace event`,
deterministic and instant.

---

## 2. What stays in Zig

Everything they read. `resume`'s packet assembly (`handlers/resume/cmd.zig`,
8-section build) and `handoff`'s snapshot+create stay atomic in Zig — they are
KEEP-ZIG per the anchor inventory because the packet must be internally
consistent from one snapshot. The workflow *invokes* `resume <task> --json`; it
does not reassemble the packet. So this child mostly migrates the *skill prose*
(the read sequence + formatting), not handler code.

---

## 3. Why bundle them

Individually trivial; bundled they prove the harness's read/format/trace path on
four real shapes with zero risk. Ship them first alongside `03` to de-risk the
ergonomics, not because they reduce much Zig.

---

## 4. Milestones

1. **status + health** as traced read-composition workflows. *Accept:* output
   matches the skills' documented shape; a `run event` records the read set.
2. **resume + handoff.** resume is read-only; handoff's single write
   (`handoff create`) is the atomic verb, the rest is read+format.
   *Accept:* handoff produces a validated record; resume packet round-trips.

**Exit:** four skills become four traced workflows; the harness read/format/trace
path is proven; the skill `.md` files shrink to a pointer at the workflow.

---

## 5. Open question

- **OQ-1.** Do trivial read-compositions even need a `run` (trace overhead for a
  status check)? Possibly a lightweight `--no-trace` mode for pure reads, or
  trace only writes. (Default: trace writes always; reads optional.)
