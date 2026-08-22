# src/lib/ — tech-spec § File-level tree.
#
# `engine/` (identity, planning, workbench, ingest, runtime, external,
# config, ops, execute) lands progressively across milestones; `identity`
# (plan 996, task cpp-scope-assoc) is the first `engine_*` (layer 2, D15)
# module — cwd-derived scope resolution, the cross-scope guard's refusal
# decision, and the association read/write surface it needs. `core` is the
# M0 seed module proving the planar_module() + architecture-gate machinery
# configures and builds. `db` (plan 996, task cpp-db-wrapper) is the first
# real layer-1 base library; `cli` (plan 996, task cpp-cli-tree-parity) is
# the etcli command-tree/parser/help/completion port. `log` (plan 996, task
# cpp-cli-output-logging, D11) is the spdlog-backed text/JSON dual-mode
# logging setup. `engine/config` (plan 996, task 6032) is the config plane
# (`~/.planar/config.toml` + embedded defaults), the embedded-templates +
# operator-override resolution surface, and the init verb's engine-level
# project-registration behavior.

add_subdirectory(core)
add_subdirectory(db)
add_subdirectory(cli)
add_subdirectory(log)
add_subdirectory(engine/identity)
add_subdirectory(engine/config)
