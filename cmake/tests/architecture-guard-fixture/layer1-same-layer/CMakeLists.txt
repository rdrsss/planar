# Fixture (d): positive case for the layer-1 interdependency decision
# (plan 996 task 6050, M2 pre-work, architecture.cmake's decision-block
# comment). Unlike the other fixtures in this directory, this one is
# expected to configure SUCCESSFULLY — it proves the decision is actually
# implemented, not just written down: a layer-1 base library (`cli`)
# depending on another layer-1 base library (`core`) must NOT trip D15,
# because base libraries are allowed to build on each other.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_layer1_same_layer CXX)

add_library(planar_core STATIC core.cpp)  # layer 1
add_library(planar_cli STATIC cli.cpp)    # layer 1

target_link_libraries(planar_cli PUBLIC planar_core)  # layer-1 -> layer-1, allowed

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_core planar_cli)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to SUCCEED — that success is the pass condition
