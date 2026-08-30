# src/lib/ — tech-spec § File-level tree.
#
# `engine/` (identity, planning, workbench, ingest, runtime, external,
# config, ops, execute) lands progressively across milestones; `identity`
# (plan 996, task cpp-scope-assoc) is the first `engine_*` (layer 2, D15)
# module — cwd-derived scope resolution, the cross-scope guard's refusal
# decision, and the association read/write surface it needs. `core` is the
# M0 seed module proving the planar_module() + architecture-gate machinery
# configures and builds. `db` (plan 996, task cpp-db-wrapper) is the first
# real layer-1 base library. `cliapp` (plan 996, task 6123) is what
# survived deleting `cli`: the hand-rolled command-tree/parser/help/
# exit-table port was removed entirely in favour of CLI11, which each
# `src/cmd/<binary>/` now drives directly, and `cliapp` holds only the
# pieces CLI11 has no equivalent for and that are TOOLING contracts rather
# than parser concerns — the `<bin> schema` catalog `zig/tools/
# cli_usage_lint` consumes, the shell-completion generator, the shared
# CLI::App tree-walk primitives, the parsed-argv accessors (including the
# single surviving `parse_int64_zig`), and build-metadata/version
# rendering. See cliapp/CMakeLists.txt. `log` (plan 996, task
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
# extraction to layer 1 under D19). `engine/external` (plan 996, tasks 6106
# and 6041) is the operational-plane state surface: the `external_links`
# local-entity ↔ external-ticket binding (6106, stood up because `planar
# unlink` needed it — task 6105 recorded that leaf as "blocked on layer 3"
# and it was the only one of those six whose LAYER-2 dependency was ALSO
# missing), plus, from 6041, the `external_systems` registry and the
# FIELD-LEVEL sync/conflict engine behind `sync pull` / `sync push` /
# `sync resolve`. It depends on `db`, `adapter` and `json_text`, and NOT on
# `engine/extsync`: the sync engine takes the layer-1 `external_adapter&`
# interface rather than a concrete Jira/GitHub adapter, which is what keeps
# a layer-2 peer edge out of the graph. `agentingest/` stays deferred with
# `ext propagate` (see engine/external/CMakeLists.txt). `engine/ingest` (plan 996, task 6035) is
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
# `engine/models` (plan 996, task 6096) is the opaque candidate registry, the
# 95%-Wilson-lower-bound cohort ranking behind `models evals`, and the
# read-only routing-evidence views — thirteen of the fourteen `models` schema
# leaves. It depends only on `db`; `models resolve` is deferred with its
# roles/profile/packet subsystem (see engine/models/CMakeLists.txt).
# `engine/workflows` (plan 996, task 6096) is Lua-workflow discovery and
# `@meta` parsing behind `workflow list` / `workflow show`. It depends on
# NOTHING -- it holds no SQLite handle -- and resolves its directories from an
# explicit env-lookup callable rather than `std::getenv`, so it cannot reach a
# real `~/.planar` by accident. `workflow run` is deferred with its
# process-spawn dependency (see engine/workflows/CMakeLists.txt).
# `json_text` (plan 996, task 6109, decision D19) is a layer-1 base library
# holding the ONE JSON string-escaping primitive every hand-rolled renderer in
# the tree shares. It was extracted after the per-bucket duplication D18 forces
# (an `engine_*` may not depend on another `engine_*`) reached TEN copies under
# two different names — seven `append_json_string` (closure, grouping, ingest,
# models, planning, runs, workflows) and three `json_quote` (planning/
# annotation, runtime/capture, promotion), the latter three of which had
# silently DROPPED the `\b` and `\f` short forms. Task 6114 swept all ten:
# every renderer in the tree now imports this module and there is exactly one
# definition of the escape table (see json_text/CMakeLists.txt for the full
# account and the oracle evidence). Same shape as `scope_ref`: a shared engine
# primitive moved DOWN a layer so the same-layer prohibition can stay strict
# without duplication.
# `policy` (plan 1001, task 6100) is the layer-1 `audit_log` write path —
# `policy.audit.record`, the append-only "what happened, when, by whom"
# trail behind `planar audit trail`, session-capture correlation and
# ext-sync reconciliation. It closes an omission thirteen ported engine
# buckets had each recorded independently: the oracle's live database holds
# tens of thousands of these rows and the C++ binaries wrote none, which no
# exit code or stdout byte reveals. Layer 1 for the same forced D19 reason
# as `scope_ref` and `json_text` — every `engine_*` bucket needs it and a
# layer-2 -> layer-2 edge FATALs at configure time. DEPENDS db. Wired into
# `engine_planning` and `engine_identity` so far; the other buckets each
# still name it in their own CMakeLists as not-yet-wired (see
# policy/CMakeLists.txt).
# `git` (plan 996, tasks 6128 and 6137) is the layer-1 git-subprocess seam —
# `git -C <dir> <args>`, trimmed, with every non-zero exit collapsing to "no
# answer". Its absence is what deferred FOUR separate ports (`capture
# session`'s start-context stamp, the worktree gate, `bench harvest`,
# `capture commits`) and it had already been duplicated privately inside two
# `cmd_*` binaries (`handlers/init.cpp`'s `probe_git_origin`,
# `planar-agent/locality.cpp`'s `run_git`), both of which are now thin
# wrappers over it. Layer 1 is forced rather than chosen: `detect_worktree`
# is called from `engine/identity` (layer 2), which cannot reach a `cmd_*`
# copy without the layer-2 -> layer-3 edge D15 forbids. DEPENDS nothing.
# `process` (plan 996, task 6272) is the layer-1 process-spawn seam — the
# GENERAL runner `git` deliberately is not. Two shapes: `run_inherited`
# (fork/execv, the caller's own terminal, for a child that must stream) and
# `capture` (posix_spawnp, stdout piped, stderr discarded, for a child whose
# output is a value). Both keep "the program never ran" separate from "the
# program ran and failed", which `git` deliberately erases and which
# callers render as different refusals. Its absence was cited as the blocker
# on FIVE leaves; verified at task 6272, it was the real and only blocker on
# exactly ONE (`workflow run`) — `capture commits` and `bench harvest` are
# blocked on ~1100 and ~671 unported engine lines respectively, and `audit
# commits` needs `listFiltered`, which is pure SQL and not spawn-blocked at
# all. Both bodies moved DOWN from layer 3 unchanged in behavior; `editor`
# and `ext_adapter_factory` are now thin delegates. DEPENDS nothing.
# `engine/local` (plan 996, task 6109) is the operator-local skills/agents
# sandbox behind all five `planar local` leaves. It depends on NOTHING but
# `json_text` -- the sandbox is pure filesystem state and holds no SQLite
# handle -- and resolves its root from an explicit env-lookup callable, which
# matters more here than anywhere else in the tree because the surface writes
# symlinks into the operator's REAL `~/.claude` / `~/.codex` / `~/.copilot`
# (see engine/local/CMakeLists.txt for the `PLANAR_LOCAL_HOME` trap).
# `engine/workspace` (plan 996, task 6110) is org-association selection, the
# `$PLANAR_HOME/workspaces/<id>/` state-directory layout, and the
# diagnose-and-repair pass behind `workspace doctor` -- ONE of the five
# `workspace` leaves. `routing build`, `routing show` and `regenerate` are
# deferred on size and on a missing xxh64, and `workspace init` is
# architecturally blocked at layer 3 (see engine/workspace/CMakeLists.txt).

