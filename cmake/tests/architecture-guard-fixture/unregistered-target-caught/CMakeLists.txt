# Fixture (k): F3 (plan 996, task 6078, M3 review remediation). A raw
# add_library() intermediate named planar_* that is never registered into
# PLANAR_MODULE_TARGETS must not hide the edges routed through it. Before
# F3's fix, the adjacency walk had no entry for the unregistered
# `engine_middleware` at all (pass 1 only builds adjacency for targets in
# PLANAR_MODULE_TARGETS), so the cycle and layer walks silently stopped there
# and configure SUCCEEDED ("GUARD PASSED") against a genuine violation.
# module.cmake's "every planar_* target MUST use this helper" was a comment,
# not a check.
#
# After the fix, pass 1b notices that `planar_cmd_probe` depends on
# `planar_engine_middleware`, a planar_-prefixed name that IS a TARGET but is
# absent from the registered set, and refuses configure.
#
# See ../upward/CMakeLists.txt for why this is plain translation units, not
# C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_unregistered_target_caught CXX)

add_library(planar_db STATIC db.cpp)                                # layer 1, registered
add_library(planar_engine_middleware STATIC engine_middleware.cpp)  # layer 2, DELIBERATELY UNREGISTERED

# Legal in isolation, but planar_engine_middleware itself is never
# registered below, so the edge it contributes is invisible to the walk.
target_link_libraries(planar_engine_middleware PUBLIC planar_db)

add_executable(planar_cmd_probe main.cpp)  # layer 3 (^cmd_ prefix)
target_link_libraries(planar_cmd_probe PRIVATE planar_engine_middleware)

# planar_engine_middleware is DELIBERATELY OMITTED from this list — that
# omission is the induced defect under test.
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_cmd_probe)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
