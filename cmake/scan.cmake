# Module-dependency scanning is for targets that use modules.
#
# CMAKE_CXX_SCAN_FOR_MODULES is ON for the whole tree, so every C++ source in
# every vendored dependency (abseil, protobuf, gRPC, ...) gets a scan step and
# a dyndep edge. None of those targets declares a module.
#
# planar_disable_third_party_scan() turns scanning off for every target whose
# sources live under a dependency cache and which has no module file set. Call
# it last, after every target exists.

function(_planar_collect_targets dir out)
  get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_sub IN LISTS _subdirs)
    _planar_collect_targets("${_sub}" _nested)
    list(APPEND _targets ${_nested})
  endforeach()
  set(${out} "${_targets}" PARENT_SCOPE)
endfunction()

function(planar_disable_third_party_scan)
  _planar_collect_targets("${CMAKE_SOURCE_DIR}" _all)
  set(_count 0)
  foreach(_target IN LISTS _all)
    get_target_property(_type "${_target}" TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY" OR _type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(_source_dir "${_target}" SOURCE_DIR)
    string(FIND "${_source_dir}" "/vendor/" _in_vendor)
    if(_in_vendor EQUAL -1)
      continue()
    endif()
    get_target_property(_module_sets "${_target}" CXX_MODULE_SETS)
    if(_module_sets)
      continue()
    endif()
    set_target_properties("${_target}" PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    math(EXPR _count "${_count} + 1")
  endforeach()
  message(STATUS "Module scanning disabled for ${_count} third-party targets")
endfunction()
