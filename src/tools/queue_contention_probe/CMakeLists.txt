# queue_contention_probe — the host queue's write-contention measurement tool
# (plan 1089, task qp-contention-measure; tech spec 656 § Write contention,
# decision 1004). It drives the real hostqueue enqueue and poll from N worker
# threads plus a planning writer against a COPY of an operator-size planar.db
# and emits one JSON report per run (R1-R5). See main.cpp's header for the
# copy-safety contract.
#
# Standalone executable, not a planar_module()/planar_binary() target: it is
# tooling (decision 1000), it is never installed, and its measurement runs are
# NOT registered with ctest (they take minutes and need an operator-size
# database). Its refusal and report-shape tests (main.t.cpp) are.
#
# It links planar_* modules downward only (engine_hostqueue, db, process,
# json_text), and SQLite::SQLite3 for the online backup API, which the `db`
# module does not expose.

if(NOT TARGET planar_engine_hostqueue)
  message(FATAL_ERROR "queue_contention_probe needs planar_engine_hostqueue; add src/engine before src/tools")
endif()

add_executable(queue_contention_probe main.cpp)
set_target_properties(queue_contention_probe PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
if(PLANAR_WARNINGS_AS_ERRORS)
  target_compile_options(queue_contention_probe PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
endif()
target_link_libraries(queue_contention_probe PRIVATE
  planar_engine_hostqueue planar_db planar_process planar_json_text SQLite::SQLite3)

# queue_contention_probe_tests — the refusal and report-shape tests
# (main.t.cpp), run against the shipped binary by subprocess. Hand-rolled like
# surface_lint_tests: the probe is a plain add_executable(), so there is no
# planar_module() test lane to reuse. The test binary migrates a scratch
# source with the embedded chain (planar_db) and hashes it (planar_sha256).
if(TARGET Catch2::Catch2WithMain)
  if(PLANAR_CATCH2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${PLANAR_CATCH2_SOURCE_DIR}/extras")
  endif()
  include(Catch OPTIONAL RESULT_VARIABLE _qcp_catch_module_found)

  add_executable(queue_contention_probe_tests main.t.cpp
    "${CMAKE_SOURCE_DIR}/cmake/test_support/arena_sweep_listener.cpp")
  planar_mark_test_binary(queue_contention_probe_tests)
  set_target_properties(queue_contention_probe_tests PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_MODULE_STD ON)
  if(PLANAR_WARNINGS_AS_ERRORS)
    target_compile_options(queue_contention_probe_tests PRIVATE
      $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>)
  endif()
  target_link_libraries(queue_contention_probe_tests PRIVATE
    Catch2::Catch2WithMain planar_db planar_sha256)
  target_compile_definitions(queue_contention_probe_tests PRIVATE
    PLANAR_QCP_BIN="$<TARGET_FILE:queue_contention_probe>")
  add_dependencies(queue_contention_probe_tests queue_contention_probe)

  if(COMMAND catch_discover_tests)
    catch_discover_tests(queue_contention_probe_tests
      TEST_PREFIX "planar.queue_contention_probe."
      PROPERTIES LABELS "queue_contention_probe")
  else()
    message(WARNING
      "queue_contention_probe: catch_discover_tests unavailable — "
      "queue_contention_probe_tests is built but its TEST_CASEs are not "
      "registered individually with ctest.")
  endif()
endif()
