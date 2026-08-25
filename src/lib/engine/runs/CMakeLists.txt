# planar_engine_runs — engine/runs bucket (plan 996, task 6095).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). This module needs only `db`, so
# no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of zig/src/engine/runs/runs.zig plus the
# validation and rendering halves of zig/src/cmd/planar/handlers/bench/*.zig
# and zig/src/cmd/planar/handlers/run/*.zig -- nine of the ten `bench` + `run`
# schema leaves (bench start|event|touch|finish|show, run start|event|
# finish|show).
#
# All nine are WIRED as of plan 996 task 6149 (src/cmd/planar/handlers/
# runs.cpp). That cycle added one renderer here, `render_invalid_task_id`:
# `bench start --task` is declared `(string)` + repeatable on the tree, so
# the integer conversion belongs to the leaf and its refusal wording is the
# oracle's rather than the parser's. See render.cppm's note on it.
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `bench harvest`, the tenth leaf. It rests on
#     zig/src/engine/runs/harvest.zig (671 lines), whose job is shelling
#     `git diff --name-only` / `git rev-parse` through `std.process.run` to
#     derive kind='actual' touches from a worktree. There is no process-spawn
#     abstraction in this tree, and building one inside a port task would be a
#     much larger change than the leaf it serves -- the same call the
#     engine/runtime bucket made for `capture commits` in cycle 4. Deferred
#     WITH its dependency. Its DB-visible half IS ported and tested:
#     `lifecycle::touch_idempotent` is exactly the `insert or ignore`
#     primitive harvest writes through.
#   - ~~`policy.audit.record` calls.~~ NOT AN OMISSION, and never was
#     (task 6100). The layer-1 `planar.policy` module now exists, and this
#     bucket was audited against it: the Zig originals behind the `bench` and `run` leaves contain
#     ZERO `policy.audit.record` call sites, so there is nothing here to
#     wire. The paragraph this replaces claimed a gap it inherited by
#     copy-paste from a bucket that has one. Verified with
#     `grep -rln policy.audit.record zig/src/engine`, whose whole output is
#     five buckets: planning, identity, entitylink, promotion, runtime.
#
# `runs.cppm` is the umbrella re-export, matching
# `lib/engine/runtime/runtime.cppm`.
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

planar_module(engine_runs
  INTERFACE
    lifecycle.cppm
    render.cppm
    runs.cppm
  SOURCES
    lifecycle.cpp
    render.cpp
  DEPENDS
    db
    json_text)

# render.cpp validates --payload / --config-json through glz::validate_json --
# the same purpose-built probe engine/config uses, and the only one that
# enables Glaze's validate_trailing_whitespace (task 6086). PRIVATE: Glaze is
# an implementation detail of the renderer, not part of this module's
# exported interface.
target_link_libraries(planar_engine_runs PRIVATE glaze::glaze)

if(TARGET planar_engine_runs_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runtime's suite does.
  target_link_libraries(planar_engine_runs_tests PRIVATE SQLite::SQLite3)
endif()
