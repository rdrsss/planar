# Fixture: the Centurion boundary's POSITIVE control (plan 1033 task 6708).
# planar-execute reaches centurion::client through engine_execute, and the
# client's closure holds only client-side Centurion targets. MUST configure.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_centurion_client_legal CXX)

# Stand-ins for Centurion's real target names and aliases.
add_library(centurion_protocol STATIC centurion.cpp)
add_library(centurion::protocol ALIAS centurion_protocol)
add_library(centurion_client STATIC centurion.cpp)
add_library(centurion::client ALIAS centurion_client)
target_link_libraries(centurion_client PUBLIC centurion::protocol)

add_library(planar_engine_execute STATIC a.cpp)
target_link_libraries(planar_engine_execute PRIVATE centurion::client)
add_library(planar_cmd_planar_execute STATIC a.cpp)
target_link_libraries(planar_cmd_planar_execute PRIVATE planar_engine_execute)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_engine_execute planar_cmd_planar_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()
