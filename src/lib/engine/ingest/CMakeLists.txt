# planar_engine_ingest — engine/ingest bucket (plan 996, task 6035, M4).
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition, unchanged by D19). `DEPENDS db` is the whole
# dependency set — every module below reaches the database directly through
# `planar.db` or is pure.
#
# Behavior-preserving port (D2) of a SUBSET of zig/src/engine/ingestor/:
# parse.zig, diff.zig, coverage.zig, render.zig, materialize.zig. That subset
# is the whole read side of the pipeline — workbench markdown in, proposed
# diff + strict-gate coverage object + provenance-bearing routing facts out.
#
# DELIBERATELY NOT PORTED THIS CYCLE, and the reason is architectural rather
# than a matter of remaining time:
#
#   - apply.zig. It is the only file in the Zig ingestor that reaches outside
#     its own bucket, and it reaches into SIX others: `planning.plan`,
#     `planning.task`, `planning.decision`, `planning.question`,
#     `planning.scenario`, `entitylink`, and `runtime.session`. Under D15/D18
#     every one of those is a layer-2 `engine_*` peer, so a `DEPENDS
#     engine_planning engine_entitylink` line here would FATAL at configure
#     time — not silently succeed. D19's remedy (extract the shared primitive
#     to layer 1) does not apply: what `apply` needs is not a shared
#     primitive but the entity CRUD verbs themselves, and extracting
#     plan/task/decision/question/scenario creation into layer 1 would move
#     the entire planning bucket down a layer to serve one caller — which is
#     the layering inverted, not respected.
#
#     Compounding it, three of those six verb surfaces DO NOT EXIST in the
#     C++ tree yet: `engine_planning` deliberately ported only plan + task
#     (see its CMakeLists.txt), leaving decision, question, scenario, and
#     artifact unported. `apply` cannot be written against them at all today.
#     It lands once those verbs exist and the correct home for the
#     cross-bucket composition has been decided (most likely layer 3, where
#     composing engine buckets is exactly what the layer is for).
#
#   - import.zig and synthesize.zig (the import/synthesize flows). Both are
#     LLM-pass-bearing flows on top of the same entity-creation surface
#     `apply` needs, so they inherit the identical blocker, and both are
#     explicitly last in this task's stated priority order.
#
# The cross-scope guard is NOT wired into this module — task 6075 is an
# unresolved OPERATOR decision on the guard's true breadth, and this module
# performs no entity-targeted mutation in any case (`materialize` replaces
# routing facts inside a transaction the CALLER owns). It stays exactly as
# opt-in as `engine_identity` exposes it.
#
# `ingest.cppm` is the umbrella re-export, matching `engine/planning/
# planning.cppm`'s pattern.

planar_module(engine_ingest
  INTERFACE
    parse.cppm
    diff.cppm
    coverage.cppm
    render.cppm
    materialize.cppm
    ingest.cppm
  SOURCES
    parse.cpp
    diff.cpp
    coverage.cpp
    render.cpp
    materialize.cpp
  DEPENDS
    db
    json_text)
