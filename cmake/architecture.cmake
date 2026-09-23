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
#   layer 2: `engine_*` and the explicit `cmd_internal` command-support
#            target — shared invocation and database injection below binaries
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
# dependency edge: any layer-3 target that actually links (directly or
# transitively — see the "Transitive closure" section below)
# `planar_engine_execute` is treated as an execute carrier, regardless of
# what its own target name is. That makes the exception impossible to
# bypass by choosing a different `planar_binary()` name — the only way to
# silence it is to not link engine_execute at all, which is the correct
# behavior.
#
# Re-evaluation of the literal-name match (plan 996, task 6069, M3
# pre-work): the original literal-name match (`_name STREQUAL
# "cmd_planar_execute"`) is KEPT alongside the edge-derived check, not
# removed. It is still load-bearing for TWO standing fixtures, not just
# one: cmd-binary-db/ (a manually-registered add_executable() proxy that
# links planar_db directly but never links planar_engine_execute at all —
# see that fixture's own comment for why it is deliberately toolchain-free)
# and planar-binary-real/ (the real planar_binary() invoked with
# DEPENDS db only, no engine_execute — that fixture's whole point is
# proving planar_binary() itself works end-to-end under the OLD/literal
# naming convention, independent of the edge-derived closure work). Both
# fixtures were written before this task and neither links engine_execute,
# so dropping the literal would silently turn both into false negatives
# (configure would start succeeding against an intentionally-violating
# fixture) rather than the intended discrimination. Removing the literal
# and restructuring both fixtures to also link engine_execute was
# considered and rejected: it would purely re-test the (already
# well-covered, see planar-binary-execct-natural-name/) edge-derived path a
# third time while destroying the one remaining "literal convention still
# works" proof (see planar-binary-execute-natural-name/, which already
# proves the edge-derived path independent of naming). The literal is
# small, frozen, and commented — the drift risk it carries is lower than
# the coverage loss from deleting it.
#
# Transitive closure (plan 996, task 6069, M3 pre-work — closes a gap
# task 6070 documented but deliberately left open): the execute-carrier
# check and the no-SQLite-handle exception it gates now both consult the
# FULL transitive closure of a target's planar_* dependency graph, not
# just its direct LINK_LIBRARIES/INTERFACE_LINK_LIBRARIES entries. Task
# 6070 derived the carrier flag from a real dependency edge, but only
# scanned DIRECT links: a cmd binary reaching planar_engine_execute only
# through an intermediate module was not classified as a carrier at all,
# and — independently — a carrier's OWN reach into planar_db was only
# checked against its direct links, so a carrier that reaches planar_db
# only through an intermediate, otherwise-legal module (e.g. an engine
# bucket that legitimately depends on db for its own reasons) was not
# caught either. See
# cmake/tests/architecture-guard-fixture/execute-transitive-db/ for the
# standing proof: a cmd binary directly links engine_execute (carrier,
# already caught pre-closure) AND separately links a legal, db-using
# intermediate engine module — the db reachability is only visible through
# the closure, not any single direct edge.
#
# The generic strictly-downward layer check does NOT need a separate
# transitive variant: every registered target is walked as the SOURCE of
# its own edges (the outer `foreach(_tgt IN LISTS _planar_targets)` below),
# so every edge in the graph is validated individually as its own target's
# direct dependency. A chain of downward-or-layer-1-allowed edges is, by
# construction, already fully downward end-to-end — there is no way for a
# multi-hop chain to reach a higher layer than a single hop could not
# already have flagged on its own. Transitivity only matters for the
# execute-carrier property specifically, because that is a semantic
# exception layered on TOP of (not derived from) the layer numbers, so
# "carrier-ness" does not automatically propagate the way "downward" does.
#
# INTERFACE edges (plan 996, task 6069, M3 pre-work — M2 reviewer probe):
# the walk used to read only the `LINK_LIBRARIES` target property, which
# only reflects PUBLIC/PRIVATE link entries actually used to build the
# target itself. A `target_link_libraries(planar_core INTERFACE
# planar_engine_widget)` edge — propagated to consumers but never used to
# build `planar_core` — does not appear in `LINK_LIBRARIES` at all, so it
# configured cleanly even though it is exactly the same upward edge the
# `upward/` fixture already proves is forbidden under PUBLIC. The walk now
# also reads `INTERFACE_LINK_LIBRARIES` and merges both properties before
# deriving the dependency edge set. See
# cmake/tests/architecture-guard-fixture/interface-edge/ for the standing
# proof.
#
# Generator expressions (plan 996, task 6069, M3 pre-work): a link entry
# wrapped in a `$<...>` generator expression is not evaluated until the
# generate step (after this walk already ran and, in the violating case,
# already returned/passed), so a genex-hidden dependency edge would
# silently bypass D15 entirely — this walk cannot evaluate what CMake
# itself defers to generate time, and pretending otherwise (e.g. trying to
# regex out the "true" target name from common genex shapes) would be
# fragile and give a false sense of coverage for the genex forms it didn't
# anticipate. Rather than silently ignore a genex entry (the pre-existing
# behavior — it simply never matched the `^planar_(.+)$` pattern), the walk
# now DETECTS a link entry containing `$<` and refuses configure with a
# diagnostic naming the offending target and entry, before it would even
# get a chance to be misclassified. This is a deliberate
# over-approximation (SOME generator expressions might be provably
# layer-safe, e.g. a config-gated choice between two targets in the same
# layer) traded for correctness: a false-positive refusal that names the
# fix ("split the target" / "use a plain name") is recoverable in seconds;
# a false-negative silent pass is exactly the blind spot this task exists
# to close. See cmake/tests/architecture-guard-fixture/genex-edge/ for the
# standing proof.
#
# Dropping the `planar_` co-match (plan 996, task 6074): task 6069 could
# only afford to refuse a genex that ALSO contained the literal `planar_`,
# because an unscoped match broke the real configure — CMake's own
# imported-target plumbing lands `$<LINK_ONLY:SQLite::SQLite3>` in
# `planar_db`'s `INTERFACE_LINK_LIBRARIES`, and refusing every `$<` made
# this file unable to configure Planar at all. Task 6078 then added the
# unconditional `$<LINK_ONLY:...>` unwrap below (for a different reason:
# the textbook `target_link_libraries(planar_X PRIVATE planar_Y)` spelling
# was tripping the refusal), and that unwrap incidentally removes exactly
# the entries that forced the narrowing. MEASURED, not assumed: with the
# unwrap in place, the fully configured Planar graph contains ZERO
# remaining `$<` entries across every registered planar_* target's merged
# LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES. So the co-match now costs
# nothing to remove and closes a real hole — a genex that computes a
# planar_* target name WITHOUT that literal substring (the smallest form
# is `$<TARGET_PROPERTY:some_non_planar_holder,DEP>`, which resolves at
# generate time to whatever that property holds) used to pass the walk
# entirely. See cmake/tests/architecture-guard-fixture/genex-no-literal/
# for the standing proof; that fixture configures CLEANLY against the
# pre-task-6074 check and FATALs against this one, which is what makes it
# a discriminating fixture rather than a restatement of genex-edge/.
#
# Re-entrancy (plan 996, task 6074): CMake has no local mutable
# containers, so the cycle DFS and the reachability BFS below keep their
# traversal state in GLOBAL properties. `planar_check_architecture()` is
# called exactly once per configure today, but nothing enforced that, and
# a SECOND call inherited the first call's `black` colouring for every
# node it had already finished — pass 2 skips a node whose state is
# already set, so a cycle introduced between two calls among
# already-visited nodes was silently not detected. The function now clears
# every piece of its own traversal state at entry (see the "Pass 0" block)
# rather than asserting single-invocation, so a second call is simply
# correct. See cmake/tests/architecture-guard-fixture/reentrant-cycle/ for
# the standing proof: it calls the guard twice, adding the cycle in
# between, and configures CLEANLY without the reset.
#
# Layer-1 cycles (plan 996, task 6069, M3 pre-work): D17 (decision 943)
# permits layer-1-to-layer-1 edges because base libraries legitimately
# build on each other, but that reopens the possibility of a genuine
# dependency CYCLE among layer-1 libraries (A -> B -> A). D17's own body
# used to claim CMake/Ninja's generator refuses to build a real circular
# target_link_libraries graph, so a cycle would fail configure/generate
# anyway — THAT CLAIM IS FALSE for planar_module()'s STATIC libraries (M2
# boundary review, plan 996 task 6066: a reviewer probe configured AND
# generated a mutual planar_a<->planar_b STATIC target_link_libraries
# cycle — exit 0, no diagnostic at either step). CMake permits and
# silently resolves a static cycle by re-listing the involved archives on
# the final link line as many times as needed; it is only an
# INTERFACE/SHARED-library cycle, or one CMake's dependency graph cannot
# topologically order at all, that configure/generate refuses. The walk
# now performs its own cycle detection (a standard white/gray/black DFS)
# over the full configured planar_* target graph before doing anything
# else with it. In practice only a layer-1 subgraph can legally contain a
# cycle — every cross-layer edge must be strictly downward (enforced
# below), and same-layer engine<->engine / cmd<->cmd edges are
# independently forbidden — so a cycle elsewhere in the graph would mean
# some OTHER check in this file has a bug; the detector does not
# special-case layer 1, it simply asks whether the graph is a DAG. See
# cmake/tests/architecture-guard-fixture/layer1-cycle/ for the standing
# proof.

