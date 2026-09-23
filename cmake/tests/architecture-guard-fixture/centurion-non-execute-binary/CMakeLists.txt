# Fixture: rule 2 of the Centurion boundary (plan 1033 task 6708). A
# layer-1 bridge library links centurion::client — legal on its own — and the
# `planar` binary (not an execute carrier) links the bridge, so it reaches the
# client only TRANSITIVELY. MUST fail, naming the binary and the bridge.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_centurion_non_execute_binary CXX)

add_library(centurion_client STATIC centurion.cpp)
add_library(centurion::client ALIAS centurion_client)

add_library(planar_centurion_bridge STATIC a.cpp)
target_link_libraries(planar_centurion_bridge PUBLIC centurion::client)
add_library(planar_cmd_planar STATIC a.cpp)
target_link_libraries(planar_cmd_planar PRIVATE planar_centurion_bridge)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_centurion_bridge planar_cmd_planar)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR
