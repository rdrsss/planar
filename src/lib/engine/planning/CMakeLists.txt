# planar_engine_planning — engine/planning bucket (plan 996, task
# cpp-planning-verbs). Layer-2 `engine_*` module (cmake/architecture.cmake):
#
# The `log` dependency (task 6202) is a legal downward layer-1 edge and
# exists for ONE reason: the oracle's engine writes an inner
# `<op> exec failed: <ErrName>` line to stderr from inside each failed
# write, ahead of the handler's outer `error: <verb>: QueryFailed`. That
# line is engine-side in the oracle and has to be engine-side here too —
# see `task.cpp`'s `exec_failed` for the seam and `lib/log/log.cppm`'s
# `diag_err` for why it bypasses spdlog's formatters.
#
# The `git` dependency (task 6317) is the SECOND downward layer-1 edge, and
# it arrived with `plan closeout`. `git` carries no name prefix, so
# `_planar_module_layer` classifies it LAYER 1 and a layer-2 → layer-1 edge
# is the ordinary case D15 permits, not an exception. It is engine-side for
# the same reason `log` is: `closeout.zig` runs its advisory ancestry probes
# from inside the engine, ahead of any renderer, and moving them to layer 3
# would split one evaluation across two layers for no gain.
#
# READ `closeout.cppm`'s DIVERGENCE section before touching that code. The
# oracle's own git probes are DEAD — they route through
# `std.Io.Threaded.global_single_threaded.io()`, on which every
# `std.process.run` fails with OutOfMemory under Zig 0.16.0 (reproduced
# standalone, three allocators) — so the oracle answers "git-evidence
# unavailable" for every input, including a clone where the identical
# commands succeed from a shell. This module ports the logic WORKING over
# `planar.git`. That is a deliberate, documented divergence and the ONLY one
# in this bucket. It is confined to ADVISORY fields: `ready`, `applied`,
# `blocked_by`, `warnings`, every count, the exit code, the `plans` UPDATE
# and the `audit_log` row never read the git layer, so the authoritative
# gate stays bit-identical to the oracle's.
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
#     UPDATE (task 6188): `plan_step` landed at task 6187 and `question`
#     landed here, again as ONE family rather than four shallow ones. The
#     remaining three — scenario, decision, artifact — are still out. See
#     question.cppm's header for what the question port itself cut and
#     where those cuts are composed instead (the entity-create activity
#     hook moves to LAYER 3 rather than disappearing).
#     UPDATE (task 6194): `decision` landed, on the same one-family
#     discipline; `scenario` and `artifact` remain out. Its port added the
#     `.decision` arm to transitions.cpp — the first arm added because its
#     CALLER arrived, which is the rule that file's header states. Two
#     things about it are worth reading before the next family lands:
#       - `supersede` writes its `entity_links` row DIRECTLY rather than
#         composing `engine_entitylink`. That is forced twice over: the
#         edge would be a layer-2-to-layer-2 dependency this file's own
#         D15/D18 note forbids, AND `entitylink::add` writes a `link`
#         audit row plus an endpoint check that the oracle's `supersede`
#         does not. See decision.cppm's header.
#       - `decision add --plan <n>` does NOT validate the plan, where
#         `question add --plan <n>` does. Both behaviours are the oracle's
#         and both are reproduced; the divergence is in the ORACLE, not in
#         this port.
#     UPDATE (task 6195): `scenario` landed, leaving `artifact` as the last
#     unported planning entity. Its port added the `.scenario` arm to
#     transitions.cpp, again because its caller arrived. Three things about
#     it are worth reading before `artifact` lands:
#       - Its EMPTY `--status` filter means EVERY status, `retired`
#         included. `question`'s means `open` and `decision`'s means
#         `{proposed, accepted}`. Three sibling families, three answers;
#         none is derivable from another.
#       - `--related <n>` and `--plan <n>` behave OPPOSITELY on the same
#         verb: the first is a real column FK and refuses a nonexistent
#         artifact, the second is an `entity_links` row and happily
#         dangles. Neither is this port's code — see scenario.cppm.
#       - `verify` does NOT call `test_spec_status`, contrary to task
#         6195's brief. The dependency runs the other way and is
#         data-level; verified by grep over the whole Zig tree before any
#         code was written. See scenario.cppm's header.
#   - The active-work-claim TOCTOU guard (decision 533 / task 4165).
#     CORRECTED at task 6141: this note used to say the guard was cut
#     because `engine.runtime.agentactivity` was "a different,
#     not-yet-ported engine bucket". That bucket IS ported (tasks 6120 and
#     earlier), so the stated reason had gone stale — exactly the failure
#     mode this project's briefs warn about, where a deferral note outlives
#     its cause and the next cycle either re-ports work or leaves a real
#     gap open. The guard still cannot live in THIS module, but for a
#     different and permanent reason: `engine_planning` and
#     `engine_runtime` are both LAYER 2, and `cmake/architecture.cmake`
#     FATALs on a same-layer edge (D15/D18). It is composed at layer 3 by
#     the `task` handlers instead — the D20 shape `annotate add` and
#     `unlink` already use — via
#     `agentactivity::has_active_claim_on_task`. See that function for the
#     one behavioural narrowing that move costs (TOCTOU width).
#   - `due_at` format validation (PORTED at task 6135),
#     `renderText`/`renderListText` (PORTED: the single-entity renderers at
#     task 6133/6135, the list renderers at task 6141, when `plan list` and
#     `task list` were wired).
#   - Still open: the `task_touch_paths`/`touches` surface — `listTouching`
#     has no port, so `plan list --touches` / `task list --touches` refuse
#     at exit 64 rather than silently ignoring the filter — and
#     the `policy.audit.record` calls, which are now PORTED (task 6100):
#     `planar.policy` exists at layer 1 and plan/task/annotation all write
#     through it. See src/lib/policy/CMakeLists.txt for the oracle-derived
#     row shapes and for which buckets are still unwired.
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
#
# STDOUT TERMINATOR CONTRACT (task 6114, resolving the M4 boundary review's
# F1). A renderer in this tree that returns a COMPLETE stdout payload returns
# the oracle's exact bytes INCLUDING the trailing newline; the layer-3 caller
# writes the string verbatim and appends nothing. A renderer that returns a
# FRAGMENT the caller composes into a larger line -- an error-message body
# that becomes `error: <body>` on stderr -- returns no terminator and its
# @return says so. The alternative shape ("renderer returns the body, caller
# appends the terminator") cannot express two payloads this tree actually has:
# a leaf whose empty output is ZERO BYTES rather than a bare newline
# (`workflow list --json`, `local list --json`), and NDJSON, where the newline
# is per-RECORD rather than per-document. Both are pinned. See
# `json_text/CMakeLists.txt` for the escaping half of the same contract.

planar_module(engine_planning
  INTERFACE
    transitions.cppm
    plan.cppm
    task.cppm
    question.cppm
    decision.cppm
    scenario.cppm
    artifact.cppm
    annotation.cppm
    plan_step.cppm
    test_spec_status.cppm
    descendants.cppm
    feedback_triage.cppm
    strategy.cppm
    closeout.cppm
    planning.cppm
  SOURCES
    transitions.cpp
    plan.cpp
    task.cpp
    question.cpp
    decision.cpp
    scenario.cpp
    artifact.cpp
    annotation.cpp
    plan_step.cpp
    test_spec_status.cpp
    descendants.cpp
    feedback_triage.cpp
    strategy.cpp
    closeout.cpp
  DEPENDS
    db
    git
    json_text
    log
    scope_ref
    policy
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
