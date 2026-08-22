# Planar — top-level build entry (C++26 rewrite, plan 996).
#
# Pure C++26 modules (tech-spec § Toolchain and conventions): no header tree
# for first-party code, Ninja-only (module dependency scanning requires it),
# `import std;` behind CMake's per-release experimental gate. See
# docs/toolchain-parity.md for the pinned toolchain this file assumes —
# CMakePresets.json is the supported way to configure; a bare
# `cmake -S . -B build` invocation works but skips the pins the presets
# carry.
#
# The Zig implementation lives under zig/ and remains the buildable parity
# reference until M10 cutover (D3). This file does not touch zig/, and
# CMake is invoked directly via presets — it is not yet wired into the
# Makefile (that happens at M10 cutover, not here).

cmake_minimum_required(VERSION 4.3)

# --- In-source-build rejection, before project() ---------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "Planar does not support in-source builds. Configure into build/<preset>, "
    "e.g.:\n  cmake --preset debug\nRemove CMakeCache.txt and CMakeFiles/ "
    "from the source root before retrying.")
endif()

# --- `import std;` experimental gate ----------------------------------------
#
# CMake gates `import std;` support behind a per-release UUID that must be
# set before CXX is enabled by project() below. The UUID is not a secret or
# a stable API — it changes between CMake feature releases as the underlying
# P1689/modules-json format changes, and using the wrong one degrades to a
# CMake authoring warning followed by a hard configure failure ("Experimental
# `import std` support not enabled"), not a silent fallback. Select the gate
# per the installed CMake minor (adapted from centurion's CMakeLists.txt);
# fail loudly rather than guess for an unrecognized minor. The 4.4.x row is
# empirically verified against this machine's CMake 4.4.2 — see
# docs/toolchain-parity.md § "Derived import-std / embed flag set".
if(CMAKE_VERSION VERSION_LESS 4.4)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "451f2fe2-a8a2-47c3-bc32-94786d8fc91b") # CMake 4.3.x
elseif(CMAKE_VERSION VERSION_LESS 4.5)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
      "f35a9ac6-8463-4d38-8eec-5d6008153e7d") # CMake 4.4.x — verified, docs/toolchain-parity.md
else()
  message(FATAL_ERROR
    "Unsupported CMake ${CMAKE_VERSION}: this project's import-std gate "
    "table (CMakeLists.txt) only covers 4.3.x and 4.4.x. Discover the new "
    "minor's gate UUID (grep the cmake binary for CxxImportStd, or read "
    "Source/cmExperimental.cxx from that CMake's own release tag), add a "
    "row here, and record the finding in docs/toolchain-parity.md before "
    "building Planar with it.")
endif()

# ------------------------------------------------------------------------------
# Project
# ------------------------------------------------------------------------------
project(planar
  VERSION 0.1.0
  DESCRIPTION "Local agent-operations infrastructure (C++26 rewrite, plan 996)"
  LANGUAGES C CXX)

if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
  message(FATAL_ERROR
    "Planar requires the Ninja generator for C++26 module dependency "
    "scanning. Configure with -G Ninja or a preset (CMakePresets.json).")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CXX_MODULE_STD is opted in per-target, not here — see cmake/module.cmake's
# planar_module() and docs/toolchain-parity.md ("Per target (not global —
# forcing it onto vendored CPM targets like Glaze or libcurl breaks their
# own configure)").

# Warnings-as-errors is a target-level option applied only to
# planar_module()-created targets (cmake/module.cmake), never as a global
# compiler flag: CMAKE_CXX_MODULE_STD compiles libc++'s std.cppm as an
# internal CMake target we do not control, and it can trigger CMake's own
# std-module plumbing warnings — a global -Werror would make every
# configure fail on those, not on Planar's own code.
option(PLANAR_WARNINGS_AS_ERRORS "Build planar_* targets with -Werror" OFF)

enable_testing()

