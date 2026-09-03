# planar_cmd_planar_ext — the `planar-ext` binary, STEP 1 of the
# external-plane extraction (plan 996, task 6418; task 6412 and decisions
# 995-1000).
#
# This is the FIFTH cmd_ target, and — like its three siblings — gets no
# shared layer-3 vocabulary: D18 makes a `cmd_* -> cmd_*` edge a
# configure-time FATAL (cmake/architecture.cmake), and `context`, `exit`,
# `handler`, `dispatch`, `tree` are duplicated here for the same reason
# `src/cmd/planar-agent/CMakeLists.txt` argues at length — the four (now
# five) binaries do not actually share this vocabulary; what looks shared
# is exactly what each binary's capability boundary makes different.
#
# --- what THIS task lands, and what it deliberately does not -------------
#
# Two leaves only: `version` and `schema`. No `ext`/`sync` verb moves here
# in this task — `planar` keeps the complete surface until tasks
# 6419-6421 move the ~10,700 lines of engine code and the verbs that use
# it, in the SAME cycle each does. Landing the skeleton and the code
# motion together would make any failure ambiguous between "the binary is
# broken" and "the moved code is broken"; this task proves the binary
# builds, is policed by cli_usage_lint, and enforces its write boundary,
# before a single verb depends on any of that being right.
#
# Because of that scope, DEPENDS below names only `cliapp` and `db` — no
# `engine_*` bucket. Nothing here has DB access to a planning bucket to
# depend on yet; tasks 6419-6421 add exactly the engine DEPENDS the moved
# code needs, at the point real handlers first call into it.
#
# --- the write-capability boundary (decision 995) -------------------------
#
# `planar.cmd.planar_ext.context::ensure_db` installs
# `db::connection::restrict_writes_to({"external_links",
# "external_systems", "sync_events"})` on every connection this binary
# opens — read-write on exactly those three tables, enforced by SQLite's
# own authorizer at prepare time, everything else read-only. See that
# module's header and `restrict_writes_to`'s own doc comment
# (src/lib/db/db.cppm) for why this is a RUNTIME enforcement rather than a
# call-site convention: a source-text audit already measured this write
# surface wrong once, missing `src/lib/engine/external/sync.cpp`'s
# `table_for`-interpolated `UPDATE`. `capability.t.cpp` is the test that
# proves the boundary holds, against real prepared statements.

planar_binary(planar_ext
  MAIN
    main.cpp
  INTERFACE
    exit.cppm
    context.cppm
    handler.cppm
    tree.cppm
    handlers/version.cppm
    handlers/schema.cppm
    dispatch.cppm
  SOURCES
    context.cpp
    tree.cpp
    handlers/version.cpp
    handlers/schema.cpp
    dispatch.cpp
  DEPENDS
    cliapp
    db)

# Each binary builds its own CLI::App tree directly, so the CLI11 module
# target is linked per binary rather than laundered through a shared
# layer-1 wrapper (plan 996, task 6123's finding, reproduced here).
target_link_libraries(planar_cmd_planar_ext PRIVATE CLI11::CLI11_Module)
if(TARGET planar_cmd_planar_ext_tests)
  target_link_libraries(planar_cmd_planar_ext_tests PRIVATE CLI11::CLI11_Module SQLite::SQLite3)
  target_include_directories(planar_cmd_planar_ext_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_ext_tests planar_cmd_planar_ext)
endif()
