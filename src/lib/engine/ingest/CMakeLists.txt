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
#
# `packet.cppm` (plan 996, task 6324) is the routing packet behind
# `planar task packet <id>`. Its Zig original lives in `engine/routing/`, so
# the obvious placement is a new `engine_routing` bucket — that bucket CANNOT
# BE BUILT. The packet's whole freshness computation is defined in terms of
# `materialize`'s digests (`artifact_section`, `roadmap_section`, `section`,
# `field`, `source_digest`, `materializer_version`), and D15/D18 FATAL on a
# layer-2 `engine_*` depending on another layer-2 `engine_*`.
#
# D19's remedy — extract the shared primitive to layer 1 — was measured and
# rejected rather than skipped: `roadmap_section` calls `parse::parse_roadmap`,
# so extracting the helpers drags `parse.cppm` (the whole markdown-parsing half
# of this bucket) down a layer to serve one caller. That is the same "layering
# inverted, not respected" argument this file already makes about `apply.zig`
# above, and it applies with the same force here.
#
# Co-location is also the honest description of the coupling rather than a
# workaround for it. `materialize` WRITES the `routing_task_facts` rows;
# `packet` READS them back and RECOMPUTES the identical digest live to decide
# whether each fact is still current. The oracle says so itself at
# `zig/src/engine/ingestor/materialize.zig:489` — the two halves must agree
# byte for byte or every fact is permanently stale the instant it is written.
# A configure-time wall between two functions that are one contract would be
# the worse outcome.
#
# The PLANNING half of `packet.zig` is NOT here: it serves `models resolve`
# (already deferred by `engine/models/CMakeLists.txt`) and depends on
# `test_spec_status`, an `engine_planning` module, so it meets this same wall
# from the other side. Whoever takes `models resolve` inherits that decision.
#
# This module added ONE export to `materialize.cppm`: `sha256_hex`, which
# `packet` digests whole evidence texts with. See its doc comment for the
# standing note that three other sha256 copies exist in the tree and that
# consolidating them is a cross-bucket refactor, not a port-cycle line item.

planar_module(engine_ingest
  INTERFACE
    parse.cppm
    diff.cppm
    coverage.cppm
    render.cppm
    materialize.cppm
    packet.cppm
    ingest.cppm
  SOURCES
    parse.cpp
    diff.cpp
    coverage.cpp
    render.cpp
    materialize.cpp
    packet.cpp
  DEPENDS
    db
    json_text)
