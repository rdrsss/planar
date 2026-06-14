# W10 — User-authored workflow surface (the strategic capability)

**Status:** spec · child of `00-anchor.md` · BUILD · **after `03` proves the pattern**
**Current home:** none — this is a new capability.
**Why it is the point:** migrating the ~10 known processes is the *near* goal.
The *far* goal is to make the workflow layer a first-class authoring surface, so
an LLM or operator can draft a **new** workflow on the fly, run it against the
real seam under a trace, and graduate the good ones into the shipped set. That is
what turns "we have some workflows" into "workflows are how processes are built."

This child specs the *authoring contract* — the guarantees that make on-the-fly
authoring safe and auditable — not the harness language (anchor D1).

---

## 1. The three non-negotiable invariants

A user-authored workflow is untrusted code. The contract makes it safe by
construction, not by review:

1. **No-handle invariant.** An authored workflow reaches Planar state ONLY by
   subprocess (`planar … --json`). It is never granted a SQLite handle, never
   imports a db/engine module. This is the same boundary every `planar-execute`
   planner module already carried in its docblock
   (`run-record-schema.md` §1) and is what keeps the harness — and any workflow
   in it — excisable. Enforced by the harness sandbox, not by trust.
2. **Trace-by-default.** Running any workflow mints a `run` and journals
   `run_events`. An experimental workflow is auditable from its first run: what
   it touched, what it decided, what it wrote — all in `run_events`, queryable by
   `planar-watch run show --json`. A workflow that suppresses its own trace is
   rejected.
3. **Capability-scoped seam.** A workflow composes the *primitive verb surface*
   only. It cannot invent a DB write or a repo-local scaffold (the standing rule
   for all skills/agents). Write verbs go through the strict scope resolver
   exactly as an operator's would — an authored workflow gets no scope-check
   bypass.

---

## 2. The authoring + graduation path

Mirror the existing skill/agent local-sandbox model (`planar local link`,
`pl-local-import`, CLAUDE.md "Operator-machine-local skills and agents"):

```
draft        →  sandbox (machine-local)        →  canonical (shipped)
LLM/operator    ~/.planar/local/workflows/         repo workflow set
writes a        run under trace; iterate;          copied in via normal
workflow        prefixed `local-` so it is         contribution flow;
against the     visibly user-authored             parity/usage gates apply
seam contract
```

- **Sandbox is never committed.** It is user-machine-local state, like local
  skills. An LLM experimenting writes here, runs it, reads the `run_events`
  trace, and either discards or promotes.
- **Promotion is manual.** Copy the workflow into the repo and follow the normal
  contribution flow — there is no auto-promote shortcut (same stance as
  `planar local promote` deliberately not existing).
- **Discovery + run** need verbs (see §4): list available workflows, run one,
  inspect its trace.

---

## 3. What makes a workflow "earn its keep" (graduation criteria)

A sandbox workflow graduates when, over real traced runs, it shows:
- **Determinism** — same inputs → same `run_events` sequence (the trace is the
  evidence; compare runs by `config_hash` per `run-record-schema.md`).
- **Composability** — it only ever shelled `planar`/`gh`/`git`, never reached
  for a handle or a scaffold (auditable from the trace + the source).
- **A real procedure** — it encodes a business process worth keeping, not a
  one-off. The friction it removes is visible in `pl-introspect` findings (W4) —
  closing the loop: introspection surfaces the friction, a workflow removes it.

---

## 4. Seam this capability needs (new)

| Need | Verb | Notes |
|------|------|-------|
| List workflows (shipped + sandbox) | `planar workflow list [--local] [--json]` | discovery |
| Show a workflow's contract/metadata | `planar workflow show <name> [--json]` | inputs, declared seam, version |
| Run one under a trace | (harness entrypoint) | mints `run`, journals events |
| Link a sandbox workflow into the harness | `planar local link` (extend existing) | `local-` prefix, machine-local |

These are thin registry/discovery verbs, not orchestration. The *running* is the
harness's job; Planar only catalogs and gates.

---

## 5. Milestones

1. **Author-contract doc + sandbox layout.** Pin the three invariants and the
   `~/.planar/local/workflows/` sandbox; extend `planar local link` to cover
   workflows. *Accept:* a hand-written sandbox workflow links, lists, and runs
   under a trace; its `run_events` are visible in `run show --json`.
2. **Discovery verbs.** `planar workflow list/show`. *Accept:* shipped + sandbox
   workflows enumerated; `--local` filters to user-authored.
3. **Graduation walk-through** in `docs/workflows.md` (a Recipe, like Recipe 14
   for local skills). *Accept:* the doc walks draft → sandbox → canonical with a
   real example.
4. **LLM-authoring guardrail test.** An authored workflow that attempts a forbidden
   move (reach for a handle, suppress trace, bypass scope) is rejected by the
   harness contract. *Accept:* each violation fails closed.

**Exit:** an LLM can draft a workflow, run it traced in the sandbox, and an
operator can graduate it — with the no-handle / trace / capability invariants
enforced, not trusted.

---

## 6. Open questions

- **OQ-1.** Sandbox-workflow capability scope — full operator seam, or a reduced
  allowlist until graduated? (Leaning: full seam but trace-mandatory and
  scope-checked, since the no-handle + scope-resolver invariants already bound
  the blast radius.)
- **OQ-2.** Does an authored workflow declare its expected seam up front (a
  manifest of verbs it will call) so the harness can vet it statically before
  the first run? (Leaning yes — a declared-seam manifest is the static analog of
  the trace's dynamic record, and it makes `workflow show` meaningful.)
- **OQ-3.** Relationship to `centurion` — is the sandbox a planar concept or a
  centurion concept? (Gate: this is downstream-harness territory; this spec only
  fixes the *contract* the sandbox must honor, per anchor D1.)