#
# The Centurion boundary (plan 1033 M1, task 6708 — pass 4 below): Centurion
# is added as a subdirectory (cmake/centurion.cmake), so its whole target
# graph exists in this configure. Planar's planar-execute becomes a Centurion
# CLIENT (decision 1007), and the boundary is that it is ONLY a client:
#
#   1. A planar_* target may link exactly one Centurion target,
#      `centurion::client`. Any other `centurion_*` edge — store, runtime,
#      protocol, crypto, anything — is refused by name.
#   2. Only an execute carrier (the same carrier definition the
#      no-SQLite-handle exception uses) may REACH centurion::client. Another
#      binary pulling it in through an intermediate library is refused.
#   3. centurion::client's own FULL link closure — walked across Centurion's
#      targets, not only planar_* ones, with aliases resolved — must contain
#      no Centurion execution component (store, workflow, model, activity,
#      extension, host, runtime, lua, daemon_*), no Lua, no SQLite, and no
#      terminal UI. Centurion asserts much of this on its own side
#      (its cmake/architecture.cmake); Planar re-asserts it here because a
#      Centurion bump that loosened its own rule would otherwise pass
#      silently into planar-execute.
#
# All three are FATAL at configure with a "Centurion boundary" diagnostic.
# Standing proofs: cmake/tests/architecture-guard-fixture/centurion-*.

