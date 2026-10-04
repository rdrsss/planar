# planar_engine_grouping — engine/grouping bucket (plan 996, task 6095).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). This module needs only `db`, so
# no D19 extraction was required and none was invented.
#
# Note it deliberately does NOT depend on `engine_closure`, even though it
# reads the same `closures` table: that would be a layer-2-to-layer-2 edge and
# would FATAL at configure time. The two buckets each issue their own SELECT,
# which is what the Zig originals do as well (grouping/load.zig has its own
# query and does not call closure/store.zig's `show`) -- so this is
# behaviour-preserving, not a workaround. The queries genuinely differ:
# grouping filters to the effective roles and folds a symbol seen under both,
# `closure show` returns every row including `transitive`.
#
# Behavior-preserving port (D2) of zig/src/engine/grouping/{load,greedy}.zig
# plus the rendering half of
# zig/src/cmd/planar/handlers/groups/recommend.zig -- the ONE `groups` schema
# leaf (`groups recommend`).
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - The `mtkahypar` SOLVER ARM is NOT on master (decision 1293), but it is
#     here: this branch (`dev/grouping-solvers`) carries it. The arm was
#     first left out for want of a process-spawn seam, later linked in as a
#     vendored library (decision 1006), and is packaged as an
#     `optimal::arm` (`mtkahypar.cppm`) over the `optimal_arm` seam that
#     `load::recommend_with` runs. It is compiled in only with
#     `-DPLANAR_WITH_MTKAHYPAR=ON`; otherwise `--solver mtkahypar` is
#     accepted and degrades to greedy with `optimal_available:false`.
#   - ~~`policy.audit.record` calls.~~ NOT AN OMISSION, and never was
#     (task 6100). The layer-1 `planar.policy` module now exists, and this
#     bucket was audited against it: the Zig originals behind `groups recommend` (a read-only leaf) contain
#     ZERO `policy.audit.record` call sites, so there is nothing here to
#     wire. The paragraph this replaces claimed a gap it inherited by
#     copy-paste from a bucket that has one. Verified with
#     `grep -rln policy.audit.record zig/src/engine`, whose whole output is
#     five buckets: planning, identity, entitylink, promotion, runtime.
#
# `grouping.cppm` is the umbrella re-export, matching
# `src/engine/runtime/runtime.cppm`.
#
# STDOUT TERMINATOR CONTRACT (task 6114, resolving the M4 boundary review's
# F1). A renderer in this tree that returns a COMPLETE stdout payload returns
# the oracle's exact bytes INCLUDING the trailing newline; the layer-3 caller
# writes the string verbatim and appends nothing. A renderer that returns a
# FRAGMENT the caller composes into a larger line -- an error-message body
# that becomes `error: <body>` on stderr -- returns no terminator and its
# @return says so. The alternative shape ("renderer returns the body, caller
# appends the terminator") cannot express two payloads this tree actually has:
# a leaf whose empty output is ZERO BYTES rather than a bare newline
# (`workflow list --json`, `local list --json`), and NDJSON, where the newline
# is per-RECORD rather than per-document. Both are pinned. See
# `json_text/CMakeLists.txt` for the escaping half of the same contract.

planar_module(engine_grouping
  INTERFACE
    greedy.cppm
    load.cppm
    optimal_arm.cppm
    mtkahypar.cppm
    grouping.cppm
  SOURCES
    greedy.cpp
    load.cpp
    mtkahypar.cpp
  DEPENDS
    db
    json_text)

if(TARGET planar_engine_grouping_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runs' and engine/closure's suites do.
  target_link_libraries(planar_engine_grouping_tests PRIVATE SQLite::SQLite3)
endif()

# --- Mt-KaHyPar linkage (plan 1006, task 6460) --------------------------
#
# OFF by default (see the top-level `PLANAR_WITH_MTKAHYPAR` option doc).
# `mtkahypar.cpp` is ALWAYS compiled -- the file itself branches on the
# `PLANAR_HAS_MTKAHYPAR` macro, defined here ONLY when both the option is ON
# and the (EXCLUDE_FROM_ALL) `MtKaHyPar::mtkahypar` target actually exists,
# so a developer who has not populated `vendor/mtkahypar` (or is on a
# platform where the CPM block did not configure) still gets a clean build
# with the graceful stub rather than a configure-time failure.
if(PLANAR_WITH_MTKAHYPAR AND TARGET MtKaHyPar::mtkahypar)
  target_compile_definitions(planar_engine_grouping PRIVATE PLANAR_HAS_MTKAHYPAR)
  target_link_libraries(planar_engine_grouping PRIVATE MtKaHyPar::mtkahypar)
  if(TARGET planar_engine_grouping_tests)
    target_compile_definitions(planar_engine_grouping_tests PRIVATE PLANAR_HAS_MTKAHYPAR)
    target_link_libraries(planar_engine_grouping_tests PRIVATE MtKaHyPar::mtkahypar)
  endif()

  # SHIP THE SOLVER'S SHARED LIBRARY (task 6536).
  #
  # `MtKaHyPar::mtkahypar` is a SHARED library whose install_name is
  # `@rpath/libmtkahypar.dylib`, and `planar_module()` installs executables
  # only. Before this block, `cmake --install` produced a `planar` that could
  # not start: CMake strips the build-tree RPATH on install, so dyld had
  # nowhere left to look and the binary died with
  # `Library not loaded: @rpath/libmtkahypar.dylib` (exit 134) -- measured
  # against a scratch prefix, not inferred.
  #
  # The library goes next to the binaries' own lib dir and each executable
  # gets a relative INSTALL_RPATH (see `planar_module()`), so the install
  # tree is relocatable and does NOT depend on the source checkout surviving
  # at its build-time path.
  #
  # TBB is deliberately NOT vendored here. Mt-KaHyPar's own CMakeLists
  # `find_package(TBB)`s the Homebrew formula and TBB does not support static
  # linking (decision 1006), so it stays an external runtime dependency that
  # `install.sh` preflights as FATAL before reaching the build.
  install(FILES "$<TARGET_FILE:MtKaHyPar::mtkahypar>" DESTINATION lib)
elseif(PLANAR_WITH_MTKAHYPAR)
  message(WARNING
    "PLANAR_WITH_MTKAHYPAR=ON but the MtKaHyPar::mtkahypar target does not "
    "exist (vendor/mtkahypar not populated, or this platform's CPM block "
    "did not configure) -- engine_grouping falls back to the graceful stub "
    "(available() == false) for this configure.")
endif()
