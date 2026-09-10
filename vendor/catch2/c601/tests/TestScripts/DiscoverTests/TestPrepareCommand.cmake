# SPDX-License-Identifier: BSL-1.0

# Unit tests for `prepare_command` helper in `extras/CatchAddTests.cmake`.
#
# Yes, we are at the stage where the script helpers need unit tests.
#
# Run as
#     cmake -DCATCH_ADD_TESTS_SCRIPT=/path/to/extras/CatchAddTests.cmake \
#           -P TestPrepareCommand.cmake


cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED CATCH_ADD_TESTS_SCRIPT)
  message(FATAL_ERROR "Missing argument `CATCH_ADD_TESTS_SCRIPT`")
endif()

if(NOT EXISTS "${CATCH_ADD_TESTS_SCRIPT}")
  message(FATAL_ERROR "Cannot find CatchAddTests.cmake at '${CATCH_ADD_TESTS_SCRIPT}'")
endif()

# Pull in the helper functions. Without `TEST_EXECUTABLE` being defined,
# `catch_discover_tests_impl` is not called.
include("${CATCH_ADD_TESTS_SCRIPT}")

set(_failures 0)

function(expect_equal description actual expected)
  if(actual STREQUAL expected)
    message("  [PASS] ${description}")
  else()
    message("  [FAIL] ${description}")
    message("         expected: [${expected}]")
    message("         actual:   [${actual}]")
    math(EXPR _n "${_failures} + 1")
    set(_failures "${_n}" PARENT_SCOPE)
  endif()
endfunction()

prepare_command(add_test SimpleName /path/to/tests)
expect_equal("Simple arg, no quotes" "${_Command}" "add_test( SimpleName /path/to/tests)\n")

prepare_command(add_test "Name with spaces")
expect_equal("Spaces in arg, needs quotes" "${_Command}" "add_test( [==[Name with spaces]==])\n")

prepare_command(set_tests_properties Foo PROPERTIES LABELS "tagA\;tagB\;tagC")
expect_equal("semicolons in argument are kept and quoted"
  "${_Command}" "set_tests_properties( Foo PROPERTIES LABELS [==[tagA\;tagB\;tagC]==])\n")

set(_Command "PRE-EXISTING")
prepare_command(set_tests_properties Foo PROPERTIES BAR baz)
expect_equal("_Command var does no accumulate commands"
  "${_Command}" "set_tests_properties( Foo PROPERTIES BAR baz)\n")



if(_failures GREATER 0)
  message(FATAL_ERROR "${_failures} prepare_command test(s) failed")
else()
  message(STATUS "All prepare_command tests passed")
endif()
