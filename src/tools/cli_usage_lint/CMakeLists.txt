# cli_usage_lint — C++ port of zig/tools/cli_usage_lint.zig (plan 996, task
# 6402). Validates authored CLI invocations against the live `<bin> schema`
# catalog. See main.cpp's header for the full contract.
#
# Standalone executable, not a planar_module() target — it shells the
# binaries it lints rather than linking their command trees, so the only
# first-party dependency is planar_json_dom (to parse `<bin> schema`
# output).

add_executable(cli_usage_lint main.cpp)
set_target_properties(cli_usage_lint PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON)
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(cli_usage_lint PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(cli_usage_lint PRIVATE planar_json_dom)

# cli_usage_lint_tests (plan 1080, task hq-cli-usage-lint-tests) — black-box
# ctest coverage of the shipped tool: it is run over synthetic repository
# roots against a fake `planar-agent` whose `schema` output is a small fixed
# catalog, the same subprocess shape surface_lint_tests uses. Hand-rolled
# rather than planar_module()'s test lane because cli_usage_lint is a plain
# add_executable() (see src/tools/surface_lint/CMakeLists.txt).
if(TARGET Catch2::Catch2WithMain)
  if(PLANAR_CATCH2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${PLANAR_CATCH2_SOURCE_DIR}/extras")
  endif()
  include(Catch OPTIONAL RESULT_VARIABLE _cli_usage_lint_catch_module_found)

  add_executable(cli_usage_lint_tests main.t.cpp
    "${CMAKE_SOURCE_DIR}/cmake/test_support/arena_sweep_listener.cpp")
  planar_mark_test_binary(cli_usage_lint_tests)
  set_target_properties(cli_usage_lint_tests PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON)
  if(PLANAR_WARNINGS_AS_ERRORS)
    target_compile_options(cli_usage_lint_tests PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
  endif()
  target_link_libraries(cli_usage_lint_tests PRIVATE Catch2::Catch2WithMain)
  target_compile_definitions(cli_usage_lint_tests PRIVATE
    PLANAR_CLI_USAGE_LINT_BIN="$<TARGET_FILE:cli_usage_lint>")
  add_dependencies(cli_usage_lint_tests cli_usage_lint)

  if(COMMAND catch_discover_tests)
    catch_discover_tests(cli_usage_lint_tests
      TEST_PREFIX "planar.cli_usage_lint."
      PROPERTIES LABELS "cli_usage_lint")
  else()
    message(WARNING
      "cli_usage_lint: catch_discover_tests unavailable — cli_usage_lint_tests "
      "is built but its TEST_CASEs are not registered individually with "
      "ctest.")
  endif()
endif()
