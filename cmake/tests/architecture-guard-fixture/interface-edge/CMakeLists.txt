# Fixture (f): INTERFACE edges are invisible to a LINK_LIBRARIES-only walk.
# Plan 996 task 6069 (M3 pre-work). The M2 reviewer proved this by flipping
# ../upward/'s PUBLIC keyword to INTERFACE — the exact same upward edge
# (layer-1 `core` reaching into layer-2 `engine_widget`) configured cleanly,
# because `target_link_libraries(... INTERFACE ...)` only appends to the
# `INTERFACE_LINK_LIBRARIES` target property (what gets propagated to
# consumers), never to `LINK_LIBRARIES` (what is actually used to build the
# target itself) — and cmake/architecture.cmake used to read only the
# latter.
#
# This fixture is deliberately a SEPARATE file from ../upward/, not an edit
# to it: task 6069's constraints require every pre-existing fixture to keep
# passing UNCHANGED, and ../upward/ already stands as the PUBLIC-keyword
# proof — this one is the INTERFACE-keyword proof, added alongside it.
#
# See ../upward/CMakeLists.txt for why this is plain translation units, not
# C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_interface_edge CXX)

add_library(planar_engine_widget STATIC engine_widget.cpp)  # layer 2
add_library(planar_core STATIC core.cpp)                    # layer 1

# The induced violation: layer-1 `core` reaching up into layer-2
# `engine_widget`, exactly like ../upward/, but via the INTERFACE keyword —
# never touching LINK_LIBRARIES, only INTERFACE_LINK_LIBRARIES.
target_link_libraries(planar_core INTERFACE planar_engine_widget)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_engine_widget planar_core)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
