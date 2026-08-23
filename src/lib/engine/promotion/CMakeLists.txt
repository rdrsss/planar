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
#   2. `policy.audit.record` -- best-effort `audit_log` writes after each
#      successful scope UPDATE. There is still no `policy.audit` module in
#      the C++ tree; `engine_planning/CMakeLists.txt` and
#      `engine_identity/association.cppm` already document the same
#      omission, and this module follows that precedent rather than
#      unilaterally standing up a layer-1 audit module inside a port task.
#      Neither `promote` nor `demote` reads or emits audit rows, so no
#      observable CLI contract for the two leaves this module covers is
#      affected. Called out in promotion.cppm's header and in the task's
#      coder report as a residual gap.
#
# The `kind:id` positional parsing the two handlers do (`entitylink.parseRef`)
# is NOT duplicated here: `engine_entitylink` already exports `parse_ref`,
# and composing the two is a layer-3 concern (D20/decision 947: "a verb that
# composes several layer-2 peers belongs at layer 3"). This module takes the
# already-parsed `(kind, id)` pair, which is also exactly the shape the Zig
# engine function takes.

planar_module(engine_promotion
  INTERFACE
    promotion.cppm
  SOURCES
    promotion.cpp
  DEPENDS
    db
    scope_ref)
