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
# logging setup. `scope_ref` (plan 996, task 6089, decision D19) is a
# layer-1 base library holding the scope-ref grammar (`global` /
# `repo:<slug>` / `assoc:<slug>` / bare-association forms) and its DB
# lookup, extracted out of `engine/identity` so `engine/identity` AND
# `engine/planning` can both depend on it downward instead of duplicating
# it or reaching sideways across engine buckets (forbidden by D18).
# `engine/config` (plan 996, task 6032) is the config plane
# (`~/.planar/config.toml` + embedded defaults), the embedded-templates +
# operator-override resolution surface, and the init verb's engine-level
# project-registration behavior. `engine/planning` (plan 996, task
# cpp-planning-verbs) is the plan/task CRUD surface, the plan/task status
# transition matrices, and the plan-304 auto-promotion invariant; it does
# NOT depend on `engine/identity` (a layer-2-to-layer-2 edge is forbidden
# by D15/D18 — see engine/planning/CMakeLists.txt). `engine/entitylink`
# (plan 996, task cpp-entity-links) is the generic cross-cutting
# `entity_links` CRUD primitive plus the `task_touch_paths` path-level
# "touches" surface; it depends only on `db` (same D15/D18 same-layer
# prohibition — see engine/entitylink/CMakeLists.txt for why it needed no
# extraction to layer 1 under D19). `engine/ingest` (plan 996, task 6035) is
# the READ side of the spec-ingest pipeline — workbench markdown parsing, the
# proposed-entity diff, the `--strict` gate's coverage object, the text/JSON
# preview renderers, and the provenance-bearing routing-fact materializer. It
# depends only on `db`; the pipeline's APPLY step is deliberately absent,
# because it composes six other layer-2 engine buckets and therefore has no
# legal home inside one (see engine/ingest/CMakeLists.txt). `engine/runs`
# (plan 996, task 6095) is the run-record lifecycle behind `planar bench *`
# and `planar run *` — both verb families write the SAME `runs` /
# `run_events` / `run_touches` tables (`run` does NOT use `workflow_runs`,
# despite the name) and differ only in their surface. It depends only on
# `db`; `bench harvest` is deferred with its git-subprocess dependency (see
# engine/runs/CMakeLists.txt). `engine/closure` (plan 996, task 6095) is the
# read side of a task's derived symbol-level closure (`closure show`); the
# `closure compute` extractor is deferred with its tree-sitter, filesystem-walk,
# and cross-bucket-handler dependencies (see engine/closure/CMakeLists.txt). `engine/grouping` (plan 996, task 6095)
# is the closure-replication-minimizing slice recommender behind `groups
# recommend`; it reads the same `closures` table as `engine/closure` through
# its OWN query rather than depending on that bucket (a layer-2-to-layer-2
# edge would FATAL), mirroring what the Zig originals do. The optional
# `mtkahypar` solver arm is deferred with its subprocess dependency.

add_subdirectory(core)
add_subdirectory(db)
add_subdirectory(cli)
add_subdirectory(log)
add_subdirectory(scope_ref)
add_subdirectory(engine/identity)
add_subdirectory(engine/config)
add_subdirectory(engine/planning)
add_subdirectory(engine/entitylink)
add_subdirectory(engine/ingest)
add_subdirectory(engine/promotion)
add_subdirectory(engine/runtime)
add_subdirectory(engine/runs)
add_subdirectory(engine/closure)
add_subdirectory(engine/grouping)
