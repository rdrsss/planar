# Fixture (a): a generic upward dependency — a layer-1 base library
# depending on a layer-2 engine module. Plan 996 task 6050 (M2 pre-work),
# adapted from tabula's cmake/tests/architecture-guard-fixture (which only
# had one hardcoded module set to test against; Planar's D15 walk
# classifies layers by name pattern, so this fixture proves the general
# `_planar_module_layer()`-driven rule rather than a fixed module pair).
#
# A minimal, self-contained CMake project — not part of the real Planar
# build graph. Plain translation units, not C++ modules: the guard walks
# the configured *target graph*, not module import edges, and this fixture
# never gets built (only configured), so there is nothing to gain from
# paying C++26 module-scanning cost in a test that runs on every ctest
# invocation.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_upward CXX)

add_library(planar_engine_widget STATIC engine_widget.cpp)  # layer 2
add_library(planar_core STATIC core.cpp)                    # layer 1

# The induced violation: layer-1 `core` reaching up into layer-2
# `engine_widget` — the "render reaching for an element" shape D15
# forbids, the opposite direction of the intended cmd -> engine -> base
# spine.
target_link_libraries(planar_core PUBLIC planar_engine_widget)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_engine_widget planar_core)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
