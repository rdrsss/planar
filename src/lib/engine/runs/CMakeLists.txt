# planar_engine_runs — engine/runs bucket (plan 996, task 6095).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). `db` and, since task 6362, the
# layer-1 `git` seam (see below); no D19 extraction was required for either.
#
# Behavior-preserving port (D2) of zig/src/engine/runs/runs.zig plus the
# validation and rendering halves of zig/src/cmd/planar/handlers/bench/*.zig
# and zig/src/cmd/planar/handlers/run/*.zig -- all ten `bench` + `run` schema
# leaves (bench start|event|touch|harvest|finish|show, run start|event|
# finish|show).
#
# Nine were WIRED as of plan 996 task 6149 (src/cmd/planar/handlers/
# runs.cpp). That cycle added one renderer here, `render_invalid_task_id`:
# `bench start --task` is declared `(string)` + repeatable on the tree, so
# the integer conversion belongs to the leaf and its refusal wording is the
# oracle's rather than the parser's. See render.cppm's note on it.
#
# `bench harvest`, the tenth leaf, landed at task 6362 in its own module,
# `harvest.cppm`/`harvest.cpp`. It had been deferred WITH its dependency —
# zig/src/engine/runs/harvest.zig shells `git diff --name-only` through
# `std.process.run`, and this tree had no process-spawn abstraction at the
# time. Tasks 6128/6137/6272/6358 closed that gap at layer 1
# (`planar.git::run`) for four other consumers; this task is the fifth,
# reached by adding `git` to this module's DEPENDS below. Its DB-visible
# half was already ported and tested — `lifecycle::touch_idempotent` is
# exactly the `insert or ignore` primitive harvest writes through, unchanged
# by this task.
#
# NOT ported, named explicitly rather than silently dropped:
#
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
    harvest.cppm
    runs.cppm
  SOURCES
    lifecycle.cpp
    render.cpp
    harvest.cpp
  DEPENDS
    db
    git
    json_text
    log)

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
