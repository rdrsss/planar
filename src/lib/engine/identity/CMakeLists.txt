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
    git
    json_text
    scope_ref
    policy
  )

# `git` (task 6325): `association.cpp`'s `detect_proposals` probes the
# cwd's `origin` remote to derive its `host:` and `org:` proposals. It
# calls `planar.git`'s `probe_origin_url`, which already carries the
# oracle's exit-status and empty-stdout handling, rather than opening a
# second subprocess path. `git` is layer 1, so this is a layer-2-to-layer-1
# edge and D18 is unaffected. This is a NEW dependency for the module --
# `scope.cpp` does not import `planar.git` (its worktree detection is
# reached through the handler, not from inside the engine), so
# `association.cpp` is currently the only file here that needs it.

# `json_text` (task 6133): `association.cpp`'s `render_json` needs the one
# shared RFC 8259 string-escape table rather than a second local copy —
# an association's `name` and `config_json` are operator-supplied and can
# carry quotes, backslashes and control characters. `json_text` is layer 1,
# so this is an ordinary D15 downward edge, the same one `engine_planning`
# already declares for the identical reason.

# TEST_DEPENDS is empty as of task 6123. It used to name `cli`, because
# scope.t.cpp imported `planar.cli.exit` to pin `check_scope_guard`'s
# `scope_error::scope_mismatch` against the shared, binary-parameterized
# `domain_error_kind::scope_mismatch -> exit 5` mapping a `cmd/`
# handler would reuse. Task 6123 deleted `src/lib/cli` and moved the
# exit-code policy into each binary's own `src/cmd/<binary>/exit.cppm`, so
# there is no layer-1 table left to import — and an engine test may not
# reach a `cmd_*` module (upward layer-2 -> layer-3, which
# `cmake/architecture.cmake` FATALs on). The `scope_mismatch -> 5`
# assertion moved down-stream into all three `src/cmd/*/exit_codes.t.cpp`
# rather than being dropped; scope.t.cpp keeps the engine-level fact those
# tests key off. TEST_DEPENDS itself (task 6079) remains available for the
# next module that needs a test-only edge.
