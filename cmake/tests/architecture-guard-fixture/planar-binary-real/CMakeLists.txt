# Fixture (d): planar_binary() exercised for real. Plan 996 task 6070 (M3
# pre-work). Hazard 1: planar_binary() (cmake/module.cmake:152-182) had zero
# call sites and zero tests — fixture (c) (../cmd-binary-db/) deliberately
# proxies it with a hand-rolled add_executable() plus manual
# PLANAR_MODULE_TARGETS registration (see that fixture's comment for why),
# so the function's own body — the add_executable() call, the
# planar_cmd_<name> naming, the target-property registration, DEPENDS
# resolution — had never actually executed.
#
# This fixture calls the REAL planar_binary() (via include(module), the same
# cmake/module.cmake the src/cmd/ tree uses from M3 onward) to build the
# execute binary under the OLD/literal naming convention
# (`planar_binary(planar_execute ...)`, yielding `planar_cmd_planar_execute`)
# with a real DEPENDS db edge (resolved by planar_binary() itself, not a
# manual target_link_libraries() call), and asserts
# planar_check_architecture() still rejects the no-SQLite-handle violation.
# That proves, through the real function body rather than a proxy:
#   (a) planar_binary() creates a target — add_executable() actually runs
#   (b) the target lands in PLANAR_MODULE_TARGETS — registration actually
#       runs (planar_check_architecture() silently skips an unregistered
#       target, the exact gap fixture (c) proved for a naked
#       add_executable())
#   (c) a violating edge sourced from a planar_binary() DEPENDS is rejected
#
# Deliberately uses an executable -> STATIC library edge (cmd_planar_execute
# -> db), not an executable -> executable edge: CMake's own generate-time
# linker validation refuses to link one executable into another regardless
# of D15, which would make a same-layer cmd<->cmd fixture fail for the
# wrong reason and mask whether cmake/architecture.cmake's own walk is
# doing the rejecting. ../planar-binary-execute-natural-name/ covers the
# natural (unprefixed) call convention and the derived-edge closure
# specifically; this fixture is the "real invocation, old convention"
# control for hazard 1 in isolation.
#
# planar_binary() sets CXX_MODULE_STD ON on every target it creates, so
# (unlike fixture (c)'s deliberately toolchain-free proxy) this fixture
# needs the pinned-libc++ toolchain flags; the top-level CMakeLists.txt
# forwards them from the active configure (itself driven by
# CMakePresets.json's "base" preset — see docs/toolchain-parity.md) via
# NEEDS_TOOLCHAIN, so this file does not hardcode a second copy of them.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_planar_binary_real CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../")
include(module)  # the real planar_module() / planar_binary(), unmodified

add_library(planar_db STATIC db.cpp)  # layer 1
set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS planar_db)

# The induced violation: planar_binary(planar_execute DEPENDS db) — a real
# invocation, old/literal naming convention. planar_binary() itself both
# creates planar_cmd_planar_execute and performs the DEPENDS link to
# planar_db; nothing here is manual.
planar_binary(planar_execute SOURCES main.cpp DEPENDS db)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
