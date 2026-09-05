# planar_engine_entitylink — engine/entitylink bucket (plan 996, task
# cpp-entity-links). Layer-2 `engine_*` module (cmake/architecture.cmake):
# may DEPEND on layer-1 `db` only (D15) — same layering `engine_planning`
# and `engine_identity` already use. Deliberately does NOT depend on
# `engine_planning` or `engine_identity` (D18's same-layer prohibition for
# layers 2/3; D19, decision 945, requires shared primitives to go to layer
# 1, never sideways — this module needed nothing from either bucket, so no
# extraction was required this cycle; see entitylink.cppm's file header for
# the one place this was actually considered and rejected).
#
# Behavior-preserving port (D2) of a SUBSET of
# zig/src/engine/entitylink.zig (the generic cross-cutting `entity_links`
# CRUD primitive: add/remove/show/list/parse_ref) plus the
# `task_touch_paths` half of zig/src/engine/planning/task.zig
# (`addTouchPath`/`removeTouchPath`/`touchedPaths`/`touchedRepoIDs` — the
# path-level and repo-level "touches" surface). The Zig original splits
# these two tables across two files (entitylink.zig owns entity_links;
# task.zig owns task_touch_paths) because task.zig already existed when
# the polyrepo-touches feature landed; the C++ port puts them in ONE
# module instead because task_touch_paths is additive schema alongside the
# entity_links `touches` edge (migrations/00019's own header comment: "The
# coarse repo-level signal already exists as an entity_links(...) edge;
# this table is ADDITIVE and coexists with it") and putting both tables'
# CRUD in `engine_planning` would require a forbidden `engine_entitylink`
# ->`engine_planning` OR `engine_planning`->`engine_entitylink` sideways
# edge. `engine_planning`'s existing `task.cpp` `mark_blocked` already
# writes its own `entity_links` `task->task depends-on` edge via a raw,
# undecorated `INSERT` rather than calling into this module, for exactly
# that reason (see task.cppm's file header) — this module does not change
# that; it is a new, independent entry point onto the same table, not a
# refactor of the existing one.
#
# CLOSED at task 6193 (previously the headline cut here):
#   - `policy.audit.record` calls and `trail()`, landed TOGETHER for the
#     reason the earlier cycle gave for deferring both: the write half has
#     no consumer without the read half, so wiring one alone ships a
#     surface nothing can prove. `add` now records `link|entity_link|<id>`
#     and `remove` records `unlink|entity_link|<id>`, both AFTER their own
#     write succeeds and both with `actor`/`scope`/`summary` NULL, which is
#     what the oracle writes. `trail()` reads them back, ordered by id,
#     refusing with `not_found` when the link itself does not exist. The
#     `policy` entry in DEPENDS below is the mechanical tell that this
#     bucket IS now wired. Row shapes and the oracle-capture method are in
#     src/lib/policy/CMakeLists.txt. That leaves `engine_promotion` and
#     `engine_runtime` as the two buckets still carrying an audit gap.
#
# NOT ported (see entitylink.cppm's file header for the full per-cut
# rationale — this is the summary):
#   - The composite `task touches add`/`remove` CLI-handler behavior
#     (repo-slug -> repo-id resolution, the orphan-repo advisory, the
#     savepoint-wrapped dual write) — that is `cmd/`-layer glue
#     (zig/src/cmd/planar/handlers/task/touches/add.zig), out of scope per
#     this task's CONSTRAINTS ("no cmd/ binary"). This module exposes the
#     primitives (`add`, `add_touch_path`, `remove_touch_path`) a future
#     handler would compose.
#   - `listTouching` (tasks scoped to a repo OR touching it) — needs
#     `identity::scope::resolve_slug`, a sideways `engine_identity`
#     dependency this module does not take (see file header above).
#   - `tree`, `search`, `health`, `audit_trail` (the standalone
#     `zig/src/engine/runtime/audit_trail.zig` surface, distinct from the
#     `policy.audit.record` write path cut above) — deferred entirely this
#     cycle in favor of a well-tested entity_links + touches core; see this
#     task's coder report for the prioritisation.
#
# The cross-scope guard is DELIBERATELY NOT wired in here — link verbs are
# UNGUARDED BY DESIGN (docs/concepts.md § cross-scope-guard: "Link verbs
# (`*_link`, `task touches add/remove`, `links add/remove`) are
# deliberately unguarded"). `entity_link_add_args`/`entity_link_list_filter`
# accept a `scope` field purely for forward API compatibility with the Zig
# original's M3 note — passing a non-null `scope` returns
# `entity_link_error::unsupported_scope` rather than resolving it, exactly
# like `zig/src/engine/entitylink.zig`'s `add`/`list`. No call in this
# module ever names `planar.engine.identity.scope`'s guard.

planar_module(engine_entitylink
  INTERFACE entitylink.cppm
  SOURCES entitylink.cpp
  DEPENDS
    db
    json_text
    log
    policy)
