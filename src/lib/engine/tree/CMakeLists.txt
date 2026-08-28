# planar_engine_tree — engine/tree bucket (plan 996, task 6278).
#
# Layer-2 `engine_*` module: may depend on layer-1 modules only, never
# sideways onto another `engine_*` bucket (D15/D18, FATAL at configure
# time). Three dependencies, all layer 1: `db`, `json_text`, `scope_ref`.
#
# Behavior-preserving port (D2) of zig/src/engine/tree.zig (1284 non-test
# lines) — the walker, the box-drawing text renderer, and the `--json`
# wire format. One leaf: `planar tree`.
#
# WHAT WAS PORTED WHOLE
#
#   - The three-channel walk (parent_plan_id, plan_id/parent_task_id,
#     entity_links(derives-from)) including the `distinct` + `left join`
#     that widens a plan's task set across a derives-from edge.
#   - The SCAFFOLD retention rule (the oracle's C-11 fix): a filtered-out
#     plan survives only when it holds a plan-or-task descendant, so a
#     plan hanging on to derived leaves alone is dropped.
#   - The `entity_links.id` ordering of derived leaves, which interleaves
#     kinds in creation order (cluster K-tree-render-drift, plan 351).
#   - Dirs-first render grouping, the Unicode connectors, the 80-BYTE
#     title truncation, and the summary footer's pluralisation.
#   - The agent-activity fold-in and its one-line sub-line.
#
# WHAT WAS DELIBERATELY NOT REPRODUCED, AND WHY
#
#   - `Filter.sort`. The oracle declares the field and a `--sort` flag and
#     NEVER READS IT: every query orders by `id` (scope roots by `slug`)
#     unconditionally. Confirmed by CAPTURE, not by reading the source —
#     `tree --sort updated` and `tree --sort bogus` are byte-identical to
#     a bare `tree`, and a bogus key does not refuse (exit 0). The flag is
#     still ACCEPTED at the handler so the argv contract is unchanged;
#     only the dead struct member is dropped. Filed as an oracle defect
#     rather than implemented, because implementing it would be a
#     behavior CHANGE and D2 forbids that in a port.
#
#   - The oracle's fixed-size render buffers. `renderChildList` uses a
#     4096-entry stack array and `renderNode` CLAMPS the child prefix into
#     a 512-byte buffer, so a tree wider than 4096 siblings overruns and
#     one deeper than ~128 levels silently renders a TRUNCATED indent.
#     Both are replaced with `std::vector` / `std::string`. This is the
#     one place the port is deliberately not byte-identical, and only
#     outside the oracle's own working range — inside it the bytes match.
#
# THE ACTIVITY ROLLUP IS DUPLICATED HERE ON PURPOSE
#
# The oracle's walker calls `engine.runtime.agentactivity.summary
# .forEntity`. In C++ that lives in `planar_engine_runtime`, and
# `engine_tree -> engine_runtime` is the sideways edge architecture.cmake
# FATALs on. `walk.cpp`'s `for_entity` is therefore a transcribed copy of
# those three queries, not a call. `planar_engine_runtime` ports the
# rollup's two siblings (`recent_actions_for_entity`,
# `claim_transitions_for_entity`) for `audit trail` but NOT the compact
# rollup, so this is currently the tree's ONLY copy and has nothing to
# drift from. The principled fix is a D19 extraction to layer 1 — the
# same move task 6089 made for scope resolution — and it belongs in a
# task of its own rather than smuggled into a port cycle.

planar_module(engine_tree
  INTERFACE
    walk.cppm
    tree.cppm
  SOURCES
    walk.cpp
  DEPENDS
    db
    json_text
    scope_ref)

if(TARGET planar_engine_tree_tests)
  target_link_libraries(planar_engine_tree_tests PRIVATE SQLite::SQLite3)
endif()
