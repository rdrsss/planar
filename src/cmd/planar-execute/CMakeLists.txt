# planar_cmd_planar_execute — the deterministic, spawn-free Lua workflow
# engine (plan 996, task 6107; plan 633 for the binary itself).
#
# --- THE DEPENDS LIST IS THE POINT OF THIS FILE ----------------------------
#
# It names exactly ONE module, and that is load-bearing rather than
# incidental. (It was EMPTY through task 6107, when the Lua half was still
# deferred; task 6042 added `engine_execute` and nothing else.) CLAUDE.md:
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
# It links `cliapp` for ONE reason — `planar.cliapp.schema`'s emitter, so
# `planar-execute schema` prints the same flat JSON catalog the other four
# binaries do (decision 1030 / D18, task 6486) — and NOT for parsing. The
# argument surface stays hand-rolled (cli.cppm explains why; the Zig
# original "uses manual arg parsing (not the etcli-zig COMMANDS table)"
# and its usage/exit-code bytes are pinned). Until D18 this binary was
# exempt from `cli_usage_lint` precisely because it had no catalog; that
# exemption is closed. `cliapp` reaches `json_text` and CLI11 only, never
# `planar_db`, so the guard below is untouched.
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
# --- the Lua engine is no longer deferred ----------------------------------
#
# Task 6107 landed this binary with everything from `luaL_newstate()` onward
# missing, and said so: no Lua in the tree, no `engine_execute` bucket, so a
# `run` whose file DID read exited 64 naming the gap rather than printing
# `{}` and exiting 0 (a real oracle outcome, and therefore a silent wrong
# answer if faked).
#
# Task 6042 closed it. `cmake/dependencies.cmake` pins Lua 5.5.0 and
# `src/lib/engine/execute/` holds the sandbox, the frozen twenty-five-entry
# host surface and the run loop. Exit code 64 is gone from this binary.
#
# The DEPENDS below is what that closure looks like, and it is also the
# moment this file predicted: with `engine_execute` linked, the architecture
# guard's EDGE-DERIVED execute-carrier arm now fires on this target, so the
# no-SQLite-handle rule reaches it by both routes and the literal-name arm is
# finally belt-and-braces rather than the only guard. Adding `db` here still
# FATALs at configure — now for two independent reasons. (The DEPENDS list is
# still not empty-and-load-bearing the way the header above describes; what
# is load-bearing now is that it names exactly ONE thing, and that thing is
# the module the guard watches.)
#
# One host function's BODY is still unported and reports so at the point of
# call: `ctx.brief` (3,600 lines of brief/schema/state rendering in the Zig
# original). It is REGISTERED, not omitted — see
# src/lib/engine/execute/CMakeLists.txt for why an absent registration would
# have been the worse choice.

planar_binary(planar_execute
  MAIN
    main.cpp
  INTERFACE
    cli.cppm
    catalog.cppm
    engine.cppm
    selector.cppm
    profile.cppm
    host.cppm
    compat.cppm
  SOURCES
    cli.cpp
    catalog.cpp
    engine.cpp
    selector.cpp
    profile.cpp
    host.cpp
    compat.cpp
    client_probe.cpp
  DEPENDS
    cliapp
    engine_execute
    # sha256 (layer 1) digests the profile's bundle tree and command policy for
    # the compatibility tuple (task 6503). It reaches nothing near planar_db.
    sha256)

# Glaze writes `profile show --json` (selector.cpp, plan 1033 task 6485).
# Header-only JSON; it reaches nothing near `planar_db`, so the execute-
# carrier guard above is untouched.
target_link_libraries(planar_cmd_planar_execute PRIVATE glaze::glaze)

# centurion::client and NOTHING else of Centurion (tech-spec D1). This is the
# edge cmake/architecture.cmake's Centurion boundary exists to police: rule 1
# refuses any other centurion_* target, rule 2 refuses a non-execute binary
# reaching this one, and rule 3 grades the client's own closure for execution
# components. `client_probe.cpp` is the only translation unit that uses it.
target_link_libraries(planar_cmd_planar_execute PRIVATE centurion::client)

if(TARGET planar_cmd_planar_execute_tests)
  target_link_libraries(planar_cmd_planar_execute_tests PRIVATE glaze::glaze centurion::client)
  target_compile_definitions(planar_cmd_planar_execute_tests PRIVATE
    "PLANAR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_execute>\""
    # golden.t.cpp (plan 1033 M0, task 6484): the committed fixtures and the
    # shipped workflows they were captured from. Source paths, so a
    # PLANAR_GOLDEN_UPDATE run rewrites the tracked files directly.
    "PLANAR_GOLDEN_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/golden\""
    "PLANAR_WORKFLOWS_DIR=\"${PROJECT_SOURCE_DIR}/workflows\"")
  target_include_directories(planar_cmd_planar_execute_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
  add_dependencies(planar_cmd_planar_execute_tests planar_cmd_planar_execute)
endif()
