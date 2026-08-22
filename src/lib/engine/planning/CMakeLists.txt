# planar_engine_planning — engine/planning bucket (plan 996, task
# cpp-planning-verbs). Layer-2 `engine_*` module (cmake/architecture.cmake):
# may DEPEND on layer-1 `db` only. This task's brief said "may depend on
# db, cli, engine_identity per D15/D18" — VERIFIED FALSE against the
# actual decision record before writing any CMake: `planar decision show
# 944` (D18, "supersedes D17; corrected rationale", status accepted) is
# explicit that "layers 2 (engine_*) and 3 (cmd_*) KEEP the same-layer
# prohibition" — D17/D18 only ever granted layer-1-to-layer-1 edges (e.g.
# cli -> core), never engine-to-engine. `cmake/architecture.cmake`'s own
# D15 walk (`_layer EQUAL 1 AND _dep_layer EQUAL 1` is the ONLY same-layer
# exception) confirms the same thing independently of the decision record.
# A `DEPENDS engine_identity` line here would therefore FATAL at configure
# time, not silently succeed. Per this task's own instructions ("Do not
# trust the brief over the workbench spec... the spec wins and the
# disagreement becomes a question or follow-up task"): this module does
# NOT depend on `engine_identity`. Its small scope-slug-resolution need
# (`global` / `repo:<slug>` / `assoc:<slug>` / bare-association forms) is
# instead duplicated locally in plan.cpp/task.cpp, mirroring
# `identity::scope`'s `resolve_slug` algorithm at the SQL level only — the
# same "no header tree for first-party code, so small helpers are
# duplicated per translation unit" convention `association.cpp`'s
# `slugify_path_segment` and every `*.t.cpp`'s `scratch_db_path` already
# establish elsewhere in this tree. See this task's coder report for the
# brief/spec disagreement writeup. Behavior-preserving port (D2) of a
# SUBSET of
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
    planning.cppm
  SOURCES
    transitions.cpp
    plan.cpp
    task.cpp
  DEPENDS
    db)