# @brief Classify a planar_module() name into its architecture layer.
# @param name The module name as passed to planar_module() (e.g. "core",
#        "engine_identity", "cmd_planar").
# @param out_var Variable name (in the caller's scope) to receive the layer.
function(_planar_module_layer name out_var)
  if(name STREQUAL "cmd_internal")
    # Application infrastructure shared by command binaries, below their
    # handler layer and above the reusable db/cli libraries.
    set(${out_var} 2 PARENT_SCOPE)
  elseif(name MATCHES "^cmd_")
    set(${out_var} 3 PARENT_SCOPE)
  elseif(name MATCHES "^engine_")
    set(${out_var} 2 PARENT_SCOPE)
  else()
    set(${out_var} 1 PARENT_SCOPE)
  endif()
endfunction()

# @brief DFS visitor for cycle detection over the GLOBAL PROPERTY adjacency
#        built by planar_check_architecture() (_planar_arch_adj_<name>).
#        White/gray/black coloring via _planar_arch_cycle_state_<name>
#        (unset=white, "gray"=on the current DFS stack, "black"=fully
#        explored). On finding a back-edge to a gray node, records the
#        path-so-far in the GLOBAL PROPERTY _planar_arch_cycle_path and
#        sets _planar_arch_cycle_found — CMake functions only propagate a
#        result one PARENT_SCOPE up, so a flag that must survive an
#        arbitrarily deep recursive unwind has to live in a GLOBAL
#        PROPERTY, not a return value.
# @param name The (unprefixed) module name to visit.
function(_planar_arch_cycle_visit name)
  get_property(_state GLOBAL PROPERTY _planar_arch_cycle_state_${name})
  if(_state STREQUAL "black")
    return()
  endif()
  if(_state STREQUAL "gray")
    get_property(_path GLOBAL PROPERTY _planar_arch_cycle_path)
    list(APPEND _path "${name}")
    set_property(GLOBAL PROPERTY _planar_arch_cycle_path "${_path}")
    set_property(GLOBAL PROPERTY _planar_arch_cycle_found TRUE)
    return()
  endif()

  set_property(GLOBAL PROPERTY _planar_arch_cycle_state_${name} "gray")
  get_property(_path GLOBAL PROPERTY _planar_arch_cycle_path)
  list(APPEND _path "${name}")
  set_property(GLOBAL PROPERTY _planar_arch_cycle_path "${_path}")

  get_property(_deps GLOBAL PROPERTY _planar_arch_adj_${name})
  foreach(_dep IN LISTS _deps)
    get_property(_found GLOBAL PROPERTY _planar_arch_cycle_found)
    if(_found)
      return()
    endif()
    _planar_arch_cycle_visit("${_dep}")
  endforeach()

  get_property(_found GLOBAL PROPERTY _planar_arch_cycle_found)
  if(NOT _found)
    get_property(_path GLOBAL PROPERTY _planar_arch_cycle_path)
    list(REMOVE_AT _path -1)
    set_property(GLOBAL PROPERTY _planar_arch_cycle_path "${_path}")
  endif()
  set_property(GLOBAL PROPERTY _planar_arch_cycle_state_${name} "black")
