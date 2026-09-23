# Fixture: rule 3 of the Centurion boundary (plan 1033 task 6708). Planar's
# own edges are all legal — only planar-execute, only centurion::client — but
# a Centurion release whose client reaches the store TWO hops down (through
# its protocol library, via an alias and a PRIVATE edge) would put an
# execution component inside planar-execute. MUST fail, naming the chain.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_centurion_client_closure CXX)

add_library(centurion_store STATIC centurion.cpp)
add_library(centurion::store ALIAS centurion_store)
add_library(centurion_protocol STATIC centurion.cpp)
target_link_libraries(centurion_protocol PRIVATE centurion::store)
add_library(centurion_client STATIC centurion.cpp)
add_library(centurion::client ALIAS centurion_client)
target_link_libraries(centurion_client PUBLIC centurion_protocol)

add_library(planar_cmd_planar_execute STATIC a.cpp)
target_link_libraries(planar_cmd_planar_execute PRIVATE centurion::client)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS planar_cmd_planar_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR
