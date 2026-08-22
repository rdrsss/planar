# Standing negative (and one positive) test for the D15 guard
# (cmake/architecture.cmake). Plan 996 task 6050 (M2 pre-work).
#
# The guard had never been observed to fail — only planar_core and
# planar_db exist (both layer 1), so no real cross-layer edge had ever
# been configured against it. A check verified once by eyeball and never
# exercised again is exactly the "quietly evaporates" failure ci.md warns
# against, aimed at our own tooling: an editor could turn the
# `message(FATAL_ERROR ...)` in cmake/architecture.cmake into a WARNING
# (or delete the check outright) and nothing in the suite would notice.
#
# This script configures one of the fixtures in
# cmake/tests/architecture-guard-fixture/ and asserts the expected
# outcome:
#
#   upward             — layer-1 `core` depends on layer-2 `engine_widget`
#                         (the generic D15 upward-edge rule). MUST fail,
#                         naming the D15 violation and the offending edge.
#   engine-execute-db  — `engine_execute` depends on `db` (the hardcoded
#                         no-SQLite-handle exception, legal by layer number
#                         alone). MUST fail, naming the D15 violation and
#                         the offending edge.
#   cmd-binary-db      — a planar_binary()-registered executable
#                         (`cmd_planar_execute`) depends on `db` (same
#                         exception, proving add_executable() targets are
#                         walked once registered). MUST fail, naming the
#                         D15 violation and the offending edge.
#   layer1-same-layer  — layer-1 `cli` depends on layer-1 `core` (the
#                         layer-1 interdependency decision). MUST SUCCEED —
#                         the positive control proving the decision is
#                         implemented, not just documented.
#
# CMake script rather than shell so the test runs identically on all
# platforms (tabula's parity note: its .sh predecessor was BAD_COMMAND on
# Windows). Invoked as
#   cmake -DCASE=<case> -DFIXTURE=<dir> -DCXX=<compiler> -DWORK=<scratch dir>
#         -DEXPECT=<fail|succeed> [-DEXPECT_MESSAGE_1=<regex>]
#         [-DEXPECT_MESSAGE_2=<regex>] -P <this file>

foreach(_required CASE FIXTURE CXX WORK EXPECT)
  if(NOT DEFINED ${_required})
    message(FATAL_ERROR "usage: cmake -DCASE=<name> -DFIXTURE=<dir> -DCXX=<compiler> -DWORK=<dir> -DEXPECT=<fail|succeed> [-DEXPECT_MESSAGE_1=<regex>] [-DEXPECT_MESSAGE_2=<regex>] -P test-architecture-guard.cmake (missing ${_required})")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")

execute_process(
  COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE}" -B "${WORK}" -G Ninja
          "-DCMAKE_CXX_COMPILER=${CXX}"
  OUTPUT_VARIABLE _out ERROR_VARIABLE _out RESULT_VARIABLE _res)

if(EXPECT STREQUAL "fail")
  if(_res EQUAL 0)
    message(FATAL_ERROR "FAIL [${CASE}]: architecture guard did not reject "
      "the induced violation (configure succeeded)\n${_out}")
  endif()

  if(DEFINED EXPECT_MESSAGE_1 AND NOT _out MATCHES "${EXPECT_MESSAGE_1}")
    message(FATAL_ERROR "FAIL [${CASE}]: configure failed, but not with the "
      "expected D15 message ('${EXPECT_MESSAGE_1}') — the guard may not be "
      "the reason it failed\n${_out}")
  endif()

  if(DEFINED EXPECT_MESSAGE_2 AND NOT _out MATCHES "${EXPECT_MESSAGE_2}")
    message(FATAL_ERROR "FAIL [${CASE}]: configure failed with a D15 "
      "message, but it did not name the induced edge "
      "('${EXPECT_MESSAGE_2}')\n${_out}")
  endif()

  file(REMOVE_RECURSE "${WORK}")
  message(STATUS "OK [${CASE}]: architecture guard rejected the induced "
    "violation and named the offending edge")
elseif(EXPECT STREQUAL "succeed")
  if(NOT _res EQUAL 0)
    message(FATAL_ERROR "FAIL [${CASE}]: architecture guard rejected a "
      "legal layer-1-to-layer-1 edge (configure failed)\n${_out}")
  endif()

  file(REMOVE_RECURSE "${WORK}")
  message(STATUS "OK [${CASE}]: architecture guard accepted the legal "
    "layer-1-to-layer-1 edge")
else()
  message(FATAL_ERROR "usage: -DEXPECT must be 'fail' or 'succeed', got '${EXPECT}'")
endif()
