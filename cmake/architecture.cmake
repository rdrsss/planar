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
#
# Naming-coupling closure (plan 996, task 6070, M3 pre-work): the exception
# below used to match SOLELY by the literal target name `cmd_planar_execute`
# — true only if the execute command binary is declared as
# `planar_binary(planar_execute ...)`. `planar_binary()` (cmake/module.cmake)
# prefixes whatever name it is given with `planar_cmd_`, so the natural,
# unprefixed call `planar_binary(execute ...)` yields `planar_cmd_execute`
# instead, which never matched the literal — the no-SQLite-handle exception
# silently never fired for that (equally real) binary. Nothing enforced or
# even documented that the caller had to spell the name exactly that one
# way. The walk below now ALSO derives the exception from the real
# dependency edge: any layer-3 target that actually links
# `planar_engine_execute` is treated as an execute carrier, regardless of
# what its own target name is. That makes the exception impossible to
# bypass by choosing a different `planar_binary()` name — the only way to
# silence it is to not link engine_execute at all, which is the correct
# behavior. The original literal-name match is kept alongside it (not
# replaced) purely for the pre-existing `cmd-binary-db` fixture, which
# proxies a `cmd_planar_execute` target that never actually links
# `engine_execute`; removing the literal check would stop that fixture's
# induced violation from being caught. See
# cmake/tests/architecture-guard-fixture/planar-binary-execute-natural-name/
# for the standing proof that the natural, unprefixed name is still caught.
# Scope note: the link-based check only looks at DIRECT LINK_LIBRARIES, not
# a transitive closure — a cmd binary that reaches engine_execute only
# through an intermediate layer-2 module is not currently classified as a
# carrier. That is unlikely (engine buckets are flat, per the layer-1
# interdependency decision above, and a cmd binary is expected to link
# engine_execute directly), but it is a real, undetected residual gap, not
# a claim this file makes and then fails to back.
#
# Layer-1 interdependency decision (plan 996, task 6050, M2 pre-work): the
# walk below classifies every non-cmd_/engine_ module into one flat layer
# (1), which by construction made a same-layer edge (e.g. a future `cli ->
# core` or `db -> core` dependency) a D15 violation identical to a genuine
# upward edge. That is wrong for layer 1 specifically — base libraries
# legitimately build on each other (M2 will want exactly this the moment
# lib/cli reaches for a lib/core helper) — so layer-1-to-layer-1 edges are
# now an explicit exception, allowed below. Layers 2 (engine buckets) and 3
# (cmd handlers) keep the same-layer prohibition: the tech-spec's engine
# bucket port order (identity -> planning -> workbench -> runtime ->
# external -> execute) and the one-handler-library-per-binary shape are
# both intentionally flat within their layer, and a same-layer engine<->
# engine or cmd<->cmd edge is exactly the kind of implicit coupling D15 was
# introduced to catch early. The residual risk this exception reopens is a
# genuine dependency *cycle* among layer-1 libraries (A -> B -> A), which
# this walk does not detect by itself. This file used to claim CMake/
# Ninja's own generator refuses to build a real circular
# target_link_libraries graph, so a layer-1 cycle would fail configure/
# generate anyway with CMake's own diagnostic — THAT CLAIM IS FALSE for
# planar_module()'s STATIC libraries (M2 boundary review, plan 996 task
# 6066, D17: a reviewer probe configured AND generated a mutual
# planar_a<->planar_b STATIC target_link_libraries cycle — exit 0, no
# diagnostic at either step). CMake permits and silently resolves a static
# cycle by re-listing the involved archives on the final link line as many
# times as needed; it is only an INTERFACE/SHARED-library cycle, or one
# CMake's dependency graph cannot topologically order at all, that
# configure/generate refuses. A layer-1 same-layer STATIC cycle is
# therefore a REAL gap: neither this D15 walk (which explicitly allows
# layer-1-to-layer-1 edges, per the decision above) nor CMake itself will
# catch it. Closing that gap (e.g. extending this walk with real cycle
# detection over the layer-1 subgraph) is an M3 follow-up, deliberately not
# done here — but the comment must not keep telling a future reader CMake
# already covers it, which would talk them out of adding the detection
# this file still lacks. See
# cmake/tests/architecture-guard-fixture/layer1-same-layer/ for the
# standing proof that a layer-1 same-layer edge is accepted (that fixture
# does not itself construct a cycle; it only proves the single same-layer
# edge case, which is the pre-existing, intentional exception).

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

    # See the comment block above for why this is BOTH a literal-name match
    # (pre-existing, kept for the cmd-binary-db fixture) AND a real-edge
    # derivation (new: closes the naming-coupling trap for any
    # planar_binary() call convention).
    set(_is_execute_carrier FALSE)
    if(_name STREQUAL "engine_execute" OR _name STREQUAL "cmd_planar_execute")
      set(_is_execute_carrier TRUE)
    endif()
    foreach(_check_dep IN LISTS _links)
      if(_check_dep STREQUAL "planar_engine_execute")
        set(_is_execute_carrier TRUE)
      endif()
    endforeach()

    foreach(dep IN LISTS _links)
      if(NOT dep MATCHES "^planar_(.+)$")
        continue()
      endif()
      set(_dep_name "${CMAKE_MATCH_1}")
      if(_dep_name STREQUAL _name)
        continue()
      endif()
      _planar_module_layer("${_dep_name}" _dep_layer)

      if(_layer EQUAL 1 AND _dep_layer EQUAL 1)
        # Layer-1 base libraries may depend on each other (decision above).
        continue()
      endif()

      if(_is_execute_carrier AND _dep_name STREQUAL "db")
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
