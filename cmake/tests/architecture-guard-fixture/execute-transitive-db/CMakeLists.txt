# Fixture (i): the execute-carrier's no-SQLite-handle exception must be
# TRANSITIVE, not just direct-link. Plan 996 task 6069 (M3 pre-work,
# closing a scope gap task 6070 documented but deliberately left open —
# see cmake/architecture.cmake's header comment, "Transitive closure").
#
# `planar_cmd_execute` links TWO things directly: `planar_engine_execute`
# (making it an execute carrier — already caught by the direct-link scan
# even before this task) and `planar_engine_middleware`, an ordinary,
# otherwise-fully-legal layer-2 engine bucket that itself legitimately
# depends on `planar_db` (a normal, downward, unexceptional edge — nothing
# forbids an engine bucket from using the database). Neither individual
# edge out of `planar_cmd_execute` is `planar_db` directly; `planar_db` is
# only reachable by walking through `planar_engine_middleware`.
#
# Before this task's closure-aware fix, the no-SQLite-handle exception only
# ever inspected `planar_cmd_execute`'s OWN direct links for `planar_db`,
# so this exact edge — the execute binary ending up with a SQLite handle
# in its link closure via a perfectly legal intermediate module — silently
# passed. After the fix, the walk asks "does this carrier's full
# transitive closure contain planar_db?", which correctly reaches through
# `planar_engine_middleware` and rejects it.
#
# Deliberately a manual add_executable() + PLANAR_MODULE_TARGETS
# registration, like ../cmd-binary-db/, rather than the real
# planar_binary() — see that fixture's comment for why (this one needs no
# toolchain wiring either, for the same reason). The target is named
# `planar_cmd_execute`, NOT `planar_cmd_planar_execute` — it deliberately
# does not match the literal-name exception, so this fixture proves the
# edge-derived/closure-aware path specifically, independent of the literal.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_execute_transitive_db CXX)

add_library(planar_db STATIC db.cpp)                              # layer 1
add_library(planar_engine_execute STATIC engine_execute.cpp)      # layer 2
add_library(planar_engine_middleware STATIC engine_middleware.cpp)  # layer 2

# Legal in isolation: an ordinary engine bucket depending on db, downward.
target_link_libraries(planar_engine_middleware PUBLIC planar_db)

add_executable(planar_cmd_execute main.cpp)  # layer 3 (^cmd_ prefix)
target_link_libraries(planar_cmd_execute PRIVATE
  planar_engine_execute      # makes this target an execute carrier
  planar_engine_middleware)  # NOT planar_db directly — only transitively

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_engine_execute planar_engine_middleware planar_cmd_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
