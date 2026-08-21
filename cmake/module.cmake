# planar_module() — one Planar module target plus its Catch2 test binary.
#
# Adapted from tabula's cmake/module.cmake (tech-spec § Module and test
# machinery, D14). Each module gets exactly two targets: the module library
# itself (`planar_<name>`, C++26 module interface + implementation units)
# and its own test binary (`planar_<name>_tests`, built only from that
# module's `*.t.cpp`, linking only the module and its declared DEPENDS).
# Test sources never compile into the module target — this extends the
# strictly-downward invariant (D15, cmake/architecture.cmake) into the
# tests, and it is why the two targets are created separately rather than
# one target with mixed sources.
#
# Deliberate divergence from tabula (documented decision, plan 996 task
# cpp-scaffold-presets): Catch2 has not been vendored yet (it lands in task
# 6023, cmake/dependencies.cmake). tabula's module.cmake FATAL_ERRORs a
# module with *.t.cpp files but no Catch2 target. Ours instead WARNs and
# skips building that module's test binary for this configure, so the
# scaffold (and any module added before task 6023 lands) still configures
# and builds cleanly — "degrade gracefully" per the task brief. Re-running
# configure once Catch2 is vendored picks the test binary back up.
#
# `include(Catch)` (Catch2's CTest integration) is appended to
# CMAKE_MODULE_PATH the first time this file runs *after* Catch2 has been
# vendored (cmake/dependencies.cmake sets PLANAR_CATCH2_SOURCE_DIR) — each
# module's CMakeLists.txt is its own directory scope (add_subdirectory does
# not share CMAKE_MODULE_PATH sideways between siblings), so this append
# happens fresh, locally, every call.

function(planar_module name)
  set(options EMBED)
  set(one_value_args)
  set(multi_value_args INTERFACE SOURCES DEPENDS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  set(_target "planar_${name}")

  add_library(${_target} STATIC)
  if(ARG_INTERFACE)
    target_sources(${_target}
      PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${ARG_INTERFACE})
  endif()
  if(ARG_SOURCES)
    target_sources(${_target} PRIVATE ${ARG_SOURCES})
  endif()
  set_target_properties(${_target} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    # `import std;` is opted in per-target, never globally (verified,
    # docs/toolchain-parity.md: forcing CXX_MODULE_STD onto vendored CPM
    # targets like Glaze or libcurl breaks their own configure).
    CXX_MODULE_STD ON)

  if(PLANAR_WARNINGS_AS_ERRORS)
    target_compile_options(${_target} PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
  endif()
  if(ARG_EMBED)
    # docs/toolchain-parity.md § "-std=c++2c and the C++26 subset": clang
    # 22.1.8 has not reclassified `#embed` as core C++26 syntax and emits
    # -Wc23-extensions under -std=c++2c; under warnings-as-errors that is a
    # build failure unless suppressed. Scoped to EMBED-declared targets only
    # (D5's migrations_embed/templates_embed generators), never globally.
    target_compile_options(${_target} PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>>:-Wno-c23-extensions>)
  endif()

  set(_depend_targets "")
  foreach(dep IN LISTS ARG_DEPENDS)
    list(APPEND _depend_targets "planar_${dep}")
  endforeach()
  if(_depend_targets)
    target_link_libraries(${_target} PUBLIC ${_depend_targets})
  endif()

  # Tracked so cmake/architecture.cmake's planar_check_architecture() can
  # walk every declared module target without needing a hand-maintained
  # module list of its own.
  set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS "${_target}")

  # --- test binary ------------------------------------------------------
  file(GLOB _test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.t.cpp")
  if(_test_sources)
    if(NOT TARGET Catch2::Catch2WithMain)
      message(WARNING
        "planar_module(${name}): *.t.cpp files exist but "
        "Catch2::Catch2WithMain is not a target yet — Catch2 has not been "
        "vendored (task 6023, cmake/dependencies.cmake). ${_target}_tests "
        "is NOT built this configure; re-run configure once Catch2 lands.")
      return()
    endif()

    if(PLANAR_CATCH2_SOURCE_DIR)
      list(APPEND CMAKE_MODULE_PATH "${PLANAR_CATCH2_SOURCE_DIR}/extras")
    endif()
    include(Catch OPTIONAL RESULT_VARIABLE _catch_module_found)

    set(_test_target "planar_${name}_tests")
    add_executable(${_test_target} ${_test_sources})
    set_target_properties(${_test_target} PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_MODULE_STD ON)
    if(PLANAR_WARNINGS_AS_ERRORS)
      target_compile_options(${_test_target} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
    endif()
    target_link_libraries(${_test_target} PRIVATE
      Catch2::Catch2WithMain
      ${_target}
      ${_depend_targets})

    if(COMMAND catch_discover_tests)
      catch_discover_tests(${_test_target}
        TEST_PREFIX "planar.${name}."
        PROPERTIES LABELS "${name}")
    else()
      message(WARNING
        "planar_module(${name}): catch_discover_tests unavailable — "
        "${_test_target} is built but its TEST_CASEs are not registered "
        "individually with ctest.")
    endif()
  endif()
endfunction()