# `engine/workbench` (plan 996, task 6037) is the bidirectional workbench
# sync surface: the front-matter parser (and, more to the point, the set of
# malformed files it REFUSES, which is what `workbench pull` enforces), the
# DB<->FS classification engine behind pull/push/status/sync, the
# `workbench_sync_state` manifest and its `.sync` mirror, the terminal-status
# write-set filter, gc, archive/restore, and lint. It depends on `db` and
# `json_text` only. Two same-layer pressures were resolved rather than
# routed around -- the entity reads go straight to the tables (as the Zig
# original's do), and the one `workbench.root` config lookup is a narrow
# duplicated scan rather than an illegal edge onto `engine_config`. It is
# also the bucket that unblocks the three deferred ingest leaves (task 6119),
# which need `feature_dir_for`, `resolve_root` and `parse`. See
# engine/workbench/CMakeLists.txt for the full account, the filesystem-safety
# argument, and the three deferred verbs.

# `http` (plan 996, task 6041) is a layer-1 base library: the HTTP
# request/response vocabulary, an abstract `transport`, and a libcurl-backed
# `curl_transport` carrying the M7 30-second whole-request AND connect
# timeouts. Layer 1 for the D19 reason `scope_ref` and `json_text` are — two
# layer-2 buckets need it and D18 forbids the edge between them. It ships
# `fixture_server.hpp`, a test-only in-process HTTP/1.1 server bound to
# 127.0.0.1 on a kernel-assigned port, so `curl_transport` is exercised over a
# REAL socket that has no route off the machine (see http/CMakeLists.txt).
# `adapter` (plan 996, task 6041) is the other half of that extraction: the
# external-plane adapter BOUNDARY — `remote_state`, `field_change_set`,
# `local_entity`, credentials, and the four-operation `external_adapter`
# interface. It depends on NOTHING, not even `http`, which is what lets
# `engine/external`'s sync engine be unit-tested against a stub adapter with
# no HTTP anywhere in the picture.
# `engine/extsync` (plan 996, task 6041) is the two operational-plane
# adapters — Jira and GitHub Issues — each ported to its complete
# four-operation core (`validate`/`pull`/`push`/`render`). It depends on
# `adapter`, `http` and `json_text` and holds NO SQLite handle. The
# create/propagate half of both Zig files (GitHub's ProjectsV2 GraphQL arm,
# sub-issues, comments, issue creation; Jira's comment POST) is deferred with
# `ext create` / `ext propagate` (see engine/extsync/CMakeLists.txt).

add_subdirectory(core)
add_subdirectory(db)
add_subdirectory(cliapp)
add_subdirectory(log)
add_subdirectory(scope_ref)
add_subdirectory(json_text)
add_subdirectory(json_dom)
add_subdirectory(policy)
add_subdirectory(git)
add_subdirectory(process)
add_subdirectory(http)
add_subdirectory(adapter)
add_subdirectory(engine/identity)
add_subdirectory(engine/config)
add_subdirectory(engine/templates)
add_subdirectory(engine/planning)
add_subdirectory(engine/entitylink)
add_subdirectory(engine/external)
add_subdirectory(engine/extsync)
add_subdirectory(engine/ingest)
add_subdirectory(engine/promotion)
add_subdirectory(engine/runtime)
add_subdirectory(engine/runs)
add_subdirectory(engine/closure)
add_subdirectory(engine/grouping)
add_subdirectory(engine/models)
add_subdirectory(engine/workflows)
add_subdirectory(engine/local)
add_subdirectory(engine/workspace)
add_subdirectory(engine/workbench)
add_subdirectory(engine/execute)
add_subdirectory(engine/search)
add_subdirectory(engine/tree)
add_subdirectory(engine/health)
add_subdirectory(engine/introspection_adapters)
add_subdirectory(engine/introspect)
