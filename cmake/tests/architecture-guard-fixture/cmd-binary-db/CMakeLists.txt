# Fixture (c): add_executable() coverage. Plan 996 task 6050 (M2
# pre-work), deliverable 2. `planar_check_architecture()` only ever walks
# `PLANAR_MODULE_TARGETS`, so a raw add_executable() cmd binary sits outside
# that walk unless something registers it. Every cmd binary is required to use
# planar_binary() (cmake/module.cmake), which performs that registration; this
# fixture proves the walk itself reaches a registered add_executable() target,
# so a cmd executable is checked at all.
#
# Deliberately plain add_executable() + manual registration here rather than
# calling the real planar_binary(): that function also sets CXX_MODULE_STD ON,
# which needs the full pinned-libc++ toolchain setup. The real helper is
# exercised by ../planar-binary-real/. What is under test here is
# architecture.cmake's walk, not module.cmake's target-property setup.
#
# The induced violation is a layer-3 binary depending on another layer-3
# target (the same-layer prohibition, D18). It is not a layer-3 -> layer-1
# edge: a cmd binary may depend downward on planar_db.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_cmd_binary_db CXX)

add_library(planar_cmd_sibling STATIC db.cpp)       # layer 3 (^cmd_ prefix)
add_executable(planar_cmd_probe main.cpp)           # layer 3 (^cmd_ prefix)

# The induced violation: a cmd binary reaching a sibling cmd target, which
# the same-layer prohibition forbids.
target_link_libraries(planar_cmd_probe PRIVATE planar_cmd_sibling)

# Mirrors planar_binary()'s registration (cmake/module.cmake) — this is the
# property under test: an add_executable() target must be registered here to
# be walked at all, and both ends of the edge must be registered.
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_cmd_sibling planar_cmd_probe)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
