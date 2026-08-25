# planar_engine_closure — engine/closure bucket (plan 996, task 6095).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on layer-1
# modules only, never sideways onto another `engine_*` bucket (D15/D18's
# same-layer prohibition for layers 2 and 3). This module needs only `db`, so
# no D19 extraction was required and none was invented.
#
# Behavior-preserving port (D2) of the READ path of
# zig/src/engine/closure/store.zig plus the rendering half of
# zig/src/cmd/planar/handlers/closure/show.zig -- ONE of the two `closure`
# schema leaves (`closure show`).
#
# NOT ported, named explicitly rather than silently dropped:
#
#   - `closure compute`, the other leaf, and the extractor behind it
#     (zig/src/engine/closure/{symbols,walk,weight}.zig, 1637 lines). Three
#     independent blockers, any one disqualifying:
#       1. It parses the repo's `.zig` corpus through TREE-SITTER
#          (`@import("treesitter")`), which is not vendored in this tree. The
#          symbol names and token weights it emits ARE the grammar traversal's
#          output; a hand-rolled substitute would produce different values and
#          break parity silently.
#       2. It recursively walks the project root's filesystem collecting that
#          corpus. No such seam exists here.
#       3. Its HANDLER additionally composes `engine.planning.task` (the task
#          lookup) and `engine.identity.scope` (the `--scope` write guard).
#          Both are layer-2 peers, so there is no legal home for the
#          composition until a `cmd_*` layer exists (decision 947) -- the same
#          wall cycle 4 hit with `unlink` (task 6098) and correctly filed
#          rather than forced. The scope guard is observable: `closure compute
#          1 --scope global` exits 5 with a scope-mismatch message.
#     Its full oracle behavior is captured in store.t.cpp's header as a
#     handoff record, so the next attempt starts from evidence.
#   - ~~`policy.audit.record` calls.~~ NOT AN OMISSION, and never was
#     (task 6100). The layer-1 `planar.policy` module now exists, and this
#     bucket was audited against it: the Zig originals behind `closure show` (a read-only leaf) contain
#     ZERO `policy.audit.record` call sites, so there is nothing here to
#     wire. The paragraph this replaces claimed a gap it inherited by
#     copy-paste from a bucket that has one. Verified with
#     `grep -rln policy.audit.record zig/src/engine`, whose whole output is
#     five buckets: planning, identity, entitylink, promotion, runtime.
#
# `closure.cppm` is the umbrella re-export, matching
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

planar_module(engine_closure
  INTERFACE
    store.cppm
    closure.cppm
  SOURCES
    store.cpp
  DEPENDS
    db
    json_text)

if(TARGET planar_engine_closure_tests)
  # The test binary opens real on-disk scratch databases through
  # planar.db.migrate, exactly as engine/runs' and engine/runtime's suites do.
  target_link_libraries(planar_engine_closure_tests PRIVATE SQLite::SQLite3)
endif()