endfunction()

# @brief Breadth-first reachability test over the same GLOBAL PROPERTY
#        adjacency: does the transitive closure of `start` contain
#        `needle`? Used for the execute-carrier / no-SQLite-handle
#        exception, which is a semantic property layered on top of the
#        layer numbers rather than derived from them (see this file's
#        header comment, "Transitive closure").
# @param start The (unprefixed) module name to start from.
# @param needle The (unprefixed) module name being searched for.
# @param out_var Variable name (in the caller's scope) to receive TRUE/FALSE.
function(_planar_arch_reaches start needle out_var)
  set(_visited "")
  set(_queue "${start}")
  set(_result FALSE)
  while(_queue)
    list(GET _queue 0 _cur)
    list(REMOVE_AT _queue 0)
    if(_cur IN_LIST _visited)
      continue()
    endif()
    list(APPEND _visited "${_cur}")
    get_property(_deps GLOBAL PROPERTY _planar_arch_adj_${_cur})
    foreach(_dep IN LISTS _deps)
      if(_dep STREQUAL needle)
        set(_result TRUE)
      endif()
      if(NOT _dep IN_LIST _visited)
        list(APPEND _queue "${_dep}")
      endif()
    endforeach()
  endwhile()
  set(${out_var} ${_result} PARENT_SCOPE)
endfunction()

# @brief Resolve a raw link entry to the real CMake target it names: strips
#        CMake's own `$<LINK_ONLY:...>` wrapper and follows an ALIAS. Sets
#        out_var to "" when the entry is not a target (a raw flag, a path, a
#        remaining generator expression).
# @param entry The link entry as read from (INTERFACE_)LINK_LIBRARIES.
# @param out_var Variable name (in the caller's scope) to receive the name.
function(_planar_arch_resolve_target entry out_var)
  if(entry MATCHES "^\\$<LINK_ONLY:(.+)>$")
    set(entry "${CMAKE_MATCH_1}")
  endif()
  if(entry MATCHES "\\$<" OR NOT TARGET "${entry}")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  get_target_property(_aliased "${entry}" ALIASED_TARGET)
  if(_aliased)
    set(entry "${_aliased}")
  endif()
  set(${out_var} "${entry}" PARENT_SCOPE)
endfunction()

# @brief Every real target a target links, directly and through its
#        interface, resolved by _planar_arch_resolve_target().
# @param tgt A real (non-alias) target name.
# @param out_var Variable name (in the caller's scope) to receive the list.
function(_planar_arch_target_links tgt out_var)
  set(_resolved "")
  foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_links "${tgt}" ${_prop})
    if(NOT _links)
      continue()
    endif()
    foreach(_entry IN LISTS _links)
      _planar_arch_resolve_target("${_entry}" _real)
      if(_real)
        list(APPEND _resolved "${_real}")
      endif()
    endforeach()
  endforeach()
  if(_resolved)
    list(REMOVE_DUPLICATES _resolved)
  endif()
  set(${out_var} "${_resolved}" PARENT_SCOPE)
endfunction()

