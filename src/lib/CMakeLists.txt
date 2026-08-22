# src/lib/ — tech-spec § File-level tree.
#
# `engine/` (identity, planning, workbench, ingest, runtime, external,
# config, ops, execute) and the real `cli` port land in later milestones;
# `core` is the M0 seed module proving the planar_module() +
# architecture-gate machinery configures and builds. `db` (plan 996, task
# cpp-db-wrapper) is the first real layer-1 base library.

add_subdirectory(core)
add_subdirectory(db)
