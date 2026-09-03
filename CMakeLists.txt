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

# Layer 1 of the worktree gate's two-layer bypass (plan 996 task 6137;
# src/cmd/planar/worktree_gate.cppm), mirroring zig's `-Dtest-binary=true`.
# When OFF — the default, and therefore every production build — the
# `PLANAR_DISABLE_WORKTREE_GATE` environment check is `if constexpr`-dead
# and the gate CANNOT be defeated from the environment. It is ON in the
# `debug` preset only, because that is the build the zig integration
# harness runs against through `make test-parity-cpp`, and the harness
# injects that variable so fixture paths which incidentally live under
# `.worktrees/` do not trip the gate for tests that are not about it.
# Deliberately not a per-target property: it is a posture of the whole
# build, and a binary built without it must be un-bypassable.
option(PLANAR_TEST_BINARY "Compile in the worktree gate's env-var bypass (test/parity builds only)" OFF)
if(PLANAR_TEST_BINARY)
  add_compile_definitions(PLANAR_TEST_BINARY)
endif()

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
# Residual 1 (plan 996 task 6074): the genex refusal used to fire only on
# an entry containing the LITERAL `planar_`, so a generator expression
# that computes a planar_* target name any other way was invisible. This
# fixture configures cleanly against that check and must FATAL against
# the current one.
_planar_add_architecture_guard_test(genex-no-literal fail
  MESSAGES "D15 violation" "generator expression")
# Residual 2 (plan 996 task 6074): the cycle DFS keeps its colouring in
# GLOBAL properties, so a SECOND planar_check_architecture() call in the
# same configure used to walk nothing and detect nothing.
_planar_add_architecture_guard_test(reentrant-cycle fail
  MESSAGES "D15 violation" "dependency cycle detected")
# Residual 3 (plan 996 task 6087): pass 1b tested TARGET-ness first, so a
# planar_-prefixed dep that is not a CMake target at all (a typo, or a
# hand-written -lplanar_x) was skipped entirely and read as a legal
# layer-1 edge.
# The second matcher is a SINGLE TOKEN on purpose: message(FATAL_ERROR)
# re-wraps its text to the terminal width, so a multi-word phrase can be
# split across a newline (this one was, between "not a" and "CMake target
# at all") and stop matching for reasons that have nothing to do with the
# guard. `-lplanar_scop_ref` cannot be broken and appears only in this
# diagnostic.
_planar_add_architecture_guard_test(nontarget-dep-caught fail
  MESSAGES "D15 violation" "-lplanar_scop_ref")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# PLANAR_ORACLE_ROOT — where the Zig parity oracle lives (plan 996, task 6333)
#
# Every parity test resolves its reference binary under `zig/zig-out/bin/`.
# That directory is a BUILD ARTIFACT and is gitignored, so it exists only in
# the main checkout — never in a fresh `git worktree`. Resolving it against
# CMAKE_SOURCE_DIR therefore pointed at a path that does not exist for every
# worktree dispatch, and because the parity cases SKIP rather than fail on an
# absent oracle, the suite reported `100% tests passed` with the entire
# oracle comparison switched off. Sixteen worktree cycles ran that way before
# anyone noticed, because a silent skip is indistinguishable from a pass.
#
# `git rev-parse --git-common-dir` yields the MAIN checkout's `.git` from
# inside any worktree, which is the same trick the Makefile already uses to
# share ZIG_LOCAL_CACHE_DIR. Strip the trailing `/.git` and that is the
# checkout root. Falls back to CMAKE_SOURCE_DIR outside a git tree (a release
# tarball), where the skip is correct because there is no oracle to find.
execute_process(
  COMMAND git rev-parse --path-format=absolute --git-common-dir
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE _planar_git_common_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET)
if(_planar_git_common_dir)
  cmake_path(GET _planar_git_common_dir PARENT_PATH PLANAR_ORACLE_ROOT)
else()
  set(PLANAR_ORACLE_ROOT "${CMAKE_SOURCE_DIR}")
endif()

# Say so out loud. The failure this replaces was silent in both directions:
# the oracle was missing AND nothing said the comparison had stopped running.
if(EXISTS "${PLANAR_ORACLE_ROOT}/zig/zig-out/bin/planar")
  message(STATUS "planar: parity oracle found at ${PLANAR_ORACLE_ROOT}/zig/zig-out/bin")
else()
  message(WARNING
    "planar: NO parity oracle at ${PLANAR_ORACLE_ROOT}/zig/zig-out/bin — "
    "every [parity] case will SKIP and the suite will still report success. "
    "Build it with `make -C zig build` (or `zig build` in zig/), or set "
    "PLANAR_PARITY_STRICT=1 to make the absence fail instead of skip.")
endif()

include(dependencies)   # CPM + vendored packages (cmake/dependencies.cmake)
include(module)         # planar_module() (cmake/module.cmake)

add_subdirectory(src)

# src/tools/ — project tooling (decision 1000). Configured after src/ so
# planar_json_dom (cli_usage_lint's one dependency) already exists.
add_subdirectory(src/tools)

# schema.t.cpp's `[lint-parity]` case (src/lib/cliapp/schema.t.cpp) runs
# `cli_usage_lint` against `planar_cliapp_schema_stub`'s live output. Wired
# here, not in src/lib/cliapp/CMakeLists.txt, because `cli_usage_lint`
# (src/tools/, just configured above) does not exist yet at the point
# add_subdirectory(src) processes cliapp.
if(TARGET planar_cliapp_tests AND TARGET cli_usage_lint)
  target_compile_definitions(planar_cliapp_tests PRIVATE
    PLANAR_CLI_USAGE_LINT_BIN="$<TARGET_FILE:cli_usage_lint>")
  add_dependencies(planar_cliapp_tests cli_usage_lint)
endif()

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
