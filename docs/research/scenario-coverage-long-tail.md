# Scenario coverage long-tail audit

Generated 2026-05-26 as part of plan 352 M14 (final closeout). This
document enumerates the integration-test leaves that remain
uncovered after the 13-scenario fan-out and classifies each into
one of three buckets: **engine-blocked**, **fixture-blocked**, or
**long-tail-accepted**. Total: 65 uncovered leaves after M1–M13.

The long-tail policy ratchet (`scripts/coverage-check.sh`) does NOT
require 100% leaf coverage. The CI gate locks the *current ratio*
and fails on regression; it does not require every uncovered leaf
to gain coverage.

## Bucket 1 — Engine-blocked (16 leaves)

Handlers are scaffolded (`_stub.zig`) or depend on engine work that
hasn't landed. Scenario coverage requires the engine implementation
first.

- `annotate add / show / list / update / remove / tag / resolve /
  dismiss / archive / bulk-resolve / bulk-dismiss / bulk-archive /
  verify / sweep` (14 leaves) — every handler dispatches to
  `_stub.zig` returning `NotImplemented`. **Filed: task 2452.**

## Bucket 2 — Fixture-blocked (9 leaves)

The handler works but the scenario needs a fixture this fan-out
didn't build (HTTP server, --body-file rendered offline, anchor
plan with a specific shape).

- `audit publish-decision` — needs ext_link fixture; covered when
  M2 (external plane) lands the HTTP fixture.
- `ext create` — needs external system registration; M2.
- `doc promote`, `doc regenerate` — need pre-rendered --body-file
  or doc-promote LLM skill. **Filed: task 2453.**
- `templates render` — needs an anchor plan that survives the
  template's parent-plan walk; trivial fixture fails with
  `AnchorPlanNotFound`. **Filed: task 2454.**
- `workbench edit`, `workbench publish`, `workbench resolve` —
  workbench/edit needs `$EDITOR` interactivity; publish needs ext
  fixture (M2); resolve needs an intentional FS↔DB conflict.
- `handoff create / validate` — covered indirectly by the
  top-level `handoff` verb but not via the `handoff create
  <snapshot-id>` direct path.

## Bucket 3 — Long-tail accepted (40 leaves)

Read-only display / editor / reviewer verbs that are exercised
by `*_test.zig` focused tests where they exist, and accepted as
focus-tested only otherwise. Scenario tests would add little
signal beyond what the focused tests already lock.

**Diff verbs** (DB-vs-workbench drift detector): `artifact diff`,
`decision diff`, `plan diff`, `question diff`, `scenario diff`,
`task diff`.

**View verbs** (open workbench file in pager): `artifact view`,
`decision view`, `plan view`, `question view`, `scenario view`,
`task view`.

**Edit verbs** (editor-first flow with `$EDITOR`): `decision edit`,
`plan edit`, `question edit`, `scenario edit`, `task edit`,
`config edit`. These rely on `EDITOR=true` (no-op editor) to
exercise the round-trip without interactive input. `editflow_*_
test.zig` files cover them.

**Review verbs** (reviewer entry points for diff workflows):
`artifact review`, `decision review`, `plan review`, `question
review`, `scenario review`, `task review`. Exercised by the editor-
flow focused tests where applicable.

**Mass-link / list / detect verbs**: `assoc detect`, `assoc list`,
`assoc remove`, `links remove`, `links trail`, `links update`,
`artifact link`, `plan link`, `task link` — duplicate the
relationship-aware `link` verbs each entity-kind already exposes;
the top-level `link` / `unlink` plus per-entity `*_link` cover
the contract.

**Capture sub-verbs** (`capture command / end / file / note`):
not currently exercised by a scenario, but exercise no engine
logic not already covered by `capture session` + `capture snapshot`.

**Task state**: `task block / cancel / reopen` — status transition
verbs whose contract is locked by the engine's status enum CHECK
constraints + the focused per-verb tests.

**resume validate**: the validation sub-verb; the top-level
`resume` (covered by flagship + M13) already exercises the
underlying validation path.

**Top-level link/unlink** (`link .`, `unlink .`): catch-all
shortcuts for the per-entity `*_link` verbs.

## Net coverage after fan-out

```
total leaves:    168
exercised:       103 (61%)
uncovered:       65   (39%)
```

By bucket:
- Engine-blocked (M6, M2-dependent): 16 (10%)
- Fixture-blocked (close after follow-ups): 9 (5%)
- Long-tail accepted: 40 (24%)

If the engine-blocked + fixture-blocked tasks land, the achievable
ratio (without writing long-tail scenarios) is **~119/168 (71%)**.
Writing scenario coverage for every long-tail diff/view/edit/review
verb would push toward 100% but trade signal for noise; the
ratchet's job is to prevent regression, not to incentivize
synthetic coverage.

## Reseed cadence

The baseline file (`scripts/coverage-baseline.txt`) is updated only
via `make coverage-update`, run by a contributor adding (or
intentionally removing) coverage. The CI gate (`make coverage`)
fails on:
- exercised count dropping below baseline, OR
- total leaves growing without exercised growing to match.

A new verb added to the CLI surface trips condition (b) immediately,
forcing the contributor to either extend a scenario or document the
deferral.

## See also

- Plan 352 — Scenario coverage buildout (anchor plan)
- CLAUDE.md § Integration test methodology — the doctrine
- `scripts/coverage-check.sh` — the ratchet itself
- Tasks 2442 (slug-extraction), 2452 (annotate engine), 2453
  (doc/promote fixture), 2454 (templates/render fixture).
