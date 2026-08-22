# src/lib/ — tech-spec § File-level tree.
#
# `engine/` (identity, planning, workbench, ingest, runtime, external,
# config, ops, execute) lands in later milestones; `core` is the M0 seed
# module proving the planar_module() + architecture-gate machinery
# configures and builds. `db` (plan 996, task cpp-db-wrapper) is the first
# real layer-1 base library; `cli` (plan 996, task cpp-cli-tree-parity) is
# the etcli command-tree/parser/help/completion port. `log` (plan 996, task
# cpp-cli-output-logging, D11) is the spdlog-backed text/JSON dual-mode
# logging setup.

add_subdirectory(core)
add_subdirectory(db)
add_subdirectory(cli)
add_subdirectory(log)
