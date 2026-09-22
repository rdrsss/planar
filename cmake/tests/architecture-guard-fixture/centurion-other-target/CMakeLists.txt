# Fixture: rule 1 of the Centurion boundary (plan 1033 task 6708). The
# execute engine links centurion::store — an execution component — instead of
# (here, alongside) the client. MUST fail, naming the store edge.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_centurion_other_target CXX)

add_library(centurion_client STATIC centurion.cpp)
add_library(centurion::client ALIAS centurion_client)
add_library(centurion_store STATIC centurion.cpp)
add_library(centurion::store ALIAS centurion_store)

add_library(planar_engine_execute STATIC a.cpp)
target_link_libraries(planar_engine_execute PRIVATE centurion::client centurion::store)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS planar_engine_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR
