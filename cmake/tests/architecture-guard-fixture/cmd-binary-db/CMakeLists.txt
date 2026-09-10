# Fixture (c): add_executable() coverage. Plan 996 task 6050 (M2
# pre-work), deliverable 2. `planar_check_architecture()` only ever walked
# `PLANAR_MODULE_TARGETS`; before this task's `planar_binary()` helper
# (cmake/module.cmake) existed, a raw add_executable() cmd binary (planar,
# planar-agent, planar-watch, planar-execute) sat outside that property
# entirely, so the tech-spec's claim that architecture.cmake turns an
# accidental `planar-execute` -> `planar_db` link into a configure/link
# failure was true only for the `engine_execute` *module*, never for the
# `cmd_planar_execute` *binary* — nothing registered it.
#
# This fixture proves the fix at the architecture.cmake level: once an
# add_executable() target is registered into PLANAR_MODULE_TARGETS (which
# is exactly what the real planar_binary() helper does — see
# cmake/module.cmake — and is what every future src/cmd/*/CMakeLists.txt
# is required to use instead of a naked add_executable()),
# planar_check_architecture() walks and rejects it identically to an
# add_library() module. Deliberately plain add_executable() + manual
# registration here rather than calling the real planar_binary(): that
# function also sets CXX_MODULE_STD ON (matching planar_module()), which
# needs the full pinned-libc++ toolchain setup CMakePresets.json supplies
# (isystem paths, CMAKE_CXX_STDLIB_MODULES_JSON) — wiring that into this
# standalone, preset-free fixture would make it fail the generate step for
# an unrelated reason under a weakened guard, defeating the discrimination
# this file exists to prove. What's under test here is
# architecture.cmake's walk, not module.cmake's target-property setup.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_cmd_binary_db CXX)

add_library(planar_db STATIC db.cpp)             # layer 1
add_executable(planar_cmd_planar_execute main.cpp)  # layer 3 (^cmd_ prefix)

# The induced violation: the planar-execute binary reaching for planar_db —
# forbidden by the named no-SQLite-handle exception, the same rule fixture
# (b) proves for the engine_execute module.
target_link_libraries(planar_cmd_planar_execute PRIVATE planar_db)

# Mirrors planar_binary()'s registration (cmake/module.cmake) — this is the
# fix under test: an add_executable() target must be registered here to be
# walked at all.
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS
  planar_db planar_cmd_planar_execute)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
