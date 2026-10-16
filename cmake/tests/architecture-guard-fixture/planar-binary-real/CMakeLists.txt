# Fixture (d): planar_binary() exercised for real. Plan 996 task 6070 (M3
# pre-work). planar_binary() (cmake/module.cmake) must be the call every cmd
# binary uses, so its own body — the add_executable() call, the
# planar_cmd_<name> naming, the PLANAR_MODULE_TARGETS registration, and
# DEPENDS resolution — has to execute under test, not only through a
# hand-rolled proxy (see ../cmd-binary-db/).
#
# This fixture calls the REAL planar_binary() and planar_module() (via
# include(module), the same cmake/module.cmake the src/cmd/ tree uses) and
# asserts planar_check_architecture() rejects the induced violation. That
# proves, through the real function bodies:
#   (a) planar_binary() creates a target — add_executable() actually runs
#   (b) the target lands in PLANAR_MODULE_TARGETS — registration actually
#       runs (planar_check_architecture() silently skips an unregistered
#       target)
#   (c) a violating edge sourced from a planar_binary() DEPENDS is rejected
#
# The violation is a layer-3 binary depending on a layer-3 library
# (planar_cmd_sibling, built by planar_module(cmd_sibling)). It is a library,
# not an executable: CMake's own generate-time linker validation refuses to
# link one executable into another regardless of D15, which would mask
# whether cmake/architecture.cmake's own walk is doing the rejecting.
#
# planar_binary() and planar_module() set CXX_MODULE_STD ON on every target
# they create, so this fixture needs the pinned-libc++ toolchain flags; the
# top-level CMakeLists.txt forwards them from the active configure via
# NEEDS_TOOLCHAIN, so this file does not hardcode a second copy of them.
cmake_minimum_required(VERSION 4.3)
project(planar_architecture_guard_fixture_planar_binary_real CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../")
include(module)  # the real planar_module() / planar_binary(), unmodified

planar_module(cmd_sibling SOURCES db.cpp)                    # layer 3, registered by planar_module()

# The induced violation: planar_binary(probe DEPENDS cmd_sibling) — a real
# invocation. planar_binary() itself both creates planar_cmd_probe and
# performs the DEPENDS link to planar_cmd_sibling; nothing here is manual.
planar_binary(probe SOURCES main.cpp DEPENDS cmd_sibling)

include("${CMAKE_CURRENT_LIST_DIR}/../../../architecture.cmake")
planar_check_architecture()  # expected to FATAL_ERROR — that failure is the pass condition
