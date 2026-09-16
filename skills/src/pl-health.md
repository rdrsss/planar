---
description: Explain each Planar health contributor and route degraded state to an executable, read-first recovery path.
origin: docs/cli-reference.md#domain-health
shared_notes:
    - 'Health is read-only: report installed freshness but never repair, reconcile, resume, or edit configuration automatically.'
    - Use the Planar CLI for Planar state and scriptorium for installed-surface drift; never inspect or write the database or installed projections directly.
slug: pl-health
vendor:
    claude:
        argument_hint: '[--json]'
        invocation_examples: |
            /pl-health
            /pl-health --json
---

# Planar Health ({{.VendorTitle}})

Explain whether Planar is operational and whether in-flight work can be resumed.
This is a read-only orientation workflow. It names the contributors that affect
health and routes each degraded contributor to an exact inspection or recovery
command; it never applies the recovery itself.

## When To Invoke

At session start, in CI, after an interrupted agent run, or when another Planar
workflow reports degraded state, stale installed surfaces, or configuration
errors.

## Workflow

1. Run `planar health --json` and read the verdict from the `overall` FIELD,
   never from the exit code. `overall` is one of `ok`, `degraded`, `critical`.

   **Do not map the exit code to a health state.** `planar` returns exit 2 for
   every usage error — unknown flag, missing argument, bad value — so a
   mistyped invocation is indistinguishable from a critical system. Keying off
   the code makes the skill report CRITICAL for a typo.

   If the payload does not parse, that is a COMMAND failure, not a health
   finding: report it as such, name the exit code and the stderr, and stop.
   Do not fall back to the exit code for a verdict it cannot carry.
2. Explain only contributors that are unhealthy or useful for orientation:
   database reachability, schema currency, SQLite integrity, resumability,
   stale handoffs, and `projection_freshness`. Include the field value, why it
   affects `overall`, and the executable route below. Do not reproduce the raw
   JSON or print empty diagnostic trees.
3. Use `scriptorium status --config scriptorium.yaml --json` only when
   `projection_freshness` is not fresh or its evidence needs projection-level
   explanation. Planar retired its in-band projection tracking (plan 918);
   scriptorium owns it now. Report stale,
   missing, legacy, and unmanaged rows distinctly. Unmanaged rows remain usable
   and do not degrade health; unselected vendors are not missing.
4. If configuration is named by an error or contributor, inspect it through
   `planar config path`, `planar config validate`, and
   `planar config show --effective`. Do not read or edit the TOML directly.
5. Render the shared feedback contract below. No command in this workflow may
   mutate state.

## Contributor Routing

| Contributor | Explanation and executable route |
|---|---|
| `db_ok == false` | Critical: the database cannot be reached. Run `planar config validate`, then `planar config path`; after correcting the reported configuration or filesystem problem, retry `planar health --json`. |
| `schema_current == false` | The database schema does not match the running binary. Route to `pl-doctor`; inspect with `planar health --json` after the guided recovery. Do not write migrations directly. |
| `integrity_ok == false` | Critical SQLite integrity failure. Stop; route to `pl-doctor` for diagnosis and do not reconcile claims or repair projections first. |
| `not_resumable_tasks > 0` | One or more doing/blocked tasks lack a next action or context snapshot. Route to `planar audit handoff-readiness --json`, then `planar resume validate <task-id> --json` for each reported task; use `pl-resume` or `pl-doctor` for guided remediation. |
| `stale_handoffs > 0` | Pending or validated handoffs exceeded the freshness window. Route to `planar handoff list --status pending --json` and `planar handoff list --status validated --json`, then inspect with `planar handoff show <handoff-id> --json`; use `pl-doctor` before any abandon action. |
| expired claim evidence | Health may expose interrupted work without being the claim inventory. Preview reconciliation with `planar-agent reconcile --dry-run --json`; use `pl-doctor` for the operator-confirmed apply path. Never reconcile a live claim. |
| projection `stale` or `missing` | Managed installed output differs from or lacks its staged projection. Report the health result's `repair_command` when present — it is the full reinstall (`./install.sh --prefix <planar-home>`), which is the single recovery path since plan 918 D5 removed the per-name repair verb. For projection-level detail inspect with `scriptorium status --config scriptorium.yaml --json`, or preview drift read-only with `scriptorium check --config scriptorium.yaml --vendor <vendor> --json`. Repair is a separate, explicit action. |
| projection `legacy` | A Planar-owned installation has no current manifest. Report the single reinstall guidance emitted by health/status; do not fabricate one missing row per projection. |
| projection `unmanaged` | Informational only. It is operator-authored, remains usable, and must not be replaced or treated as degraded. |
| configuration failure | Run `planar config path`, `planar config validate`, and `planar config show --effective`; guide the operator to `planar config edit` only after validation identifies a configuration change. |

