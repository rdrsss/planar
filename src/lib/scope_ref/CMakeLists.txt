# planar.scope_ref — layer-1 scope-reference parsing/normalisation
# (decision D19, plan 996 task 6089). Extracted out of `engine_identity`
# so that BOTH `engine_identity` and `engine_planning` (and any future
# engine bucket) can depend on it downward, per D19's "shared engine
# primitives are extracted into a layer-1 module" — D18's same-layer
# prohibition for layers 2/3 is UNCHANGED; this module is what lets that
# rule stay strict without duplicating the algorithm per bucket. Layer-1
# by name (cmake/architecture.cmake classifies anything not `cmd_*`/
# `engine_*` as layer 1), same as `db`/`cli`/`core`.
#
# DEPENDS db: this module's whole job is a DB-backed resolution primitive
# (the `global`/`repo:<slug>`/`assoc:<slug>`/bare-association grammar,
# resolved against the `projects`/`associations` tables) — see
# scope_ref.cppm's file header for why the full resolution (not just the
# pure-parsing half) was extracted, and why a layer-1-to-layer-1 edge onto
# `db` here is the same shape D15/D18 already permit (e.g. `cli` -> `core`).

planar_module(scope_ref
  INTERFACE scope_ref.cppm
  SOURCES scope_ref.cpp
  DEPENDS db)