# Real target names centurion::client's closure may never contain (header
# comment, "The Centurion boundary", rule 3).
set(_PLANAR_CENTURION_FORBIDDEN_IN_CLIENT
  "^(centurion_(store|workflow|model|activity|extension|host|runtime|runtime_policy|lua|daemon_.*)|lua_static|sqlite3|screen|dom|component|ftxui.*)$")

# @brief Pass 4 of planar_check_architecture(): the Centurion boundary. See
#        the header comment, "The Centurion boundary". A configure with no
#        Centurion targets at all has nothing to check and returns cleanly.
# @param all_names The registered (unprefixed) planar_* module names.
function(_planar_check_centurion_boundary all_names)
  set(_client_linkers "")
  foreach(_name IN LISTS all_names)
    set(_tgt "planar_${_name}")
    set(_entries "")
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_links "${_tgt}" ${_prop})
      if(_links)
        list(APPEND _entries ${_links})
      endif()
    endforeach()
    foreach(_entry IN LISTS _entries)
      _planar_arch_resolve_target("${_entry}" _real)
      if(NOT _real MATCHES "^centurion")
        continue()
      endif()
      if(NOT _real STREQUAL "centurion_client")
        message(FATAL_ERROR
          "Centurion boundary: ${_tgt} links '${_entry}' (Centurion target "
          "${_real}). Planar may link centurion::client and nothing else of "
          "Centurion — planar-execute is a client of the engine, never a "
          "host of it (decision 1007, plan 1033 task 6708).")
      endif()
      list(APPEND _client_linkers "${_name}")
    endforeach()
  endforeach()
  if(_client_linkers)
    list(REMOVE_DUPLICATES _client_linkers)
  endif()

  # Rule 2: only an execute carrier may reach a client linker.
  foreach(_name IN LISTS all_names)
    if(NOT _client_linkers)
      break()
    endif()
    _planar_module_layer("${_name}" _layer)
    if(NOT _layer EQUAL 3)
      continue()
    endif()
    set(_via "")
    foreach(_linker IN LISTS _client_linkers)
      set(_hit FALSE)
      if(_linker STREQUAL _name)
        set(_hit TRUE)
      else()
        _planar_arch_reaches("${_name}" "${_linker}" _hit)
      endif()
      if(_hit)
        set(_via "${_linker}")
        break()
      endif()
    endforeach()
    if(NOT _via)
      continue()
    endif()
    if(_name STREQUAL "cmd_planar_execute")
      set(_carrier TRUE)
    else()
      _planar_arch_reaches("${_name}" "engine_execute" _carrier)
    endif()
    if(NOT _carrier)
      message(FATAL_ERROR
        "Centurion boundary: planar_${_name} reaches centurion::client "
        "(through planar_${_via}). Only planar-execute may be a Centurion "
        "client; every other binary reaches workflow state through "
        "planar-execute or the Planar database, never the engine directly "
        "(plan 1033 task 6708).")
    endif()
  endforeach()

  # Rule 3: centurion::client's own closure, across every project's targets.
  # Checked whenever the target EXISTS, not only once a planar_* target links
  # it, so the pinned Centurion release is graded at the configure that adds
  # it rather than at the later one that first links it.
  if(NOT TARGET centurion_client)
    return()
  endif()
  set(_visited "")
  set(_queue "centurion_client")
  while(_queue)
    list(GET _queue 0 _cur)
    list(REMOVE_AT _queue 0)
    if(_cur IN_LIST _visited)
      continue()
    endif()
    list(APPEND _visited "${_cur}")
    if(_cur MATCHES "${_PLANAR_CENTURION_FORBIDDEN_IN_CLIENT}")
      # Rebuild the chain back to the client for the diagnostic.
      set(_chain "${_cur}")
      set(_step "${_cur}")
      while(DEFINED _parent_${_step})
        set(_step "${_parent_${_step}}")
        list(PREPEND _chain "${_step}")
      endwhile()
      string(REPLACE ";" " -> " _chain_str "${_chain}")
      message(FATAL_ERROR
        "Centurion boundary: centurion::client's link closure reaches "
        "${_cur} (${_chain_str}). planar-execute links the client, so this "
        "would put a Centurion execution component, Lua, SQLite or a "
        "terminal UI inside a Planar binary (plan 1033 task 6708). Pin a "
        "Centurion release whose client target stays client-only.")
    endif()
    _planar_arch_target_links("${_cur}" _next)
    foreach(_n IN LISTS _next)
      if(NOT _n IN_LIST _visited AND NOT DEFINED _parent_${_n})
        set(_parent_${_n} "${_cur}")
        list(APPEND _queue "${_n}")
      endif()
    endforeach()
  endwhile()
