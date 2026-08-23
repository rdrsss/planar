# planar_cmd_planar_execute — the deterministic, spawn-free Lua workflow
# engine (plan 996, task 6107; plan 633 for the binary itself).
#
# --- THE DEPENDS LIST IS THE POINT OF THIS FILE ----------------------------
#
# It is EMPTY, and that is load-bearing rather than incidental. CLAUDE.md:
# planar-execute "holds NO SQLite handle — it reaches Planar state only by
# shelling `planar`/`planar-agent`" and "exposes NO model-spawning host
# function". An earlier planar-execute grew re-entrant headless LLM spawning
# and became a harness in its own right, which is why it was extracted to a
# separate project; the revival deliberately reins that back in.
#
# The no-SQLite half is enforced BY THE BUILD, not by review.
# cmake/architecture.cmake carries an explicit exception for the execute
# carrier, and it fires on this target through the LITERAL name arm
# (`_name STREQUAL "cmd_planar_execute"`) — which is why this target is
# declared as `planar_binary(planar_execute ...)` and not, say,
# `planar_binary(execute ...)`. planar_binary() prefixes with `planar_cmd_`,
# so THIS spelling yields `planar_cmd_planar_execute` and matches; the
# natural unprefixed spelling would yield `planar_cmd_execute`, silently
# miss the literal, and — with no `planar_engine_execute` target in the tree
# yet for the edge-derived arm to catch instead — leave the exception firing
# on nothing at all. That naming trap is documented at length in
# cmake/architecture.cmake's header (task 6070) and this is the first real
# target it applies to.
#
# So: adding `db` to DEPENDS below, or adding any dependency that reaches
# `planar_db` transitively, fails CONFIGURE with a named diagnostic. Not a
# review comment, not a runtime check — the build refuses. That is the
# strongest available evidence that this binary holds no handle, and it is
# stronger than any test, because a test can only observe the handle a
# binary chose to open.
#
# It also links no `cli`, and that too is inherited rather than a shortcut:
# zig/integration_tests/capability_boundary_test.zig states that
# planar-execute "uses manual arg parsing (not the etcli-zig COMMANDS
# table), so we cannot apply the parseHelpVerbs/assertExactSet pattern",
# and CLAUDE.md records that `tools/cli_usage_lint.zig` excludes this binary
# because it has NO `schema` catalog. Wiring it onto planar.cli would
# manufacture a catalog the reference binary does not have.
#
# --- what is landed --------------------------------------------------------
#
# The entire ENTRY POINT and argument surface, oracle-exact:
#
#   argv                       stdout  stderr                       exit
#   (none)                     —       usage                        2
#   --help / -h / help         —       usage                        0
#   <unknown>                  —       "unknown verb: X" + usage     2
#   run [bad args]             —       usage                        2
#   run wf.lua --phase p       —       "cannot read workflow: …"     1
#
# Note two shapes a reasonable port gets wrong, both captured rather than
# inferred: the usage text goes to STDERR even on `--help` (stdout stays
# empty — the opposite of the other three binaries), and a bare invocation
# prints the SAME bytes as `--help` but exits 2 instead of 0. The
# cannot-read-workflow path is exit 1 while every other failure is 2,
# because `LoadFailed` is not `BadUsage`; 0, 1 and 2 are all produced by
# this binary so a collapsed mapping cannot pass.
#
# --- what is deferred, with its dependency ---------------------------------
#
# Everything from `luaL_newstate()` onward: the sandboxed stdlib
# (`openSandboxedLibs`), the frozen host-fn manifest (`installHostSurface`,
# ALLOWED_HOST_FNS / DENIED_HOST_FNS), phase resolution, and the
# `flow.result` marshalling. There is no Lua in this tree —
# cmake/dependencies.cmake names "Lua 5.5 — planar-execute sandbox" in its
# inventory comment but declares no CPMAddPackage for it — and no
# `engine_execute` bucket under src/lib/engine/. A `run` whose file DOES
# read therefore exits 64 naming the gap, rather than printing `{}` and
# exiting 0: that is a real oracle outcome (a workflow declaring no result)
# and a caller could not tell it from a missing engine.
#
# When engine_execute lands it goes in DEPENDS here, and the edge-derived
# arm of the architecture guard starts protecting this target too — at which
# point the literal-name arm becomes belt-and-braces rather than the only
# guard.

planar_binary(planar_execute
  MAIN
    main.cpp
  INTERFACE
    cli.cppm
    engine.cppm
  SOURCES
    cli.cpp
    engine.cpp)

if(TARGET planar_cmd_planar_execute_tests)
  target_compile_definitions(planar_cmd_planar_execute_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_execute>\""
    "PLANAR_ZIG_BIN=\"${CMAKE_SOURCE_DIR}/zig/zig-out/bin/planar-execute\"")
  target_include_directories(planar_cmd_planar_execute_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_execute_tests planar_cmd_planar_execute)
endif()
