# src/cmd/ — layer-3 command binaries (tech-spec § Target layout; D15's
# `cmd -> engine.* -> {cli, db} -> vendored`).
#
# `planar` (plan 996, task 6105) is the first. `planar-agent` and
# `planar-watch` land beside it. `planar-ext` (task 6418; task 6412,
# decisions 995-1000) is the fourth and owns the external-plane `ext` and
# `sync` families.
#
# The four binaries share invocation values, path resolution and the lazy
# database holder through `cmd_internal`, a command-support target classified
# below the binary layer. Each binary supplies its own database open policy;
# the architecture guard still forbids binary-to-binary `cmd_* -> cmd_*`
# links.
#
# `parity_harness.hpp` is test-only and remains a plain included header.
#
# Every binary here MUST be declared with planar_binary() (cmake/module.cmake),
# never a bare add_executable(): the helper is what registers the target into
# PLANAR_MODULE_TARGETS so cmake/architecture.cmake's D15 walk sees it at all.
# A raw add_executable() sits outside the guard entirely, the gap task 6050
# found. cmake/tests/architecture-guard-fixture/cmd-binary-db/ proves a
# registered executable is walked, and planar-binary-real/ proves
# planar_binary()'s registration runs. unregistered-target-caught/ guards a
# different gap: an unregistered planar_* intermediate target hiding edges.

add_subdirectory(internal)
add_subdirectory(planar)
add_subdirectory(planar-agent)
add_subdirectory(planar-watch)
add_subdirectory(planar-ext)
add_subdirectory(integration_tests)

# --- cross-binary black-box coverage (plan 996, task 6547, decision 1035) ---
#
# `planar/cross_process.t.cpp` ports the part of the Zig integration suite
# that no in-process test can reach: several REAL processes contending on one
# database while a fourth watches, and every built binary's ability to START
# at all (task 6536 — a solver-ON build produced a `planar` that died at
# dyld with exit 134 while all 3,422 in-process tests passed).
#
# It therefore needs the paths of its four SIBLING binaries, which is why the
# block lives here rather than in `planar/CMakeLists.txt`: at that point in
# the configure the sibling targets do not exist yet, so `add_dependencies`
# below would FATAL.
#
# THIS CREATES NO `cmd_* -> cmd_*` LINK EDGE, and that distinction is what
# keeps D18 satisfied. cmake/architecture.cmake's walk reads exactly two
# target properties — LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES. A compile
# definition carrying a `$<TARGET_FILE:...>` path is a string, and
# add_dependencies() is an order-only build edge; neither appears in either
# property. The test target still only ever LINKS its own binary's modules.
if(TARGET planar_cmd_planar_tests)
  target_compile_definitions(planar_cmd_planar_tests PRIVATE
    "PLANAR_AGENT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_agent>\""
    "PLANAR_WATCH_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_watch>\""
    "PLANAR_EXT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_ext>\"")
  add_dependencies(planar_cmd_planar_tests
    planar_cmd_planar_agent
    planar_cmd_planar_watch
    planar_cmd_planar_ext)
endif()

# --- planar-watch's read-verb rendering pin (plan 996, task 6545) ----------
#
# `planar-watch/readpin.t.cpp` replaces the case decision 1034 deleted at the
# M10 cutover. It seeds a fixture with the OPERATOR and AGENT binaries and
# then pins what the six read verbs print over it, so it needs those two
# sibling paths (and `diagnose_checks.t.cpp` drives `planar-ext` against a local fake
# adapter to make and settle a sync conflict, so it needs the EXT binary's too) for the same reason `planar/cross_process.t.cpp` needs its
# four — and it lives here, after every `add_subdirectory` above, because at
# `planar-watch/CMakeLists.txt` time the sibling targets do not exist yet and
# `add_dependencies` would FATAL.
#
# NO `cmd_* -> cmd_*` LINK EDGE IS CREATED. See the block above for why: a
# compile definition holding a `$<TARGET_FILE:...>` string and an order-only
# `add_dependencies()` appear in neither LINK_LIBRARIES nor
# INTERFACE_LINK_LIBRARIES, which are the only two properties
# cmake/architecture.cmake's D18 walk reads.
if(TARGET planar_cmd_planar_watch_tests)
  target_compile_definitions(planar_cmd_planar_watch_tests PRIVATE
    "PLANAR_OPERATOR_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar>\""
    "PLANAR_AGENT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_agent>\""
    "PLANAR_EXT_CPP_BIN=\"$<TARGET_FILE:planar_cmd_planar_ext>\"")
  add_dependencies(planar_cmd_planar_watch_tests
    planar_cmd_planar
    planar_cmd_planar_agent
    planar_cmd_planar_ext)
endif()

# --- compile-fail check for the flag-declaration helpers (plan 1104, task 7200)
#
# Every helper that declares a flag or positional takes a REQUIRED
# description, so a call without one must not compile. A defaulted
# `desc = {}` put back on any of them would compile silently and re-open the
# empty-help hole, and nothing else would notice until the catalog walk
# (`planar/catalog_descriptions.t.cpp`) found a flag that had slipped through.
#
# There is no ctest idiom for "this translation unit must not compile", so
# each probe is an OBJECT library kept out of `all` and `planar_tests`, and
# one ctest case (cmake/tests/compile_fail_driver.cmake) builds them through
# the build's own generator, compiler, flags and module paths:
#
#   - `compile_fail_positive` is the POSITIVE CONTROL, the same calls with a
#     description; it must build.
#   - each `compile_fail_neg_*` is the same call without one; it must fail,
#     and the diagnostic must name the helper, so a probe that fails for an
#     unrelated reason cannot pass for the right one.
#
# The probes compile the three helper interface units themselves (the
# executables' module sources are not linkable libraries) and link nothing,
# so the check costs a handful of translation units, plus building
# `planar_engine_runtime` for the module the shared cli.cppm imports. It is RUN_SERIAL
# because it drives a nested build of this build directory.
# The probe sources live under cmake/tests/ rather than src/: the negative
# probes are deliberately ill-formed, and `make cpp-lint` runs clang-tidy over
# every *.cpp under src/, where they would be reported as compile errors.
set(_planar_compile_fail_dir "${CMAKE_SOURCE_DIR}/cmake/tests/compile_fail")
set(_planar_compile_fail_probes
  positive
  neg_bool_flag
  neg_declare_string
  neg_declare_int
  neg_declare_positional
  neg_declare_positional_optional
  neg_agent_json)
