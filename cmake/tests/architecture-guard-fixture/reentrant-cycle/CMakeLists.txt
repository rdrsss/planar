# Fixture: planar_check_architecture() called TWICE in one configure, with
# a dependency cycle introduced between the two calls. Plan 996 task 6074.
#
# CMake has no local mutable containers, so the cycle detector's
# white/gray/black colouring lives in GLOBAL properties
# (`_planar_arch_cycle_state_<name>`). Pass 2 skips any node whose state is
# already set — that is what makes the DFS linear — so a second invocation
# inherited every node the FIRST call had finished as `black` and walked
# nothing at all. A cycle introduced after the first call, among nodes the
# first call had already visited, was therefore silently not detected:
# "GUARD PASSED" on a graph ../layer1-cycle/ proves is a violation.
#
# The shape below is the minimal reproduction. Call 1 sees `a -> b`, which
# is a perfectly legal layer-1-to-layer-1 edge (../layer1-same-layer/ is
# the positive control for exactly that), and colours both nodes black.
# The `b -> a` edge is then added, closing the cycle, and call 2 must
# refuse. Without the reset at the top of planar_check_architecture(),
# call 2 finds both nodes already black, walks nothing, and CONFIGURES
# CLEANLY — which is what makes this a discriminating fixture rather than
# a second copy of ../layer1-cycle/.
#
# Nothing in the shipped build calls the guard twice today (the top-level
# CMakeLists.txt calls it once, last). This fixture exists so that stays a
# free choice rather than a load-bearing invariant nothing enforces.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_reentrant_cycle CXX)

add_library(planar_a STATIC a.cpp)  # layer 1
add_library(planar_b STATIC b.cpp)  # layer 1

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS planar_a planar_b)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")

# Call 1: a legal single layer-1 edge. MUST succeed — if this one FATALs
# the fixture is testing the wrong thing.
target_link_libraries(planar_a PUBLIC planar_b)
planar_check_architecture()

# Call 2: the back-edge closes a cycle. MUST FATAL.
target_link_libraries(planar_b PUBLIC planar_a)
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
