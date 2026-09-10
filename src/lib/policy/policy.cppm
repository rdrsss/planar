/// @file policy.cppm
/// @brief `planar.policy` — single import point re-exporting the layer-1
/// policy surface landed so far (plan 1001, task 6100). Matches
/// `lib/engine/planning/planning.cppm`'s umbrella pattern.
///
/// The Zig original's `engine/policy.zig` aggregates three submodules:
/// `audit`, `scope_guard` and `status`. Only `audit` is ported here.
/// `status` (the per-entity transition matrices) already lives in the C++
/// tree as `planar.engine.planning.transitions`, and `scope_guard` lives
/// in `planar.engine.identity.scope` as `check_scope_guard` — moving
/// either under this umbrella would be a rename with no consumer, not a
/// port. This umbrella re-exports only what exists here.
module;

export module planar.policy;

export import planar.policy.audit;