foreach(_probe IN LISTS _planar_compile_fail_probes)
  set(_target "compile_fail_${_probe}")
  add_library(${_target} OBJECT EXCLUDE_FROM_ALL "${_planar_compile_fail_dir}/${_probe}.cpp")
  target_sources(${_target} PRIVATE
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/planar/declare.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/planar-agent/handlers/shared/cli.cppm")
  set_target_properties(${_target} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON)
  # planar_engine_runtime provides `planar.engine.runtime.agentactivity`, which
  # the planar-agent shared cli.cppm imports; linking it gives the probe that
  # module's BMI (an OBJECT library's own link step never runs).
  target_link_libraries(${_target} PRIVATE planar_cliapp planar_engine_runtime CLI11::CLI11_Module)
endforeach()

add_test(NAME compile_fail_check
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DPOSITIVE=compile_fail_positive"
    "-DNEGATIVES=compile_fail_neg_bool_flag:add_bool_flag;compile_fail_neg_declare_string:add_string;compile_fail_neg_declare_int:add_int;compile_fail_neg_declare_positional:add_positional;compile_fail_neg_declare_positional_optional:add_positional_optional;compile_fail_neg_agent_json:add_json"
    -P "${CMAKE_SOURCE_DIR}/cmake/tests/compile_fail_driver.cmake")
set_tests_properties(compile_fail_check PROPERTIES LABELS "compile_fail" RUN_SERIAL TRUE)

# Distribution checks never appear in developer configurations. Use target
# locations so the same check covers the exact products being bundled.
if(PLANAR_PORTABLE)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME portable.binaries
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/portable-check.py"
      --bin-dir "$<TARGET_FILE_DIR:planar_cmd_planar>"
      --platform "${CMAKE_SYSTEM_NAME}"
      --toolchain-prefix "${PLANAR_LLVM_PREFIX}")
  add_test(NAME portable.inspector
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/portable-check.test.py")
  set_tests_properties(portable.binaries portable.inspector PROPERTIES LABELS "portable")
endif()

# --- the order of an install, end to end (plan 1122 M2, rel-install-order) ----
#
# `scripts/install-order-test.sh` runs install.sh --prebuilt under /bin/bash and
# base utilities only, against bundles assembled from the REAL built binaries
# and scratch homes: fresh, behind, ahead and faulty databases, the queue
# warning, kill-and-resume at every swap point, concurrent owners, the updater
# handoff and --cleanup, and uninstall over a pending install.
if(TARGET planar_cmd_planar AND TARGET planar_cmd_planar_agent AND TARGET planar_cmd_planar_watch
   AND TARGET planar_cmd_planar_ext)
  # One ctest entry per group of scenarios (INSTALL_ORDER_GROUP; the groups are listed in
  # the script): the whole file took 757 s on a loaded host and the five-minute ceiling
  # applies to every installer test. Running the script with no group runs all of them.
  foreach(_order_group db swap recover concurrent handoff)
    add_test(NAME install.order_${_order_group}
      COMMAND "${CMAKE_COMMAND}" -E env "INSTALL_ORDER_GROUP=${_order_group}"
        /bin/bash "${PROJECT_SOURCE_DIR}/scripts/install-order-test.sh"
        "$<TARGET_FILE:planar_cmd_planar>" "$<TARGET_FILE:planar_cmd_planar_agent>"
        "$<TARGET_FILE:planar_cmd_planar_watch>"
        "$<TARGET_FILE:planar_cmd_planar_ext>")
    set_tests_properties(install.order_${_order_group} PROPERTIES LABELS "install_order;install_order_${_order_group}" TIMEOUT ${PLANAR_INSTALLER_TEST_TIMEOUT})
  endforeach()
endif()

# --- the installer's staging and vendor-surface fixtures (plan 1122 M5) --------
#
# `scripts/install-stage-test.sh` stages the six vendors' surfaces and, for its
# health scenarios, runs the built `planar` (PLANAR_BIN); without a binary the
# script skips those scenarios and says so, so it is registered only where the
# binary is a target, and always with it. `make test-install-stage` runs the same
# script against build/debug/bin/planar.
if(TARGET planar_cmd_planar)
  # One ctest entry per group of scenarios (INSTALL_STAGE_GROUP; the groups are listed in
  # the script): the whole file took 843 s on a loaded host and the ceiling for an
  # installer test is five minutes.
  foreach(_stage_group staging vendors placement modes uninstall)
    add_test(NAME install.stage_${_stage_group}
      COMMAND "${CMAKE_COMMAND}" -E env "PLANAR_BIN=$<TARGET_FILE:planar_cmd_planar>"
        "INSTALL_STAGE_GROUP=${_stage_group}"
        /bin/bash "${PROJECT_SOURCE_DIR}/scripts/install-stage-test.sh")
    set_tests_properties(install.stage_${_stage_group} PROPERTIES LABELS "install_stage;install_stage_${_stage_group}" TIMEOUT ${PLANAR_INSTALLER_TEST_TIMEOUT})
  endforeach()
endif()
