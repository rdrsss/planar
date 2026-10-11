# compile_fail_driver.cmake -- run by the `compile_fail_check` ctest case
# (src/cmd/CMakeLists.txt).
#
# Inputs:
#   BUILD_DIR  the build directory to drive.
#   POSITIVE   the control target; it must build.
#   NEGATIVES  `target:helper` pairs; each target must FAIL to build and its
#              diagnostic must be a missing-argument
#              error that names `helper`.
#
# Everything the nested build uses (generator, compiler, flags, module
# paths) comes from BUILD_DIR's own configuration; nothing is spelled here.

foreach(_var BUILD_DIR POSITIVE NEGATIVES)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "compile_fail_driver: -D${_var}=... is required")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${POSITIVE}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _log
  ERROR_VARIABLE _log)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR
    "positive control ${POSITIVE} did not build (exit ${_rc}); the negative "
    "probes prove nothing until it does.\n${_log}")
endif()
message(STATUS "positive control ${POSITIVE} builds")

set(_failures "")
foreach(_pair IN LISTS NEGATIVES)
  string(REPLACE ":" ";" _parts "${_pair}")
  list(GET _parts 0 _target)
  list(GET _parts 1 _helper)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${_target}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _log
    ERROR_VARIABLE _log)
  if(_rc EQUAL 0)
    string(APPEND _failures
      "${_target}: a call to ${_helper} without a description COMPILED. "
      "A defaulted description parameter has been added back.\n")
  elseif(NOT _log MATCHES "(no matching function for call to|too few arguments to function call)"
         OR NOT _log MATCHES "'${_helper}'")
    string(APPEND _failures
      "${_target}: failed to build, but not for the expected reason (a "
      "missing argument in a call to '${_helper}').\n${_log}\n")
  else()
    message(STATUS "${_target}: ${_helper} without a description does not compile")
  endif()
endforeach()

if(_failures)
  message(FATAL_ERROR "${_failures}")
endif()
