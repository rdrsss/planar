# planar_engine_external — engine/external bucket (plan 996, task 6106).
# Layer-2 `engine_*` module (cmake/architecture.cmake): DEPENDS on layer-1
# `db` only (D15), and on no other `engine_*` (D18's same-layer
# prohibition).
#
# This bucket exists because `planar unlink` needed it. Task 6105 recorded
# `unlink` as one of six leaves "blocked on layer 3"; it was the only one of
# the six whose LAYER-2 dependency was also missing — the C++ tree had no
# `engine_external` of any kind, so `unlink` was blocked twice over and the
# handler could not be written without standing this up first.
#
# Behavior-preserving port (D2) of the create/show/delete third of
# zig/src/engine/external/link.zig (612 lines), scoped to what one leaf
# composes. `create` is included because `show`/`delete` cannot be
# exercised against a real row without it, and a hand-rolled INSERT inside
# a test file would be a second, undocumented statement of the table's
# defaults.
#
# NOT ported this cycle, each deferred WITH the verb that needs it rather
# than speculatively (link.cppm's header carries the same list):
#   - `list` / `linksForEntity` / `pullable` / `pushable` — `links list`,
#     `sync pull`, `sync push`.
#   - `updateSyncState` / `updateSyncDirection` plus the `sync_events`
#     audit row they write — the `sync` verb family.
#   - `baseline_title` / `baseline_status`, columns a later migration added
#     and the Zig `ExtLink` struct does not carry at all — the
#     conflict-detection surface.
#   - The rest of the bucket's Zig siblings entirely: `sync.zig`,
#     `system.zig`, `agentingest/` (the `ext register` / `ext create` /
#     `ext propagate` families and the adapter boundary behind them).
#
# The cross-scope guard is NOT wired in, and that is the documented rule
# rather than an omission: `external_links` carries no scope column at all,
# and link verbs are UNGUARDED BY DESIGN (docs/concepts.md §
# cross-scope-guard). `planar unlink` declares `--scope` and its Zig
# handler discards it with a literal `_ = args.scope;`.
#
# No `policy.audit` rows are written, following the precedent every engine
# bucket in this tree already documents (task 6100 is the open operator
# decision). The `session_entries` audit row `unlink` appends is a
# different table and is written from the HANDLER, by composing
# `engine_runtime`'s session surface — that composition is the layer-3
# work this bucket exists to make possible.

planar_module(engine_external
  INTERFACE
    link.cppm
    external.cppm
  SOURCES
    link.cpp
  DEPENDS db)
