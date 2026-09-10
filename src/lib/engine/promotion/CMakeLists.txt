# planar_engine_promotion — engine/promotion bucket (plan 996, task 6094).
#
# Layer-2 `engine_*` module (cmake/architecture.cmake): may DEPEND on
# layer-1 modules only, never sideways onto another `engine_*` bucket
# (D15/D18's same-layer prohibition for layers 2 and 3).
#
# Behavior-preserving port (D2) of zig/src/engine/promotion.zig plus the
# OUTPUT half of zig/src/cmd/planar/handlers/{promote,demote}.zig. The Zig
# engine module reaches sideways twice:
#
#   1. `identity.scope.resolveSlug` -- an `engine_identity` peer, which
#      would FATAL here under D15/D18. D19's remedy DOES apply: the shared
#      primitive already lives at layer 1 as `planar.scope_ref` (task 6089),
#      so this module depends downward on `scope_ref` exactly the way
#      `engine_identity` and `engine_planning` already do. No new
#      extraction was needed and none was invented.
#   2. `policy.audit.record` -- WIRED as of task 6184, preserving the
#      asymmetry deliberately: zig/src/engine/promotion.zig `catch {}`s the
#      write at both call sites, so a failed audit row does NOT roll back
#      an already-committed scope change. This bucket's two sites are the
#      tree's ONLY BEST-EFFORT audit call sites -- every other wired site
#      (engine_planning, engine_identity, engine_entitylink) uses the
#      `try`-propagate shape where a failed audit write fails the whole
#      verb. promotion.t.cpp proves the asymmetry with a deliberately
#      broken `audit_log` (dropped table) and asserts `promote`/`demote`
#      still succeed. The DEPENDS line below now names `policy`.
#
# The `kind:id` positional parsing the two handlers do (`entitylink.parseRef`)
# is NOT duplicated here: `engine_entitylink` already exports `parse_ref`,
# and composing the two is a layer-3 concern (D20/decision 947: "a verb that
# composes several layer-2 peers belongs at layer 3"). This module takes the
# already-parsed `(kind, id)` pair, which is also exactly the shape the Zig
# engine function takes.
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

planar_module(engine_promotion
  INTERFACE
    promotion.cppm
  SOURCES
    promotion.cpp
  DEPENDS
    db
    json_text
    policy
    scope_ref)
