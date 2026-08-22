# Fixture (e): the naming-coupling trap closed. Plan 996 task 6070 (M3
# pre-work). Hazard 2: cmake/architecture.cmake's no-SQLite-handle exception
# used to key SOLELY off the literal target name `cmd_planar_execute` —
# true only if the execute command binary is declared as
# `planar_binary(planar_execute ...)`. Calling planar_binary() the natural,
# unprefixed way — `planar_binary(execute ...)` — yields `planar_cmd_execute`
# instead. That name never matched the literal, so the exception silently
# never fired for that (equally real) binary, even though it embeds
# engine_execute exactly the same way. Nothing enforced or documented that a
# future src/cmd/execute/CMakeLists.txt had to spell the name one specific
# way to keep the safety invariant alive.
#
# The fix (cmake/architecture.cmake, same task) makes the exception ALSO
# derive from the real dependency edge: any target that actually links
# planar_engine_execute is treated as an execute carrier, independent of its
# own name. This fixture proves the trap is closed: it calls the REAL
# planar_binary() with the natural name "execute" (not "planar_execute") and
# DEPENDS engine_execute + db, and asserts the guard still rejects the
# resulting planar_cmd_execute -> planar_db edge.
#
# See ../planar-binary-real/CMakeLists.txt for why this needs the pinned
# toolchain flags (NEEDS_TOOLCHAIN in the top-level CMakeLists.txt) and
# ../cmd-binary-db/CMakeLists.txt for why the OLDER literal-name fixture is
# left as a manual add_executable() proxy rather than migrated to
# planar_binary() here.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_planar_binary_natural_name CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../")
include(module)  # the real planar_module() / planar_binary(), unmodified

add_library(planar_db STATIC db.cpp)                           # layer 1
add_library(planar_engine_execute STATIC engine_execute.cpp)   # layer 2
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_engine_execute)

# The natural, unprefixed call convention — deliberately NOT
# planar_binary(planar_execute ...). This is what a real
# src/cmd/execute/CMakeLists.txt would write if nothing forced the other
# spelling, yielding planar_cmd_execute (not planar_cmd_planar_execute).
planar_binary(execute SOURCES main.cpp DEPENDS engine_execute db)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
