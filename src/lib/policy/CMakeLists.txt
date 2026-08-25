# planar.policy — layer-1 policy primitives (plan 1001, task 6100).
#
# Today this is exactly one thing: `policy.audit`, the append-only
# `audit_log` write path. It closes the omission thirteen ported engine
# buckets' CMakeLists had each recorded independently (closure, entitylink,
# external, grouping, local, models, planning, promotion, runs, runtime,
# workbench, workflows, workspace), plus `engine_identity`'s
# association.cppm. The Zig oracle's live database holds tens of thousands
# of these rows and the C++ binaries wrote none — invisible to an exit-code
# or stdout diff, and due to become visible as a fake port bug the moment
# the `audit` verb family lands and reads a table this port had emptied.
#
# LAYER 1 is forced rather than chosen. Every layer-2 `engine_*` bucket
# needs `record`, and cmake/architecture.cmake FATALs at configure time on
# a layer-2 -> layer-2 edge (D15/D18), so the module cannot live inside one
# bucket and be reached from the others. Same D19 shape as `scope_ref` and
# `json_text`: a shared engine primitive moved DOWN a layer so the
# same-layer prohibition stays strict without per-bucket duplication.
#
# DEPENDS db for the reason `scope_ref` does — the module IS a DB write
# primitive, `db` is already layer 1, and every bucket that will call this
# already links `db` directly.
#
# WIRED SO FAR (task 6100): `engine_planning` (annotation, plan, task) and
# `engine_identity` (association). Those are the buckets whose verbs an
# operator can actually reach through `cmd_planar` today, so wiring them
# makes the binary start writing real rows rather than merely making it
# possible to. The remaining buckets each still carry a note in their own
# CMakeLists naming THIS module and saying they are not wired to it yet —
# deliberately per-bucket rather than one uniform paragraph, so the
# omission stays specific and countable instead of becoming boilerplate
# that reads the same everywhere and is skimmed past.
#
# NOT PORTED here: the other two submodules of the Zig `engine/policy.zig`
# umbrella. `status` already exists in this tree as
# `planar.engine.planning.transitions`, and `scope_guard` as
# `engine_identity`'s `check_scope_guard`; re-homing either under this name
# would be a rename with no consumer, not a port.

planar_module(policy
  INTERFACE
    audit.cppm
    policy.cppm
  SOURCES
    audit.cpp
  DEPENDS db)
