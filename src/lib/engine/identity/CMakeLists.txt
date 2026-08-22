# planar_engine_identity — engine/identity bucket (plan 996, task
# cpp-scope-assoc). Layer-2 `engine_*` module (cmake/architecture.cmake):
# may DEPEND on layer-1 `db` and `scope_ref` (D15). Behavior-preserving
# port (D2) of zig/src/engine/identity/{scope.zig,association.zig} —
# cwd-derived scope resolution, the `resolveSlug`/`slugFromRef` scope-ref
# plumbing, the cross-scope guard's refusal decision (mirrors
# zig/src/engine/policy/scope_guard.zig — folded into scope.cppm rather
# than a separate `policy` module, since no `policy` bucket task exists
# yet and the guard is a single small pure function; tech-spec's
# File-level tree lists this directory's contents as "scope, association,
# project, workspace" with no separate guard file), and the association
# CRUD + membership surface the guard's callers need. `project.cppm` and
# `workspace.cppm` are not part of this task's scope (task brief: "do NOT
# attempt the config/templates work" and the association module here only
# exposes the minimal `project_ref` projection `members()` needs, mirroring
# zig/src/engine/identity/association.zig's own minimal `Project` struct —
# full `project.zig`/`workspace.zig` CRUD lands in a later task).
#
# `identity.cppm` is the umbrella re-export, matching `lib/cli/cli.cppm`'s
# pattern (single import point for the whole bucket).
#
# `scope_ref` (plan 996, task 6089, decision D19): `scope.cpp`'s
# `resolve_slug` used to implement the `global`/`repo:<slug>`/
# `assoc:<slug>`/bare-association grammar and its DB lookup directly; that
# algorithm is now extracted into the layer-1 `planar.scope_ref` module
# (`lib/scope_ref/`) so `engine_planning` can consume it too without a
# forbidden layer-2-to-layer-2 edge onto `engine_identity` (D18 unchanged).
# `resolve_slug` is now a thin translator onto `scope_ref::resolve`; this
# module's own `scope_kind`/`scope_ref`/`scope_error` public types are
# unaffected — only the implementation moved.

planar_module(engine_identity
  INTERFACE
    scope.cppm
    association.cppm
    identity.cppm
  SOURCES
    scope.cpp
    association.cpp
  DEPENDS
    db
    scope_ref
  TEST_DEPENDS
    cli)

# `cli` is a TEST-only dependency: scope.t.cpp imports `planar.cli.exit` to
# pin `check_scope_guard`'s `scope_error::scope_mismatch` against the SAME
# `domain_error_kind::scope_mismatch -> exit 5` mapping a future `cmd/`
# handler will reuse (task brief: "reuse it rather than re-deriving"),
# rather than asserting a bare enumerator value with no tie back to the
# oracle-captured exit code. No production `.cpp` in this module names
# `planar.cli` at all, so it belongs under `planar_module()`'s
# TEST_DEPENDS (task 6079) — linked into `planar_engine_identity_tests`
# only, never into the production `planar_engine_identity` target, so
# `engine_identity` does not carry an unused `planar_cli` edge for
# `cmake/architecture.cmake`'s D15 walk to police.
