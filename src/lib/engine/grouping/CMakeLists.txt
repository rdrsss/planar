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
#   - The `mtkahypar` SOLVER ARM (zig/src/engine/grouping/mtkahypar.zig, 1776
#     lines). It probes for an external solver binary via `std.process.run`,
#     shells it, and reads its partition output back off disk. No
#     process-spawn seam exists in this tree -- the same call engine/runs made
#     for `bench harvest` and engine/runtime made for `capture commits`. The
#     `--solver` flag, its refusal message, and every reporting field the JSON
#     envelope carries ARE modelled, so what is missing is one arm rather than
#     one leaf. The arm is additionally host-dependent (present on the
#     derivation machine, absent elsewhere, with materially different output
#     each way), which makes it a poor parity subject regardless.
#   - `policy.audit.record` calls. There is still no `policy.audit` module in
#     the C++ tree; `engine_planning/CMakeLists.txt`,
#     `engine_identity/association.cppm`, `engine_promotion`,
#     `engine_runtime`, `engine_runs` and `engine_closure` all document the
#     same omission. `groups recommend` is read-only.
#
# `grouping.cppm` is the umbrella re-export, matching
# `lib/engine/runtime/runtime.cppm`.
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
    grouping.cppm
  SOURCES
    greedy.cpp
    load.cpp
  DEPENDS
    db
    json_text)

if(TARGET planar_engine_grouping_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runs' and engine/closure's suites do.
  target_link_libraries(planar_engine_grouping_tests PRIVATE SQLite::SQLite3)
endif()
