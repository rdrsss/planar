# planar_engine_search — engine/search bucket (plan 996, task 6090).
#
# Layer-2 `engine_*` module: may depend on layer-1 modules only, never
# sideways onto another `engine_*` bucket (D15/D18). This bucket is the
# clearest case in the tree of D19 paying for itself. The Zig original
# (zig/src/engine/search.zig) calls `identity.scope.resolveSlug` to turn
# `--scope <slug>` into a `(kind, id)` pair; as a C++ dependency that would
# have been `engine_search -> engine_identity`, which
# cmake/architecture.cmake FATALs on at configure time. Task 6089 had
# already extracted that resolution into layer-1 `planar.scope_ref`, so the
# edge points DOWN instead and nothing needed inventing here.
#
# Two dependencies, both layer 1: `db` and `scope_ref`.
#
# The six FTS5 virtual tables this queries (`plans_fts`, `tasks_fts`,
# `questions_fts`, `test_scenarios_fts`, `decisions_fts`, `artifacts_fts`)
# come from migration 00011 and are a CLOSED set — see search.cppm on why an
# unrecognised `--kind` refuses rather than matching nothing.

planar_module(engine_search
  INTERFACE search.cppm
  SOURCES search.cpp
  DEPENDS
    db
    scope_ref)
