---
slug: pl-ui-driver-hook
description: "Detection gate for the UI-verification harness: decide whether a claimed task has a design attached and the driver scripts are present, so the orchestrator knows whether to dispatch the ui-driver worker (Phase 3.6)."
source: agents/ui-driver.md
model_tier: small
vendor:
  claude:
    argument_hint: "<task-id[,task-id…]> [--plan <plan-id>]"
    invocation_examples: |
      /pl-ui-driver-hook 4821                 # one claimed task
      /pl-ui-driver-hook 4821,4822 --plan 91  # a cycle's claimed tasks
shared_notes:
  - "Read-only: never writes. The presence-gate and the query are both side-effect-free."
  - "Keeps design-detection out of core agent prose — the orchestrator invokes this skill, it does not inline the join."
---

# UI-driver hook ({{.VendorTitle}})

{{.VendorTitle}} detection gate for the UI-verification harness. The orchestrator invokes this in **Phase 3.6** (after the coder / test-coder report done, before the reviewer) to decide whether to dispatch the [`ui-driver`](../../agents/ui-driver.md) worker. It answers one question: *does a claimed task have a design attached, and can this repo actually drive it?*

Returns `{ dispatch_ui_driver: bool, design_artifacts: [{id, source_path}], reason }`.

## What you do

Run two checks, in order. **The presence-gate runs FIRST** — a target repo that has a registered `design_note` but lacks the driver scripts must cleanly no-op, never dispatch a worker that would then die on missing scripts.

### 1. Driver-presence gate (cross-repo safety — runs BEFORE the query)

The `ui-driver` worker shells the target repo's `scripts/ui-driver/run.mjs` + `scripts/ui-verify/synthesize.mjs` (present once Sill PR #469 is merged). If either is absent — a non-Sill target that happens to carry a `design_note` — return `dispatch_ui_driver: false` and STOP. Do not run the query.

```bash
worktree=$(planar plan show <plan-id> --json | jq -r '.workbench // .root_path')
if [[ ! -f "$worktree/scripts/ui-driver/run.mjs" || ! -f "$worktree/scripts/ui-verify/synthesize.mjs" ]]; then
  echo '{"dispatch_ui_driver": false, "design_artifacts": [], "reason": "driver scripts not present in target repo"}'
  exit 0
fi
```

### 2. Detection query

Only if the gate passed, ask whether any claimed task has a linked `design_note` (P3a registers these; the query enforces `from_kind='artifact'` so a polymorphic id collision cannot dispatch a phantom design):

```bash
planar ui-driver-query --task-ids <claimed_task_ids> --json
```

Its `{ dispatch_ui_driver, design_artifacts, reason }` output IS this skill's return value.

## Verdict routing (for the orchestrator)

- `dispatch_ui_driver: false` → **skip** Phase 3.6; log the `reason`.
- `dispatch_ui_driver: true` → dispatch the `ui-driver` role with a context capsule carrying each `design_artifacts[].source_path`'s **content** (the headless rule — never just the path), the surface (from the diff paths), and the scenario contract.

## What this hook does NOT do

- No writes, no dispatch — it only reports intent. The orchestrator dispatches.
- No re-implementation of the join in agent prose — the `ui-driver-query` verb owns it (and the `from_kind='artifact'` guard).
- No design content read here — content resolution happens at dispatch time (re-read `source_path`), keeping this gate cheap.

## Vendor Notes

{{.VendorNotes}}
{{- if .InvocationBlock}}

## Invocation

```
{{.InvocationBlock -}}
```
{{- end}}
