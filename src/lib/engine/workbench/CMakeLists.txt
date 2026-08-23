# planar_engine_workbench — engine/workbench bucket (plan 996, task 6037, M5).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition, unchanged by D19).
#
# Behavior-preserving port (D2) of zig/src/engine/workbench/{parse,render,
# feature,terminal,manifest,sync,gc,lint}.zig plus the printing halves of
# zig/src/cmd/planar/handlers/workbench/*.zig.
#
# Eleven C++ module names compile into this ONE target, following the
# precedent `engine_config` documents at length: architecture.cmake
# classifies by TARGET name, so splitting them into separate targets would
# put them in the same layer as each other and every internal edge would
# need a D17 exception it does not qualify for. Multiple module names in one
# static library is fine — only the CMake target graph is walked.
#
# ---------------------------------------------------------------------------
# THE SAME-LAYER PRESSURE, AND HOW IT WAS RESOLVED
#
# This bucket sits between the planning entities and the filesystem, so it
# looks like it wants `engine_planning`. It does not get it, and there were
# exactly two places the pressure showed up:
#
#   1. Entity reads and writes. `sync` renders plans, tasks, artifacts,
#      decisions, questions and scenarios, and writes bodies/statuses back on
#      `pull`. It reaches all of that through its OWN SQL against `planar.db`,
#      exactly as the Zig original does — sync.zig never calls a planning verb
#      either. So this is NOT a duplicated abstraction; the shipped design
#      already talks to the tables directly, and `DEPENDS db` covers it. The
#      one thing that IS deliberately duplicated is the entity-status read
#      (`fetch_status`), which exists in both sync.cpp and gc.cpp, mirroring
#      the Zig files' own narrow duplication and its stated reason ("keep gc
#      self-contained").
#
#   2. Reading `workbench.root` out of `~/.planar/config.toml`.
#      `planar.engine.config.toml` already produces exactly that flattened
#      key — and it is a layer-2 peer, so the edge would FATAL at configure
#      time. D19's remedy (extract the primitive down to layer 1) is not
#      proportionate for ONE string key out of one document: it would drag
#      Glaze and the whole flattening pass down a layer to serve a single
#      lookup. So root.cpp carries a narrow, single-key scan instead,
#      following the precedent `engine_grouping` set when it duplicated its
#      `closures` read rather than depend on `engine_closure`. It is
#      documented as not-a-TOML-parser at the definition, and its accepted
#      shapes were oracle-probed (`[workbench]` section, top-level dotted
#      key, `~/` expansion, empty-value fallthrough).
#
# `json_text` (layer 1, D19) supplies the one JSON string-escaping primitive
# for the operator-facing renderers. The ONE payload that does NOT use it is
# `sync_events.context_json`, which reproduces the Zig original's raw
# interpolation byte for byte; see the comment on `render_conflict_context`
# in sync.cpp for why matching the oracle wins over producing stricter JSON
# on a string this module both writes and reads back itself.
#
# ---------------------------------------------------------------------------
# SHA-256 IS IMPLEMENTED HERE
#
# The manifest's whole drift-detection mechanism is a content hash, and none
# of the four vendored dependencies (Catch2, SQLite, Glaze, spdlog) exposes
# one. Adding a crypto dependency for a 64-character fingerprint would be out
# of proportion, so manifest.cpp carries a ~90-line FIPS 180-4 SHA-256. It is
# pinned in manifest.t.cpp against the standard empty-string and "abc"
# vectors AND against digests taken from a real `.sync` file the oracle
# wrote, so a transcription slip fails a test rather than silently making
# every file look drifted.
#
# ---------------------------------------------------------------------------
# FILESYSTEM SAFETY
#
# This is the highest-risk bucket in the port: `archive` deletes a directory
# tree and `gc` unlinks files, both under a root that ultimately comes from
# the environment. The protection is structural rather than a test
# convention, matching `engine_workflows` and `engine_local`:
#
#   - `planar.engine.workbench.root` is the ONLY module that decides where
#     the workbench lives, and it takes an injected env-lookup callable and
#     an injected file reader. There is no `std::getenv` anywhere in this
#     bucket.
#   - Every other entry point — `sync::push`, `gc::run_for_plan`,
#     `sync::archive`, … — takes the resolved root as an explicit parameter.
#     The Zig originals call `resolveWorkbenchRoot` from inside `run`,
#     reaching `std.c.environ`; that is the one shape deliberately NOT
#     transliterated.
#   - `fsutil::delete_tree` refuses anything that is not a real directory,
#     symlinks included (`symlink_status`, not `status`).
#
# ---------------------------------------------------------------------------
# NOT PORTED, named rather than silently dropped:
#
#   - `workbench publish` (zig .../handlers/workbench/publish.zig). It
#     renders workbench files THROUGH the propagation templates and pushes
#     them to a registered external system. The adapter surface it needs
#     does not exist in the C++ tree — `engine_external` ports only the
#     `external_links` create/show/delete rows and explicitly defers the
#     external-system/adapter surfaces — so there is nothing to call. It
#     lands with the adapters.
#
#   - `workbench extract-questions` (402 lines). A self-contained read-only
#     Markdown pass that scrapes `## Open questions` sections out of
#     top-level specs. Nothing blocks it architecturally; it is deferred on
#     size, and it shares no code with the sync engine.
#
#   - `workbench edit`. Spawns `$EDITOR` as a subprocess. Same
#     process-spawn dependency that defers `workflow run` (see
#     engine/workflows/CMakeLists.txt) and `bench harvest` (engine/runs).
#
#   Each of the three is absent from the layer-3 command tree as well, so
#   the `workbench` group's help page lists ten commands where the oracle
#   lists thirteen — the same accounted-for divergence `workflow` and
#   `workspace` already carry. No PORTED leaf drops a flag.
#
#   - `policy.audit.record` calls. There is still no `policy.audit` module
#     in the C++ tree; engine_planning, engine_identity, engine_promotion,
#     engine_runtime, engine_runs, engine_models, engine_workflows and
#     engine_local all document the same omission (task 6100, pending an
#     operator decision). No `workbench` leaf reads or emits audit rows.

planar_module(engine_workbench
  INTERFACE
    parse.cppm
    render.cppm
    feature.cppm
    terminal.cppm
    fsutil.cppm
    root.cppm
    manifest.cppm
    sync.cppm
    gc.cppm
    lint.cppm
    render_cli.cppm
    workbench.cppm
  SOURCES
    parse.cpp
    render.cpp
    feature.cpp
    terminal.cpp
    fsutil.cpp
    root.cpp
    manifest.cpp
    sync.cpp
    gc.cpp
    lint.cpp
    render_cli.cpp
  DEPENDS
    db
    json_text)
