# planar_engine_planning — engine/planning bucket (plan 996, task
# cpp-planning-verbs). Layer-2 `engine_*` module (cmake/architecture.cmake):
# may DEPEND on layer-1 modules only, never sideways onto another `engine_*`
# bucket (D15/D18's same-layer prohibition for layers 2/3 — see task
# cpp-planning-verbs's original writeup below for the brief/spec
# disagreement that first surfaced this). Originally this module's small
# scope-slug-resolution need (`global` / `repo:<slug>` / `assoc:<slug>` /
# bare-association forms) was duplicated locally in plan.cpp/task.cpp,
# mirroring `identity::scope`'s `resolve_slug` algorithm at the SQL level.
# Decision D19 (plan 996, task 6089) closed that duplication risk by
# extracting the algorithm into the new layer-1 `planar.scope_ref` module
# (`lib/scope_ref/`), which THIS module and `engine_identity` both now
# depend on downward — D18's same-layer prohibition is unchanged, this is
# what lets it stay strict. `association.cpp`'s `slugify_path_segment` was
# assessed under the same D19 pass and found NOT duplicated anywhere else
# in the tree (see `lib/scope_ref/scope_ref.cppm`'s file header) — it stays
# where it is.
#
# This task's brief said "may depend on db, cli, engine_identity per
# D15/D18" — VERIFIED FALSE against the actual decision record before
# writing any CMake: `planar decision show 944` (D18, "supersedes D17;
# corrected rationale", status accepted) is explicit that "layers 2
# (engine_*) and 3 (cmd_*) KEEP the same-layer prohibition" — D17/D18 only
# ever granted layer-1-to-layer-1 edges (e.g. cli -> core, and now
# engine_identity -> scope_ref / engine_planning -> scope_ref), never
# engine-to-engine. `cmake/architecture.cmake`'s own D15 walk (`_layer
# EQUAL 1 AND _dep_layer EQUAL 1` is the ONLY same-layer exception)
# confirms the same thing independently of the decision record. A `DEPENDS
# engine_identity` line here would therefore FATAL at configure time, not
# silently succeed. Behavior-preserving port (D2) of a SUBSET of
# zig/src/engine/planning/{plan.zig,task.zig} plus the `.plan`/`.task` arms
# of zig/src/engine/policy/status.zig — the plan/task CRUD surface, the
# status-transition matrices for those two entity kinds, and the plan-304
# auto-promotion invariant (docs/concepts.md:459) on every one of task's
# six write paths (create, update, mark_done, mark_cancelled, mark_blocked,
# reopen).
#
# Explicitly NOT ported by this task (see plan.cppm / task.cppm file
# headers for the full per-cut rationale — this is the summary):
#   - question, scenario, decision, artifact, plan_step entities entirely.
#     Their Zig originals total ~4300 more lines on top of plan.zig +
#     task.zig's ~3460, and each has its own status.zig arm, its own CRUD
#     shape, and (for scenario/decision/artifact) its own delegation
#     wrinkles. Porting a coherent, well-tested plan+task+transition+
#     promotion subset in one cycle beats a rushed six-entity sweep with
#     thin test coverage on all of them — this task's own brief invites
#     exactly this cut ("A well-tested subset beats a rushed whole").
#   - The active-work-claim TOCTOU guard (decision 533 / task 4165) — the
#     `agent_work_claims` table belongs to a different, not-yet-ported
#     engine bucket (`engine.runtime.agentactivity`).
#   - `due_at` format validation, the `task_touch_paths`/`touches` surface,
#     `renderText`/`renderListText` (no `cmd/` binary in this task's
#     scope), and `policy.audit.record` calls (no `policy.audit` module in
#     the C++ tree yet — same omission `engine_identity`'s `association.cppm`
#     already documents).
#
# The cross-scope guard (`planar.engine.identity.scope`'s
# `check_scope_guard`/`guard_write`) is DELIBERATELY NOT wired into this
# module's verbs — task 6075 is an unresolved OPERATOR decision on whether
# the guard should apply to every mutation verb (documented behavior) or
# only three (actual Zig behavior); wiring it in here implicitly would
# silently pick a side. The guard stays exactly as opt-in as
# `engine_identity` exposes it.
#
# `planning.cppm` is the umbrella re-export, matching `lib/engine/identity/
# identity.cppm`'s pattern.

planar_module(engine_planning
  INTERFACE
    transitions.cppm
    plan.cppm
    task.cppm
    annotation.cppm
    test_spec_status.cppm
    planning.cppm
  SOURCES
    transitions.cpp
    plan.cpp
    task.cpp
    annotation.cpp
    test_spec_status.cpp
  DEPENDS
    db
    scope_ref
  TEST_DEPENDS
    engine_identity)

# `engine_identity` is a TEST-only dependency (task 6089, D19): the
# cross-consumer agreement test in scope_ref_agreement.t.cpp imports
# `planar.engine.identity.scope` purely to prove, at the API level, that
# `engine_identity` and `engine_planning` resolve the same scope-ref slug
# to the same `(kind, id)` pair now that both consume `planar.scope_ref`.
# No production `.cpp` in this module names `planar.engine.identity`
# at all, so this belongs under TEST_DEPENDS (same convention
# `engine_identity/CMakeLists.txt` already establishes for its own `cli`
# test dependency) — linked into `planar_engine_planning_tests` only,
# never into the production `planar_engine_planning` target. This does
# NOT reopen a layer-2-to-layer-2 production edge: `cmake/architecture.cmake`'s
# D15 walk only polices `PLANAR_MODULE_TARGETS` (production module
# targets), which test binaries are never appended to (see
# `cmake/module.cmake`).