# D15 guard standing tests (plan 996 task 6050, M2 pre-work;
# cmake/architecture.cmake, scripts/test-architecture-guard.cmake,
# cmake/tests/architecture-guard-fixture/): each fixture configures a
# small, self-contained CMake project exercising one rule the D15 walk
# enforces and asserts the expected outcome, every ctest run, so the guard
# cannot silently weaken (a FATAL_ERROR downgraded to WARNING, or the walk
# itself deleted) without a standing test noticing. Registered before
# add_subdirectory(src) so these run alongside the rest of the default
# ctest lane regardless of what src/ configures.
function(_planar_add_architecture_guard_test case_name expect)
  cmake_parse_arguments(ARG "NEEDS_TOOLCHAIN" "" "MESSAGES" ${ARGN})
  set(_args
    "-DCASE=${case_name}"
    "-DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/architecture-guard-fixture/${case_name}"
    "-DCXX=${CMAKE_CXX_COMPILER}"
    "-DWORK=${CMAKE_BINARY_DIR}/architecture-guard-${case_name}"
    "-DEXPECT=${expect}")
  # planar_binary()/planar_module() set CXX_MODULE_STD ON on every target
  # they create, which needs the pinned-libc++ toolchain wiring (see
  # docs/toolchain-parity.md). Forward THIS configure's own already-resolved
  # values (from CMakePresets.json's "base" preset) rather than duplicating
  # the hardcoded paths in a second place.
  if(ARG_NEEDS_TOOLCHAIN)
    list(APPEND _args
      "-DTOOLCHAIN_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
      "-DTOOLCHAIN_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DTOOLCHAIN_STDLIB_MODULES_JSON=${CMAKE_CXX_STDLIB_MODULES_JSON}"
      "-DTOOLCHAIN_IMPORT_STD_UUID=${CMAKE_EXPERIMENTAL_CXX_IMPORT_STD}")
  endif()
  set(_n 1)
  foreach(_msg IN LISTS ARG_MESSAGES)
    list(APPEND _args "-DEXPECT_MESSAGE_${_n}=${_msg}")
    math(EXPR _n "${_n} + 1")
  endforeach()
  add_test(NAME "architecture_guard.${case_name}"
    COMMAND "${CMAKE_COMMAND}" ${_args}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-architecture-guard.cmake")
  set_tests_properties("architecture_guard.${case_name}" PROPERTIES LABELS "architecture")
endfunction()

_planar_add_architecture_guard_test(upward fail
  MESSAGES "D15 violation" "planar_core.*depends on.*planar_engine_widget")
_planar_add_architecture_guard_test(engine-execute-db fail
  MESSAGES "D15 violation" "planar_engine_execute depends on planar_db")
_planar_add_architecture_guard_test(cmd-binary-db fail
  MESSAGES "D15 violation" "planar_cmd_planar_execute depends on planar_db")
_planar_add_architecture_guard_test(layer1-same-layer succeed)
# Hazard 1 (plan 996 task 6070, M3 pre-work): planar_binary() exercised
# through its real function body, not the manual-registration proxy the
# fixtures above use.
_planar_add_architecture_guard_test(planar-binary-real fail NEEDS_TOOLCHAIN
  MESSAGES "D15 violation" "planar_cmd_planar_execute depends on planar_db")
# Hazard 2 (plan 996 task 6070, M3 pre-work): the no-SQLite-handle exception
# must still fire when planar_binary() is called with the natural,
# unprefixed name ("execute", not "planar_execute").
_planar_add_architecture_guard_test(planar-binary-execute-natural-name fail NEEDS_TOOLCHAIN
  MESSAGES "D15 violation" "planar_cmd_execute depends on planar_db")
# Blind spot 1 (plan 996 task 6069, M3 pre-work): INTERFACE edges are
# invisible to a LINK_LIBRARIES-only walk. M2 reviewer probe.
_planar_add_architecture_guard_test(interface-edge fail
  MESSAGES "D15 violation" "planar_core.*depends on.*planar_engine_widget")
# Blind spot 2 (plan 996 task 6069, M3 pre-work): a generator-expression
# link entry is unevaluated at configure time and must be refused, not
# silently ignored.
_planar_add_architecture_guard_test(genex-edge fail
  MESSAGES "D15 violation" "generator expression")
# Blind spot 3 (plan 996 task 6069, M3 pre-work): D17 (decision 943)
# permits layer-1-to-layer-1 edges, which reopens a genuine dependency
# cycle that neither the layer walk nor CMake's own generator catches.
_planar_add_architecture_guard_test(layer1-cycle fail
  MESSAGES "D15 violation" "dependency cycle detected")
# Blind spot 4 (plan 996 task 6069, M3 pre-work — inherited scope gap from
# task 6070): the execute-carrier / no-SQLite-handle exception must
# consult the full transitive closure, not just direct links.
_planar_add_architecture_guard_test(execute-transitive-db fail
  MESSAGES "D15 violation" "planar_cmd_execute depends on planar_db")
# F2 (plan 996 task 6078, M3 review remediation): a legal PRIVATE downward
# edge on a STATIC library — CMake records this as $<LINK_ONLY:...> in
# INTERFACE_LINK_LIBRARIES — must configure cleanly, not trip the genex
# refusal.
_planar_add_architecture_guard_test(private-link-legal succeed)
# F3 (plan 996 task 6078, M3 review remediation): an unregistered
# planar_-prefixed intermediate target must not silently void the
# transitive closure for a real carrier->db violation reachable through it.
_planar_add_architecture_guard_test(unregistered-target-caught fail
  MESSAGES "D15 violation" "planar_cmd_execute depends on planar_engine_middleware")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(dependencies)   # CPM + vendored packages (cmake/dependencies.cmake)
include(module)         # planar_module() (cmake/module.cmake)

add_subdirectory(src)

# D15 enforcement over the configured target graph (cmake/architecture.cmake)
# — run last, after every module target exists.
include(architecture)
planar_check_architecture()

# --- clangd symlink ----------------------------------------------------------
# compile_commands.json always lands in the build tree; symlink it to the
# source root for zero-config clangd. The symlink itself is git-ignored and
# always points at whichever build directory was configured most recently.
if(NOT WIN32)
  file(CREATE_LINK
    "${CMAKE_BINARY_DIR}/compile_commands.json"
    "${CMAKE_SOURCE_DIR}/compile_commands.json"
    COPY_ON_ERROR SYMBOLIC)
endif()
