# Fixture (b): the engine.execute / planar_db exception. Plan 996 task 6050
# (M2 pre-work). `db` sits in layer 1 and `engine_execute` sits in layer 2,
# so `engine_execute -> db` is otherwise a perfectly legal *downward* edge
# under the generic layer rule — it is forbidden only by the hardcoded
# no-SQLite-handle exception in cmake/architecture.cmake
# (planar-execute must never hold a SQLite handle; it reaches Planar state
# only by shelling `planar`/`planar-agent`). This fixture proves that
# hardcoded exception actually fires, independent of the generic layer
# check proven by ../upward/.
#
# See ../upward/CMakeLists.txt for why this is plain translation units,
# not C++ modules.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_engine_execute_db CXX)

add_library(planar_db STATIC db.cpp)                    # layer 1
add_library(planar_engine_execute STATIC engine_execute.cpp)  # layer 2

# The induced violation: engine_execute reaching for planar_db — legal by
# layer number, forbidden by the named exception.
target_link_libraries(planar_engine_execute PUBLIC planar_db)

set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_engine_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
