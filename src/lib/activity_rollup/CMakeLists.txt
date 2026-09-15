# planar.activity_rollup — layer-1 per-entity agent-activity rollup
# (decision D19, plan 996 task 6282). Extracted out of `engine_tree` so
# that BOTH `engine_tree` (which folds it onto every `planar tree` node)
# and `engine_runtime` (the natural home for it, alongside the two
# siblings it already carries for `audit trail`) can depend on it
# downward. `engine_tree -> engine_runtime` is an engine<->engine edge
# that cmake/architecture.cmake FATALs on, which is why the rollup was a
# documented duplicate rather than a call; D19's "shared engine primitives
# are extracted into a layer-1 module" is the fix, and D18's same-layer
# prohibition for layers 2/3 stays strict.
#
# Done BEFORE a second copy exists: with one implementation this is a MOVE,
# and with two it would be a three-way merge.
#
# DEPENDS db: the rollup is three queries over `agent_actions` and
# `agent_work_claims`. A layer-1-to-layer-1 edge onto `db` is the shape
# D17 (decision 943) explicitly permits, same as `scope_ref` -> `db`.

planar_module(activity_rollup
  INTERFACE activity_rollup.cppm
  SOURCES activity_rollup.cpp
  DEPENDS db)

if(TARGET planar_activity_rollup_tests)
  target_link_libraries(planar_activity_rollup_tests PRIVATE SQLite::SQLite3)
endif()
