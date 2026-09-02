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
#               `planar.cliapp.completion` defers flag-VALUE completion — and no
#               test claims it is.
#
#   ps          The six READ verbs task 6120 landed, all resting on
#   claims      `planar.engine.runtime.agentactivity`'s read paths. This is
#   actions     what finally gives the read-only handle a VERB CONSUMER:
#   plans       before them, level 2 above was proved only at the context
#   log         level, by a unit test that opened the handle and executed an
#   tree        `insert` through it. `handlers.t.cpp`'s read-only case now
#               runs a real verb end to end against a seeded database, in
#               both directions — the SELECT returns the seeded rows AND a
#               write through the same handle is refused — so "the handle
#               the verbs actually use is the read-only one" is an
#               observation rather than an inference.
#
# --- NOT landed, named rather than dropped silently ------------------------
#
# `feed`, `run` and `sync-events`, three of the oracle's twelve verbs:
#
#   feed          648 lines plus follow.zig's SIGINT loop. It is the DEFAULT
#                 verb, so its absence is also why a bare `planar-watch`
#                 renders root help here where the oracle streams the feed.
#   run           `run list` / `run show`, over `workflow_runs` +
#                 `context_records`. A different bucket entirely
#                 (`engine_runs`), not agentactivity — it was gated by this
#                 milestone only by association.
#   sync-events   Its query is over `sync_events` joined to `external_links`,
#                 both reachable from the ported `engine_external` bucket;
#                 what it borrowed from `ps` — the `generated_at` emitter —
#                 now exists at `handlers/format.cppm`. Cheapest next verb.
#
# `--follow` and `--interval` on every verb that declares them. The flags ARE
# declared (catalog parity compares declared flag sets), and the handler
# returns not_implemented (exit 64) rather than emitting one snapshot and
# exiting 0. A silent single-shot would read as a working stream to any
# script that pipes it. The arm needs a SIGINT handler and a poll/wake loop,
# neither of which exists anywhere in this tree.
#
# One consequence still visible in output, stated rather than hidden: the
# ROOT help page lists nine verbs where the oracle lists twelve. Leaf pages
# are pinned; the root page is not oracle-comparable.

planar_binary(planar_watch
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    handler.cppm
    tree.cppm
    surface.cppm
    handlers/format.cppm
    handlers/version.cppm
    handlers/schema.cppm
    handlers/completion.cppm
    handlers/feed.cppm
    handlers/ledger.cppm
    handlers/live.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    tree.cpp
    surface.cpp
    handlers/format.cpp
    handlers/version.cpp
    handlers/schema.cpp
    handlers/completion.cpp
    handlers/feed.cpp
    handlers/ledger.cpp
    handlers/live.cpp
    dispatch.cpp
  DEPENDS
    cliapp
    db
    json_text
    engine_planning
    engine_runtime)

# Each binary builds its own CLI::App tree directly (plan 996, task 6123),
# so the CLI11 module target is linked per binary rather than laundered
# through a shared layer-1 wrapper.
target_link_libraries(planar_cmd_planar_watch PRIVATE CLI11::CLI11_Module)
if(TARGET planar_cmd_planar_watch_tests)
  target_link_libraries(planar_cmd_planar_watch_tests PRIVATE CLI11::CLI11_Module)
endif()

if(TARGET planar_cmd_planar_watch_tests)
  target_compile_definitions(planar_cmd_planar_watch_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_watch>\""
    "PLANAR_ZIG_BIN=\"${PLANAR_ORACLE_ROOT}/zig/zig-out/bin/planar-watch\"")
  target_include_directories(planar_cmd_planar_watch_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  # `../catalog_parity.hpp` parses the two catalogs with Glaze (plan 996,
  # task 6065 brought this binary into the catalog comparison; before it
  # the tree declared three of the oracle's twelve verbs and `schema` was
  # explicitly excluded from the compared set).
  target_link_libraries(planar_cmd_planar_watch_tests PRIVATE glaze::glaze)
  add_dependencies(planar_cmd_planar_watch_tests planar_cmd_planar_watch)
endif()
