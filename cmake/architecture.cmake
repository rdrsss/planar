# D15 enforcement: the configured CMake target graph over planar_* targets
# must be strictly downward (tech-spec § Module and test machinery / §
# engine buckets): cmd -> engine.* -> {cli, db} -> vendored.
#
# Adapted from tabula's cmake/architecture.cmake, generalized. Tabula's
# version hardcodes a fixed allow-list per module name because tabula's
# module set is fixed and small. Planar's module set is not fully known at
# this M0 scaffold task (only `core` exists; the tech-spec's file-level tree
# names dozens more that land over the following milestones), so this walk
# classifies a module's layer from its *name*, which already encodes its
# position in the file tree (src/cmd/<bin>/, src/lib/engine/<bucket>/,
# src/lib/<name>/) rather than maintaining a hand-written per-module
# allow-list that would need editing on every future planar_module() call:
#
#   layer 3: `cmd_*`     — src/cmd/<binary>/ handler libraries
#   layer 2: `engine_*`  — src/lib/engine/<bucket>/ (identity, planning, ...)
#   layer 1: everything else — src/lib/<name>/ base libraries (db, cli,
#            core, ...) that engine.* and cmd/* depend on
#   layer 0: vendored/third-party targets (CPM packages) — not planar_*
#            targets, out of scope for this walk
#
# A target may depend only on planar_* targets in a STRICTLY lower layer.
#
# Exception (tech-spec "boundary invariants" § engine buckets): engine.execute
# (and its planar-execute command binary) must NEVER depend on planar_db,
# even though db sits in a strictly-lower layer — planar-execute is
# deliberately never handed a SQLite handle; it reaches Planar state only by
# shelling `planar`/`planar-agent`. This turns an accidental `import
# planar.db;` inside engine.execute into a configure-time failure rather
# than a silent capability leak.
#
# Call `planar_check_architecture()` once, after every module's
# CMakeLists.txt (and therefore every `planar_module()` call) has run — the
# top-level CMakeLists.txt does this last, deliberately.

# @brief Classify a planar_module() name into its architecture layer.
# @param name The module name as passed to planar_module() (e.g. "core",
#        "engine_identity", "cmd_planar").
# @param out_var Variable name (in the caller's scope) to receive the layer.
function(_planar_module_layer name out_var)
  if(name MATCHES "^cmd_")
    set(${out_var} 3 PARENT_SCOPE)
  elseif(name MATCHES "^engine_")
    set(${out_var} 2 PARENT_SCOPE)
  else()
    set(${out_var} 1 PARENT_SCOPE)
  endif()
endfunction()

# @brief Validate the configured target graph against D15 and fail configure,
#        naming the offending edge, on the first violation found. A build
#        failure, not a review comment.
function(planar_check_architecture)
  get_property(_planar_targets GLOBAL PROPERTY PLANAR_MODULE_TARGETS)
  foreach(_tgt IN LISTS _planar_targets)
    if(NOT TARGET ${_tgt})
      continue()
    endif()
    string(REGEX REPLACE "^planar_" "" _name "${_tgt}")
    _planar_module_layer("${_name}" _layer)

    get_target_property(_links ${_tgt} LINK_LIBRARIES)
    if(NOT _links)
      continue()
    endif()

    foreach(dep IN LISTS _links)
      if(NOT dep MATCHES "^planar_(.+)$")
        continue()
      endif()
      set(_dep_name "${CMAKE_MATCH_1}")
      if(_dep_name STREQUAL _name)
        continue()
      endif()
      _planar_module_layer("${_dep_name}" _dep_layer)

      if((_name STREQUAL "engine_execute" OR _name STREQUAL "cmd_planar_execute")
          AND _dep_name STREQUAL "db")
        message(FATAL_ERROR
          "D15 violation: ${_tgt} depends on planar_db, which is forbidden "
          "for '${_name}' — engine.execute must never hold a SQLite handle "
          "(tech-spec § engine buckets: it reaches Planar state only by "
          "shelling planar/planar-agent).")
      endif()

      if(NOT _dep_layer LESS _layer)
        message(FATAL_ERROR
          "D15 violation: ${_tgt} (layer ${_layer}) depends on "
          "planar_${_dep_name} (layer ${_dep_layer}), which is not "
          "strictly downward. Allowed layering: cmd(3) -> engine(2) -> "
          "{lib base, e.g. cli/db}(1) -> vendored(0). Fix the dependency "
          "direction or relocate the module.")
      endif()
    endforeach()
  endforeach()
endfunction()