Health is global, while many task reads are cwd-scoped. If contributor counts
cannot be attributed from the current scope, say so and route to the global
`planar audit handoff-readiness --json` result or `pl-doctor`; do not claim a
quiet current scope explains a healthy global database.

## Output Contract

Use this shared operator-feedback envelope. Omit empty optional sections, but
always include `Result`. In requested JSON mode, return the same fields as
structured values rather than a separate narrative contract.

### Context

Report the global health target, resolved configuration path when inspected,
and read-only mode.

### Intent

One sentence: assess Planar health, explain unhealthy contributors, and select
safe next diagnostics.

### Actions

Report `attempted`, `applied`, `skipped`, and `failed`. For this read-only
workflow, `applied` counts successful reads; it never means writes.

### Result

Set `outcome=ok|partial|error`, report `overall`, and list only material
contributors with their counts or states. A healthy quiet installation should
be a short orientation result: one health line plus zero to three useful next
actions, with no empty trees, `none` placeholders, or fabricated alerts.

### Warnings

Report critical or degraded signals, partial reads, global-versus-scoped
attribution limits, legacy installs, and unmanaged projections when relevant.
Do not describe unmanaged projections as failures.

### Next actions

Give zero to three executable commands selected from actual contributors, in
severity order. Prefer the exact health-provided projection repair command,
`planar resume validate <task-id> --json`,
`planar-agent reconcile --dry-run --json`, or `planar config validate` as
applicable. For a healthy quiet result, useful optional orientation commands
are `planar dashboard --json` and `planar plan list --status active --json`.

### Recovery

On partial or error outcomes, give the exact idempotent failed read to retry.
For a critical contributor, give `planar health --json` as the post-recovery
recheck. Never claim a repair, reconciliation, resume, or configuration edit
occurred unless a separately authorized workflow performed and verified it.

## What It Does Not Do

- Does not repair installed projections; freshness is visible, never automatic.
- Does not reconcile or abort claims, abandon handoffs, or mutate tasks.
- Does not edit configuration, open SQLite directly, or infer state from files.
- Does not force `overall=ok`; legitimate active work may remain degraded.

## Context

Report that health is global, the resolved database/configuration, and text or
JSON mode. Do not imply that the cwd limits health contributors.

## Intent

State in one sentence that the run is a read-only check of database integrity,
schema currency, handoff readiness, and installed projection health.

## Actions

Report `attempted`, `applied=0`, `skipped`, and `failed` counts for the health
check and its unavailable contributors. Keep the contributor counts returned
by `planar health`; do not recast a degraded contributor as a write failure.

## Result

Always report `outcome=ok|partial|error`, the verified `overall` value, and the
concise non-empty contributor summary from `planar health --json`. A healthy
result explicitly says no reconciliation is needed.

## Warnings

Name degraded or critical contributors and unavailable checks. An expected
zero count or healthy installation emits no warning.

## Next actions

Give zero to three executable recommendations tied to actual contributors,
such as `scriptorium status --config scriptorium.yaml --json`, `planar resume validate <task-id>`, or
invoking `pl-doctor`; omit generic advice when health is clean.

## Recovery

If the check fails, provide `planar health --json` as the exact retry and route
an integrity failure to inspection rather than reconciliation. This read-only
skill has no undo path.

## Vendor Notes
