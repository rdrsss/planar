---
slug: pl-ui-driver
description: "Drives a live preview through UI scenarios and returns a schema-conforming evidence manifest + gate verdict. Dispatched (Phase 3.6) when a claimed task has a linked design_note."
source: agents/ui-driver.md
model_tier: large
vendor:
  claude:
    argument_hint: "<task-id>"
    invocation_examples: |
      /pl-ui-driver <task-id>             # drive scenarios for one task's linked design_note
      /pl-ui-driver <task-id> --surface pro   # scope to the pro surface explicitly
shared_notes:
  - "Active scope is read at the start of every invocation; no vendor-specific state is kept outside the database."
  - "On return to the orchestrator, the session id and vendor are recorded on the snapshot."
---

# ui-driver ({{.VendorTitle}})

{{.VendorTitle}} skill surface for the vendor-neutral `ui-driver` agent. See [`agents/ui-driver.md`](../../agents/ui-driver.md) for the role spec and [`agents/methodology.md`](../../agents/methodology.md) for the iteration loop and Phase 3.6 dispatch rules.

## What the ui-driver does

1. **Read the brief's context capsule** — `{ worktree_path, surface (consumer|pro), design_note content, scenario contract }`. The design content is IN the brief (headless rule) — never just a path.
2. **Derive typed scenarios** — scenario contract: steps `{action,selector?,value?}`, expect `{kind,value}` with kind ∈ `testid-appears|network-2xx|snapshot-changed|text-matches`; scoped to the changed surface. A design affordance with no wiring is a scenario that must actually DO something.
3. **Start the preview and drive** via:
   `node scripts/ui-driver/run.mjs --scenarios s.json --surface <surface> --base-url <url> --out manifest.json`
4. **Classify fidelity** — diff-scoped, intent-vs-system 95/5; existing components/chrome staying as the system defines them is intentional-system-divergence, not a bug.
5. **Synthesize** — assemble `{ scenarios: manifest.scenarios, fidelity }` and run `node scripts/ui-verify/synthesize.mjs evidence.json` (exit 1 = gating FAIL).
6. **Return** the manifest + `{ verdict: 'fail'|'pass', markdown }`. Never re-implement the gate — `synthesize.mjs` owns it.

## What the ui-driver does NOT do

- No feature-code edits. Production-code changes belong to a coder cycle.
- No re-implementation of the gate logic. `synthesize.mjs` is the single gate authority.
- No driver runs against production — only local preview or CI preview URLs.
- No expansion outside the cited task's design_note scenarios.

## Verdict taxonomy

- **`pass`** — all scenarios pass fidelity threshold; gate exits zero.
- **`fail`** — one or more scenarios fail or `synthesize.mjs` exits non-zero; report names each failing scenario with the observed vs expected value.

## Status reporting

The ui-driver emits a status string at each meaningful phase boundary using `planar-agent heartbeat --claim <token> --status "<text>"`. The canonical transitions and their strings are:

| Phase | Status string |
|-------|---------------|
| Reading the brief and design_note | `"reading brief"` |
| Deriving scenarios from the design_note | `"deriving scenarios"` |
| Starting the preview | `"starting preview"` |
| Driving scenarios (`run.mjs`) | `"driving scenarios"` |
| Running `synthesize.mjs` | `"synthesizing"` |
| Producing the work-complete report | `"reporting"` |

Status strings use the `awaiting:` prefix when blocked on an external event. The cap on `--status` payload is 256 bytes. See [`agents/methodology.md` § Heartbeat status contract](../../agents/methodology.md#heartbeat-status-contract).

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
