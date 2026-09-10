# Fixture (j): positive case for F2 (plan 996, task 6078, M3 review
# remediation). The M3 boundary review reproduced a false rejection: for a
# STATIC library, CMake records a PRIVATE dependency as
# $<LINK_ONLY:planar_Y> inside INTERFACE_LINK_LIBRARIES — CMake's own
# generated plumbing, not caller-authored genex — and the pre-fix genex
# refusal in cmake/architecture.cmake matched THAT wrapper unconditionally,
# aborting configure on the ordinary, textbook spelling
# target_link_libraries(planar_X PRIVATE planar_Y) and telling the author
# to "replace it with a plain target name" when they already had. This was
# not latent: src/lib/engine/config/CMakeLists.txt already writes exactly
# this PRIVATE spelling and only escaped because none of its PRIVATE deps
# (glaze::glaze) contain the substring "planar_".
#
# This fixture proves the fix: a legal downward PRIVATE edge (layer-2
# `engine_widget` -> layer-1 `core`) using the ordinary PRIVATE keyword on
# a STATIC library must CONFIGURE CLEANLY, exactly like the PUBLIC spelling
# already does elsewhere in this fixture set.
#
# See ../upward/CMakeLists.txt for why this is plain translation units, not
# C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_private_link_legal CXX)

add_library(planar_core STATIC core.cpp)                    # layer 1
add_library(planar_engine_widget STATIC engine_widget.cpp)  # layer 2

# The legal edge under test: layer-2 depending PRIVATE (downward) on
# layer-1, on a STATIC library — the shape that made CMake wrap the
# recorded dependency in $<LINK_ONLY:...>.
target_link_libraries(planar_engine_widget PRIVATE planar_core)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_core planar_engine_widget)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to SUCCEED — that success is the pass condition
