# surface_lint — C++ port of zig/tools/surface_lint.zig (plan 996, task
# 6402). Deterministic semantic validator for authored agent/skill/doc
# Markdown. See main.cpp's header for the full contract.
#
# Standalone executable, not a planar_module() target — no first-party
# module dependency at all; it does its own JSON emission with a local
# escaper (see main.cpp's header for why it does not reuse planar_json_text).

add_executable(surface_lint main.cpp)
set_target_properties(surface_lint PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(surface_lint PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()

# surface_lint_tests (plan 996, task 6415) — standing black-box ctest
# coverage, run against the shipped binary via subprocess (main.t.cpp's
# `capture()` helper), mirroring schema.t.cpp's `[lint-parity]` shape: a
# fixture tree with KNOWN, provably-discriminating violations per check
# class under fixtures/, asserted against the exact diagnostics. See
# main.t.cpp's header for the full rationale (the zig oracle differential
# this expires with, and why missing-binary FAILs rather than SKIPs).
#
# Hand-rolled rather than planar_module()'s test lane: surface_lint is a
# plain add_executable() above, not a planar_* module target, so there is
# no module library for planar_module() to build a test binary against —
# this block reproduces just the pieces that function needs (Catch2
# degrade-gracefully guard, the shared arena-sweep listener,
# catch_discover_tests) directly against the standalone executable.
if(TARGET Catch2::Catch2WithMain)
  if(PLANAR_CATCH2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${PLANAR_CATCH2_SOURCE_DIR}/extras")
  endif()
  include(Catch OPTIONAL RESULT_VARIABLE _surface_lint_catch_module_found)

  add_executable(surface_lint_tests main.t.cpp
    "${CMAKE_SOURCE_DIR}/cmake/test_support/arena_sweep_listener.cpp")
  planar_mark_test_binary(surface_lint_tests)
  set_target_properties(surface_lint_tests PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON)
  if(PLANAR_WARNINGS_AS_ERRORS)
    target_compile_options(surface_lint_tests PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
  endif()
  target_link_libraries(surface_lint_tests PRIVATE Catch2::Catch2WithMain)
  target_compile_definitions(surface_lint_tests PRIVATE
    PLANAR_SURFACE_LINT_BIN="$<TARGET_FILE:surface_lint>"
    PLANAR_SURFACE_LINT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    PLANAR_REPO_ROOT="${CMAKE_SOURCE_DIR}")
  # UNCONDITIONAL, not `if(TARGET surface_lint)` guarded: surface_lint is
  # created unconditionally a few lines above in this same file, so its
  # absence here would mean this file itself is broken, not a legitimate
  # bootstrap state — matching top-level CMakeLists.txt's identical stance
  # on the cli_usage_lint <-> planar_cliapp_tests edge.
  add_dependencies(surface_lint_tests surface_lint)

  if(COMMAND catch_discover_tests)
    catch_discover_tests(surface_lint_tests
      TEST_PREFIX "planar.surface_lint."
      PROPERTIES LABELS "surface_lint")
  else()
    message(WARNING
      "surface_lint: catch_discover_tests unavailable — surface_lint_tests "
      "is built but its TEST_CASEs are not registered individually with "
      "ctest.")
  endif()
else()
  message(WARNING
    "surface_lint: Catch2::Catch2WithMain is not a target yet — Catch2 has "
    "not been vendored (task 6023, cmake/dependencies.cmake). "
    "surface_lint_tests is NOT built this configure; re-run configure once "
    "Catch2 lands.")
endif()
