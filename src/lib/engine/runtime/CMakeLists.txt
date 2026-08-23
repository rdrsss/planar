# planar_engine_runtime — engine/runtime bucket (plan 996, task 6094).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18's same-layer prohibition for layers 2 and 3). This module needs
# only `db`, so no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of zig/src/engine/runtime/{session,
# snapshot,capture}.zig plus the body-composition and rendering halves of
# zig/src/cmd/planar/handlers/capture/*.zig -- six of the seven `capture`
# schema leaves (session, end, note, command, file, snapshot).
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `capture commits`, the seventh leaf. It rests entirely on
#     zig/src/engine/runtime/sessioncommits.zig (1205 lines), whose whole
#     job is shelling `git` through `std.process.run` -- revision walks,
#     ref resolution, per-commit metadata. There is no process-spawn
#     abstraction in this tree, and building one inside a port task would
#     be a much larger change than the leaf it serves. Deferred WITH its
#     dependency.
#   - `capture.zig`'s `probeStartGitContext` and `recordSessionWindow`,
#     for the same reason. Their DB-visible halves are preserved:
#     `session::set_start_git_context_if_unset` is ported and tested
#     (including its write-once semantics), and `capture::open_session`
#     takes the probed context as an optional argument instead of probing
#     for it. `recordSessionWindow` is a no-op whenever `repo_root` or
#     `head_sha_at_start` is NULL, which is every session this tree can
#     currently open, so its absence changes nothing observable here.
#   - `policy.audit.record` calls in `startSession` / `endSession` /
#     `snapshot.create`. There is still no `policy.audit` module in the
#     C++ tree; `engine_planning/CMakeLists.txt`,
#     `engine_identity/association.cppm` and `engine_promotion` all
#     document the same omission. No `capture` leaf reads or emits audit
#     rows, so no observable CLI contract is affected.
#
# `runtime.cppm` is the umbrella re-export, matching
# `lib/engine/identity/identity.cppm`.

planar_module(engine_runtime
  INTERFACE
    session.cppm
    snapshot.cppm
    capture.cppm
    runtime.cppm
  SOURCES
    session.cpp
    snapshot.cpp
    capture.cpp
  DEPENDS
    db)
