# Fixture: a `planar_`-prefixed dependency name that is not a CMake target
# at all. Plan 996 task 6087.
#
# ../unregistered-target-caught/ covers the adjacent case — a name that IS
# a real target but was never routed through planar_module() /
# planar_binary(). Pass 1b tested TARGET-ness FIRST, so the case below
# fell through every check the guard performs:
#
#   * CMake itself does not error. An unresolvable link name becomes a raw
#     `-lplanar_scop_ref` flag and surfaces only at LINK time, with a much
#     worse diagnostic than a named configure failure.
#   * The layer walk (pass 3) classifies a dependency by NAME, and
#     `_planar_module_layer()` answers 1 for anything not matching
#     `^cmd_` or `^engine_`. So a typo'd base-library name reads as a
#     legal layer-1 dependency from every layer and passes silently.
#
# A one-letter typo in a real dependency (`planar_scope_ref` ->
# `planar_scop_ref`) is the realistic shape, and it is what this fixture
# induces. `planar_cmd_thing` is a genuine registered layer-3 target so
# the fixture has a real walk to run; the induced violation is only the
# unresolvable name.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_nontarget_dep CXX)

add_library(planar_cmd_thing STATIC core.cpp)  # layer 3

# The induced violation: `planar_scop_ref` names nothing. Under the
# pre-task-6087 pass 1b this configured cleanly and read as a legal
# layer-3 -> layer-1 edge.
target_link_libraries(planar_cmd_thing PUBLIC planar_scop_ref)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS planar_cmd_thing)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
