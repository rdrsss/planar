# Fixture (h): a genuine layer-1 dependency CYCLE. Plan 996 task 6069 (M3
# pre-work). D17 (decision 943, cmake/architecture.cmake's decision-block
# comment) permits layer-1-to-layer-1 edges (../layer1-same-layer/ proves
# the single-edge case), which reopens the possibility of a mutual cycle
# among layer-1 base libraries. D17's own body used to claim CMake/Ninja's
# generator refuses to build such a cycle — that claim was proven FALSE by
# an M2 reviewer probe (plan 996, task 6066): a mutual
# `planar_a<->planar_b` STATIC target_link_libraries cycle configures AND
# generates with exit 0 and no diagnostic at either step. CMake silently
# resolves a static-library cycle by re-listing the involved archives on
# the final link line as many times as needed.
#
# This fixture reproduces exactly that mutual cycle and proves
# cmake/architecture.cmake's own cycle detector (not CMake's generator)
# rejects it. The names `planar_a` / `planar_b` deliberately match the
# reviewer probe's naming from the D17 write-up rather than reusing
# `core`/`cli` (../layer1-same-layer/'s names) — this fixture proves a
# DIFFERENT, additional property (a cycle) than that one (a single legal
# same-layer edge), and giving it its own module names avoids any reader
# confusion that the two fixtures are testing the same thing.
#
# See ../upward/CMakeLists.txt for why this is plain translation units, not
# C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_layer1_cycle CXX)

add_library(planar_a STATIC a.cpp)  # layer 1
add_library(planar_b STATIC b.cpp)  # layer 1

# The induced violation: a mutual layer-1<->layer-1 cycle. Each individual
# edge is legal under D17 in isolation; only the cycle itself is the
# violation.
target_link_libraries(planar_a PUBLIC planar_b)
target_link_libraries(planar_b PUBLIC planar_a)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_a planar_b)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
