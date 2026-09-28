# planar_engine_external — engine/external bucket (plan 996, tasks 6106, 6041).
# Layer-2 `engine_*` module (cmake/architecture.cmake): DEPENDS on layer-1
# `db`, `adapter` and `json_text` only (D15), and on no other `engine_*`
# (D18's same-layer prohibition).
#
# ## Task 6106 stood this bucket up; task 6041 finished it
#
# 6106 ported only the `external_links` create/show/delete third of
# `link.zig`, because that was all `planar unlink` composed, and recorded
# every other surface as deferred WITH the verb family that needed it. Task
# 6041 (M7, the external plane) is that verb family arriving, so those
# deferrals are now closed:
#
#   - `list` / `linksForEntity` / `pullable` / `pushable` — LANDED.
#   - `updateSyncState` / `updateSyncDirection` and the `sync_events` row
#     the latter writes — LANDED.
#   - `baseline_title` / `baseline_status` — LANDED as `load_baseline` /
#     `store_baseline`. They stay OFF the `ext_link` struct, exactly as the
#     Zig `ExtLink` leaves them off, because no SELECT in link.cpp reads them
#     and the conflict engine wants them separately anyway.
#   - `system.zig` — LANDED in full as `planar.engine.external.system`.
#   - `sync.zig` — LANDED in full as `planar.engine.external.sync`.
#
# STILL DEFERRED, and still recorded against the verb that needs it:
#
#   - `agentingest/` (claude.zig, copilot.zig, dispatch.zig, interface.zig) —
#     the `ext propagate` agent-dispatch surface.
#
# ## Why this bucket does NOT depend on engine_extsync
#
# The sync engine drives an adapter, and the adapters live in
# `engine_extsync` — a LAYER-2 PEER, which D18 forbids reaching. It is not
# reached: `pull_link` / `push_link` / `resolve_conflict` all take
# `planar.adapter`'s `external_adapter&`, a layer-1 interface, so this bucket
# never names Jira or GitHub at all. The Zig original solves the same problem
# with `adapter: anytype`; the C++ equivalent that also erases the type at a
# function boundary is an abstract base class. A useful side effect: every
# test in sync.t.cpp drives a stub adapter with no HTTP anywhere.
#
# ## Why it does NOT depend on engine_planning either
#
# `pull_link` writes `tasks.title` / `plans.status` / etc. directly, through
# a table name chosen by a four-arm switch. `engine_planning` owns the same
# tables and is likewise a layer-2 peer. Going through it is not an option,
# and the Zig original does not either — `localEntityFields` and
# `applyRemoteToLocal` both issue their own SQL. NOTE the consequence
# honestly: a sync-applied status change does NOT run the plan/task status
# TRANSITION MATRIX. That is oracle behavior, not an oversight (verified: a
# captured `sync pull` moved a task straight from `done` to `todo`, a
# transition `task update --status` refuses).
#
# The cross-scope guard is NOT wired in, and that is the documented rule
# rather than an omission: `external_links` carries no scope column at all,
# and link verbs are UNGUARDED BY DESIGN (docs/concepts.md §
# cross-scope-guard).
#
# No `policy.audit` rows are written, and that is CORRECT rather than a gap
# (task 6100). The layer-1 `planar.policy` module now exists and this bucket
# was audited against it: the Zig originals behind the `external_links` and
# `external_systems`/sync surfaces contain zero `policy.audit.record` call
# sites. Verified with `grep -rln policy.audit.record zig/src/engine`, whose
# whole output is five buckets: planning, identity, entitylink, promotion,
# runtime.
#
# ## STDOUT TERMINATOR CONTRACT
#
# Not applicable — this bucket renders no operator output. It DOES build one
# JSON document, `sync_events.context_json`, and that one is a stored COLUMN
# VALUE with no terminator: byte-compared against a captured oracle blob in
# sync.t.cpp, key order and all.

planar_module(engine_external
  INTERFACE
    link.cppm
    system.cppm
    sync.cppm
    parent_issue.cppm
    external.cppm
  SOURCES
    link.cpp
    system.cpp
    sync.cpp
    parent_issue.cpp
  DEPENDS db adapter json_text json_dom log sha256)

target_link_libraries(planar_engine_external PRIVATE glaze::glaze)

if(TARGET planar_engine_external_tests)
  target_link_libraries(planar_engine_external_tests PRIVATE glaze::glaze)
endif()
