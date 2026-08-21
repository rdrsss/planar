# src/lib/ — tech-spec § File-level tree.
#
# `engine/` (identity, planning, workbench, ingest, runtime, external,
# config, ops, execute) and the real `db`/`cli` ports land in later
# milestones; `core` is this task's seed module, proving the
# planar_module() + architecture-gate machinery configures and builds.

add_subdirectory(core)
