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
# TEST_DEPENDS (task 6079, plan 996 M4 pre-work) names modules the
# `*.t.cpp` test binary needs that the PRODUCTION module target itself does
# not — e.g. a test importing `planar.cli.exit` purely to pin a domain-error
# mapping against the oracle-captured exit code, with no `.cpp` in the
# module naming `planar.cli` at all. Before this parameter existed, the
# only way to get such an edge into the test binary was to add it to
# DEPENDS, which also links it into the production module target and
# creates a real dependency edge `cmake/architecture.cmake`'s D15 walk sees
# and polices — an edge that exists ONLY for a test import is not a
# genuine architectural dependency and should never show up there. Entries
# here are linked into `<name>_tests` alone.
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
  set(multi_value_args INTERFACE SOURCES DEPENDS TEST_DEPENDS)
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

  set(_test_depend_targets "")
  foreach(dep IN LISTS ARG_TEST_DEPENDS)
    list(APPEND _test_depend_targets "planar_${dep}")
  endforeach()

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
      ${_depend_targets}
      ${_test_depend_targets})

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

# planar_binary() — one `src/cmd/<binary>/` executable target.
#
# cmake/architecture.cmake's planar_check_architecture() walks only the
# GLOBAL PLANAR_MODULE_TARGETS property. planar_module() targets append
# themselves to it automatically; a plain add_executable() call (as every
# cmd binary — planar, planar-agent, planar-watch, planar-execute — will
# be) does NOT, so a naked add_executable() in a future src/cmd/*/
# CMakeLists.txt would sit outside D15 enforcement entirely — exactly the
# gap plan 996 task 6050 (M2 pre-work) found: the tech-spec claims
# architecture.cmake turns an accidental planar-execute -> planar_db edge
# into a "configure/link failure", which was only true for the
# `engine_execute` module, not the `cmd_planar_execute` binary target,
# because nothing registered the binary into the walk.
#
# planar_binary(<name> [SOURCES ...] [DEPENDS ...]) creates
# `planar_cmd_<name>` (matching `_planar_module_layer()`'s `^cmd_` layer-3
# classification) and registers it into PLANAR_MODULE_TARGETS exactly like
# planar_module() does, so every cmd binary that uses this helper instead of
# a raw add_executable() is walked by planar_check_architecture() for free.
# src/cmd/*/CMakeLists.txt MUST use this helper, not add_executable()
# directly, once M2/M3 add the first cmd binary — a bare add_executable()
# there silently re-opens the coverage gap this function closes.
#
# Name your execute binary however reads naturally — e.g.
# planar_binary(execute DEPENDS engine_execute ...) yielding
# `planar_cmd_execute` — you do NOT need to spell it
# planar_binary(planar_execute ...) to get the no-SQLite-handle exception in
# architecture.cmake to fire. That exception now derives from whether the
# target actually links `planar_engine_execute`, not from matching this
# function's naming output against a literal string (plan 996, task 6070,
# M3 pre-work — see cmake/architecture.cmake's header comment and
# cmake/tests/architecture-guard-fixture/planar-binary-execute-natural-name/
# for the standing proof). This function itself performs no name
# validation or normalization; the robustness lives in the consuming check,
# not here — do not add a parallel name-based guard in this function, it
# would just be a second literal to keep in sync.
#
# INTERFACE / MAIN / the test lane (plan 996, task 6105, M4b — the first
# real cmd binary). Three additions, all forced by the same fact: a cmd
# binary is ONE layer-3 target, and D18 forbids the obvious alternative.
#
#   - INTERFACE: the handler layer is written in C++26 module interface
#     units like every other first-party file in this tree, so the
#     executable needs a FILE_SET CXX_MODULES exactly as planar_module()
#     sets one up. Without it a cmd binary could only be written as
#     header-free plain .cpp files with no way to declare anything across
#     translation units.
#
#   - MAIN + a test binary: planar_module() builds `<name>_tests` from the
#     module's own *.t.cpp; planar_binary() built NOTHING and SILENTLY
#     IGNORED any *.t.cpp beside it. That is the same class of coverage
#     hole task 6050 found (a target outside the guard walk), one layer up:
#     handler-layer tests would sit in the tree looking like coverage while
#     never being compiled, let alone run. The obvious fix — put the
#     handlers in their own planar_module() and have the binary link it —
#     is a layer-3 -> layer-3 edge, which cmake/architecture.cmake FATALs
#     (D18 keeps the same-layer prohibition for cmd_*, deliberately: "the
#     one-handler-per-binary shape"). So the test binary is built from the
#     SAME sources as the executable, minus the MAIN entry point (Catch2
#     brings its own main), which keeps the binary a single layer-3 target
#     and still gets its handler layer under test. MAIN names the entry
#     point so this function knows which source to drop; it is also
#     compiled into the executable, so callers list it only once.
#
# A cmd binary with *.t.cpp but no MAIN is a configure-time FATAL rather
# than a silent skip: without knowing the entry point this function cannot
# build a Catch2 binary at all, and silently not building one is the exact
# failure mode this lane exists to close.
function(planar_binary name)
  set(options)
  set(one_value_args MAIN)
  set(multi_value_args INTERFACE SOURCES DEPENDS TEST_DEPENDS)
  cmake_parse_arguments(ARG "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

  set(_target "planar_cmd_${name}")

  add_executable(${_target})
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
  if(ARG_MAIN)
    target_sources(${_target} PRIVATE ${ARG_MAIN})
  endif()
  set_target_properties(${_target} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON)

  if(PLANAR_WARNINGS_AS_ERRORS)
    target_compile_options(${_target} PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
  endif()

  set(_depend_targets "")
  foreach(dep IN LISTS ARG_DEPENDS)
    list(APPEND _depend_targets "planar_${dep}")
  endforeach()
  if(_depend_targets)
    target_link_libraries(${_target} PRIVATE ${_depend_targets})
  endif()

  set(_test_depend_targets "")
  foreach(dep IN LISTS ARG_TEST_DEPENDS)
    list(APPEND _test_depend_targets "planar_${dep}")
  endforeach()

  # Same registration planar_module() performs — see this function's
  # header comment for why a plain add_executable() must not skip it.
  set_property(GLOBAL APPEND PROPERTY PLANAR_MODULE_TARGETS "${_target}")

  # --- test binary ------------------------------------------------------
  file(GLOB _test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.t.cpp")
  if(NOT _test_sources)
    return()
  endif()
  if(NOT ARG_MAIN)
    message(FATAL_ERROR
      "planar_binary(${name}): *.t.cpp files exist but no MAIN was given. "
      "The test binary is built from this target's own sources minus the "
      "entry point (Catch2 supplies its own main), so MAIN must name the "
      "entry-point source. Without it these tests would be silently never "
      "compiled.")
  endif()
  if(NOT TARGET Catch2::Catch2WithMain)
    message(WARNING
      "planar_binary(${name}): *.t.cpp files exist but "
      "Catch2::Catch2WithMain is not a target yet. ${_target}_tests is NOT "
      "built this configure.")
    return()
  endif()

  if(PLANAR_CATCH2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${PLANAR_CATCH2_SOURCE_DIR}/extras")
  endif()
  include(Catch OPTIONAL RESULT_VARIABLE _catch_module_found)

  set(_test_target "${_target}_tests")
  add_executable(${_test_target} ${_test_sources})
  if(ARG_INTERFACE)
    target_sources(${_test_target}
      PUBLIC
        FILE_SET CXX_MODULES
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
        FILES ${ARG_INTERFACE})
  endif()
  if(ARG_SOURCES)
    target_sources(${_test_target} PRIVATE ${ARG_SOURCES})
  endif()
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
    ${_depend_targets}
    ${_test_depend_targets})

  # The test binary is NOT registered into PLANAR_MODULE_TARGETS, matching
  # planar_module()'s `<name>_tests`: a test binary is not part of the
  # shipped architecture, and registering it would make the D15 walk police
  # Catch2's own (layer-0, vendored) edges alongside first-party ones.

  if(COMMAND catch_discover_tests)
    catch_discover_tests(${_test_target}
      TEST_PREFIX "planar.cmd_${name}."
      PROPERTIES LABELS "cmd_${name}")
  else()
    message(WARNING
      "planar_binary(${name}): catch_discover_tests unavailable — "
      "${_test_target} is built but its TEST_CASEs are not registered "
      "individually with ctest.")
  endif()
endfunction()
