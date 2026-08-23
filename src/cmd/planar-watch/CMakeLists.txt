# planar_cmd_planar_watch — the `planar-watch` READ-ONLY viewer binary
# (plan 996, task 6107).
#
# See src/cmd/planar-agent/CMakeLists.txt for the shared-layer-3-vocabulary
# finding (short version: D18 forbids a `cmd_* -> cmd_*` edge, and the
# vocabulary that looks shared — `context`, `exit` — is exactly what the
# capability boundary makes different per binary; what is genuinely shared
# already lives at layer 1 in `planar.cli`).
#
# --- the read-only invariant, and how it is actually held ------------------
#
# CLAUDE.md: "`planar-watch` — read-only viewer. It opens SQLite via
# `file:?mode=ro`. This is a hard invariant, not a convention." The binary
# advertises it to operators in its own `--help` page, and
# zig/integration_tests/capability_boundary_test.zig states it as two
# INDEPENDENT levels. Both are held here, and neither subsumes the other:
#
#   level 1  THE VERB SET. `tree.cpp` registers no write verb, and
#            `tree.t.cpp` asserts that none of the twelve planar-agent write
#            verbs nor any of the seventeen planar planning-entity verbs
#            appears anywhere in the tree, at any depth. A watcher with only
#            planar-watch on PATH cannot touch anything, and that sentence is
#            true only because that function keeps it true.
#
#   level 2  THE HANDLE. `context.cpp` calls `db::connection::open_read_only`
#            — never `open` — so the SQLite driver itself refuses write SQL.
#            `context.t.cpp` proves this the only way that is not vacuous:
#            it EXECUTES a real `insert` through the handle this context
#            hands out and requires it to FAIL, and separately requires that
#            pointing the context at a nonexistent path does NOT create the
#            file (the operator binary's context would create AND migrate
#            it). Break-probed by swapping `open_read_only` for `open`.
#
# This target links `planar_db` deliberately: the read-only invariant is
# about HOW the handle is opened, not about having none. The binary that
# has none is planar-execute, and that one is enforced by
# cmake/architecture.cmake instead.
#
# --- verb subset -----------------------------------------------------------
#
#   version     The spine, plus the program-name prefix operators grep on.
#   schema      The boundary made machine-readable; a FRAGMENT renderer.
#   completion  The only ported leaf here with a required POSITIONAL and a
#               real failure path, and the two failure paths carry DIFFERENT
#               exit codes (a bad shell value is 2 — invalid input; a missing
#               positional is 1 — parse error, this binary's policy). That
#               pair is what makes a collapsed exit mapping impossible to
#               pass. Its generated SCRIPT is not oracle-byte-comparable —
#               `planar.cli.completion` defers flag-VALUE completion — and no
#               test claims it is.
#
# --- NOT landed: nine leaves, ONE bucket -----------------------------------
#
# `feed`, `ps`, `claims`, `actions`, `plans`, `log`, `tree` and `run` all read
# through zig `engine.runtime.agentactivity` (store/types/json — 5,702 lines
# across seven files), which this tree has never ported. That is the
# identical blocker task 6106 found under `planar dashboard`: one bucket
# gating nine leaves across two binaries, which makes it a milestone rather
# than something to chip at.
#
# `sync-events` is the near miss worth recording: its own query is over
# `sync_events` joined to `external_links`, both reachable from the ported
# `engine_external` bucket — but it reaches into `ps` for the shared
# `generated_at` emitter and interval parsing, and its `--follow` arm needs
# the SIGINT/poll loop `follow.zig` owns. Cheapest next read verb, not free.
#
# Two consequences visible in output, stated rather than hidden: the ROOT
# help page lists three verbs where the oracle lists twelve (not
# oracle-comparable; leaf pages are), and a BARE invocation renders root help
# where the oracle runs its default `feed` verb.

planar_binary(planar_watch
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    handler.cppm
    tree.cppm
    handlers/version.cppm
    handlers/schema.cppm
    handlers/completion.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    tree.cpp
    handlers/version.cpp
    handlers/schema.cpp
    handlers/completion.cpp
    dispatch.cpp
  DEPENDS
    cli
    db)

if(TARGET planar_cmd_planar_watch_tests)
  target_compile_definitions(planar_cmd_planar_watch_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_watch>\""
    "PLANAR_ZIG_BIN=\"${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar-watch\"")
  target_include_directories(planar_cmd_planar_watch_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_watch_tests planar_cmd_planar_watch)
endif()