endfunction()

# @brief Validate the configured target graph against D15 and fail configure,
#        naming the offending edge, on the first violation found. A build
#        failure, not a review comment.
function(planar_check_architecture)
  get_property(_planar_targets GLOBAL PROPERTY PLANAR_MODULE_TARGETS)

  # Pass 0: clear this function's own GLOBAL traversal state, so a SECOND
  # invocation in the same configure is correct rather than silently
  # weaker (see the header comment, "Re-entrancy"). The adjacency is
  # rebuilt from scratch in pass 1 and would be overwritten anyway; the
  # load-bearing one is `_planar_arch_cycle_state_*`, which pass 2 reads
  # as "already visited, skip" and which would otherwise carry the
  # previous call's `black` colouring into this one.
  foreach(_tgt IN LISTS _planar_targets)
    string(REGEX REPLACE "^planar_" "" _name "${_tgt}")
    set_property(GLOBAL PROPERTY _planar_arch_cycle_state_${_name} "")
    set_property(GLOBAL PROPERTY _planar_arch_adj_${_name} "")
  endforeach()
  set_property(GLOBAL PROPERTY _planar_arch_cycle_path "")
  set_property(GLOBAL PROPERTY _planar_arch_cycle_found FALSE)

  # Pass 1: build the merged (LINK_LIBRARIES + INTERFACE_LINK_LIBRARIES)
  # planar_* adjacency for every registered target, refusing configure
  # outright on any generator expression found along the way (see header
  # comment, "Generator expressions"). This has to run to completion for
  # every target BEFORE any cycle detection or closure query below, since
  # both need the full graph, not just the one target currently being
  # walked.
  set(_all_names "")
  foreach(_tgt IN LISTS _planar_targets)
    if(NOT TARGET ${_tgt})
      continue()
    endif()
    string(REGEX REPLACE "^planar_" "" _name "${_tgt}")
    list(APPEND _all_names "${_name}")

    set(_merged_links "")
    get_target_property(_direct_links ${_tgt} LINK_LIBRARIES)
    if(_direct_links)
      list(APPEND _merged_links ${_direct_links})
    endif()
    get_target_property(_iface_links ${_tgt} INTERFACE_LINK_LIBRARIES)
    if(_iface_links)
      list(APPEND _merged_links ${_iface_links})
    endif()

    set(_dep_names "")
    foreach(_entry IN LISTS _merged_links)
      # Strip an unconditional $<LINK_ONLY:...> wrapper BEFORE applying the
      # genex refusal below (plan 996 task 6078, M3 review remediation F2).
      # For a STATIC library, CMake itself records every PRIVATE link entry
      # in INTERFACE_LINK_LIBRARIES wrapped in $<LINK_ONLY:...> — that is
      # CMake's own generated plumbing (so it always names its target
      # literally; there is nothing to "evaluate" here, unlike a genex
      # authored by a caller), not evasive or ambiguous code. Without this
      # unwrap, the ordinary, textbook spelling
      # target_link_libraries(planar_X PRIVATE planar_Y) tripped the genex
      # refusal below and told the author to "replace it with a plain
      # target name" when they already had — see
      # cmake/tests/architecture-guard-fixture/private-link-legal/ for the
      # standing proof this is now accepted.
      if(_entry MATCHES "^\\$<LINK_ONLY:(.+)>$")
        set(_entry "${CMAKE_MATCH_1}")
      endif()
      # EVERY remaining generator expression is refused, not only those
      # containing the literal `planar_` (plan 996, task 6074 — see the
      # header comment, "Generator expressions", for the measurement that
      # made this affordable). The `planar_` co-match this check used to
      # carry was a false negative by construction: a genex that computes
      # a planar_* target name without that substring appearing literally
      # — `$<TARGET_PROPERTY:some_holder,DEP>` is the smallest example —
      # slipped through undetected, and no amount of pattern-matching
      # short of evaluating the expression can tell the two apart.
      if(_entry MATCHES "\\$<")
        message(FATAL_ERROR
          "D15 violation: ${_tgt} links a generator expression "
          "('${_entry}') that cannot be evaluated at configure time — the "
          "architecture walk reads the configured target graph, not "
          "generated build output, so a genex-hidden dependency edge would "
          "silently bypass D15. Replace it with a plain target name "
          "(splitting the target if the conditional differs per "
          "configuration) so the edge is visible to this walk. If a "
          "VENDORED package put this entry here, the fix is to stop "
          "linking that package directly into a planar_* target, or to "
          "extend the unconditional $<LINK_ONLY:...> unwrap above if the "
          "new form is, like that one, CMake's own generated plumbing "
          "naming its target literally.")
      endif()
      if(_entry MATCHES "^planar_(.+)$")
        set(_dep_name "${CMAKE_MATCH_1}")
        if(NOT _dep_name STREQUAL _name)
          list(APPEND _dep_names "${_dep_name}")
        endif()
      endif()
    endforeach()
    if(_dep_names)
      list(REMOVE_DUPLICATES _dep_names)
    endif()
    set_property(GLOBAL PROPERTY _planar_arch_adj_${_name} "${_dep_names}")
  endforeach()
  if(_all_names)
    list(REMOVE_DUPLICATES _all_names)
  endif()

  # Pass 1b: reject any planar_-prefixed dependency name that IS a real
  # CMake TARGET but was never registered into PLANAR_MODULE_TARGETS (plan
  # 996 task 6078, M3 review remediation F3). module.cmake's "every
  # planar_* target MUST use planar_module()/planar_binary()" rule is
  # enforced only by a comment, not a check — a raw add_library()
  # intermediate that happens to be named planar_* configures cleanly and
  # is invisible to _all_names, which silently voids the transitive
  # closure for anything that depends through it (a real carrier->db edge
  # routed through such an intermediate would never be flagged; "GUARD
  # PASSED" on a genuine violation). This must run after _all_names is
  # fully built (needs the complete registered set), but before any
  # closure query below consumes it. See
  # cmake/tests/architecture-guard-fixture/unregistered-target-caught/ for
  # the standing proof.
  #
  # Plan 996 task 6087 widened this from "is a real TARGET but
  # unregistered" to "is not in _all_names", full stop. The original check
  # tested TARGET-ness first, so a planar_-prefixed dependency that is not
  # a CMake target AT ALL — a typo, or a hand-written `-lplanar_x` — was
  # skipped entirely: CMake does not error on it (it becomes a raw link
  # flag), and pass 3 below classifies it by NAME, where anything not
  # matching `^cmd_`/`^engine_` reads as a layer-1 base library and is a
  # legal downward edge from every layer. So `planar_scop_ref` (one letter
  # short) configured clean, passed the layer walk, and failed at link
  # time. Both halves now FATAL, with distinct diagnostics naming the
  # actual fix. See
  # cmake/tests/architecture-guard-fixture/nontarget-dep-caught/ for the
  # standing proof. The "D15 violation" prefix is kept on both even though
  # this is registration hygiene rather than layering: it keeps the
  # fixture matcher and grep-ability consistent with every other refusal
  # this file emits (M3 review iteration 2 recommendation).
  foreach(_tgt IN LISTS _planar_targets)
    if(NOT TARGET ${_tgt})
      continue()
    endif()
    string(REGEX REPLACE "^planar_" "" _name "${_tgt}")
    get_property(_dep_names GLOBAL PROPERTY _planar_arch_adj_${_name})
    foreach(dep_name IN LISTS _dep_names)
      if(dep_name IN_LIST _all_names)
        continue()
      endif()
      if(TARGET "planar_${dep_name}")
        message(FATAL_ERROR
          "D15 violation: ${_tgt} depends on planar_${dep_name}, which is "
          "a real CMake target but was never registered via "
          "planar_module()/planar_binary() (PLANAR_MODULE_TARGETS is "
          "missing it). An unregistered intermediate voids this walk's "
          "transitive closure for anything that depends on it — route "
          "planar_${dep_name} through planar_module()/planar_binary() so "
          "its own dependency edges are checked too.")
      endif()
      message(FATAL_ERROR
        "D15 violation: ${_tgt} depends on planar_${dep_name}, which is "
        "not a CMake target at all — CMake passes an unresolvable name "
        "straight through to the linker as a raw '-lplanar_${dep_name}' "
        "flag, so a typo (or a hand-written -l for a planar_* library) "
        "configures cleanly and surfaces only at link time with a far "
        "worse diagnostic. It is also invisible to this walk's layer and "
        "closure checks, which classify a dep by NAME: `planar_typo` "
        "reads as a layer-1 base library and is therefore a legal "
        "downward edge from anywhere. Fix the name, or register the "
        "target via planar_module()/planar_binary().")
    endforeach()
  endforeach()

  # Pass 2: cycle detection over the full graph (see header comment,
  # "Layer-1 cycles").
  set_property(GLOBAL PROPERTY _planar_arch_cycle_found FALSE)
  foreach(_n IN LISTS _all_names)
    get_property(_state GLOBAL PROPERTY _planar_arch_cycle_state_${_n})
    if(NOT _state)
      set_property(GLOBAL PROPERTY _planar_arch_cycle_path "")
      _planar_arch_cycle_visit("${_n}")
      get_property(_found GLOBAL PROPERTY _planar_arch_cycle_found)
      if(_found)
        get_property(_path GLOBAL PROPERTY _planar_arch_cycle_path)
        string(REPLACE ";" " -> " _path_str "${_path}")
        message(FATAL_ERROR
          "D15 violation: dependency cycle detected among planar_* "
          "targets: ${_path_str}. D17 (decision 943) permits "
          "layer-1-to-layer-1 edges; CMake's own generator does not refuse "
          "a STATIC-library target_link_libraries cycle (it silently "
          "resolves one by relisting archives on the final link line), so "
          "this walk performs its own cycle detection rather than relying "
          "on CMake to catch it.")
      endif()
    endif()
  endforeach()

  # Pass 3: per-target layer + exception checks, fed by the adjacency built
  # in pass 1 (so INTERFACE edges are covered identically to direct
  # edges), plus the closure-aware execute-carrier / no-SQLite-handle
  # exception (see header comment, "Transitive closure").
  foreach(_tgt IN LISTS _planar_targets)
    if(NOT TARGET ${_tgt})
      continue()
    endif()
    string(REGEX REPLACE "^planar_" "" _name "${_tgt}")
    _planar_module_layer("${_name}" _layer)

    get_property(_dep_names GLOBAL PROPERTY _planar_arch_adj_${_name})

    # See the header comment ("Re-evaluation of the literal-name match")
    # for why this literal check is kept alongside the edge-derived,
    # closure-aware check below rather than replaced by it.
    set(_is_execute_carrier FALSE)
    if(_name STREQUAL "engine_execute" OR _name STREQUAL "cmd_planar_execute")
      set(_is_execute_carrier TRUE)
    endif()
    _planar_arch_reaches("${_name}" "engine_execute" _reaches_execute)
    if(_reaches_execute)
      set(_is_execute_carrier TRUE)
    endif()

    if(_is_execute_carrier)
      _planar_arch_reaches("${_name}" "db" _reaches_db)
      if(_reaches_db)
        message(FATAL_ERROR
          "D15 violation: ${_tgt} depends on planar_db, which is forbidden "
          "for '${_name}' — engine.execute must never hold a SQLite handle "
          "(tech-spec § engine buckets: it reaches Planar state only by "
          "shelling planar/planar-agent).")
      endif()
    endif()

    foreach(dep_name IN LISTS _dep_names)
      _planar_module_layer("${dep_name}" _dep_layer)

      if(_layer EQUAL 1 AND _dep_layer EQUAL 1)
        # Layer-1 base libraries may depend on each other (D17, decision
        # 943). A genuine cycle hiding behind this exception was already
        # rejected in pass 2, above.
        continue()
      endif()

      if(NOT _dep_layer LESS _layer)
        message(FATAL_ERROR
          "D15 violation: ${_tgt} (layer ${_layer}) depends on "
          "planar_${dep_name} (layer ${_dep_layer}), which is not "
          "strictly downward. Allowed layering: cmd(3) -> engine(2) -> "
          "{lib base, e.g. cli/db}(1) -> vendored(0). Fix the dependency "
          "direction or relocate the module.")
      endif()
    endforeach()
  endforeach()

  # Pass 4: the Centurion boundary (see header comment).
  _planar_check_centurion_boundary("${_all_names}")
endfunction()
