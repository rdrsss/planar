/// @file grouping.cppm
/// @brief `planar.engine.grouping` — umbrella re-export for the
/// engine/grouping bucket, matching the pattern
/// `lib/engine/runtime/runtime.cppm` and `lib/engine/runs/runs.cppm`
/// establish. The optional `mtkahypar` solver arm is not ported; see
/// load.cppm's cut list.
module;

export module planar.engine.grouping;

export import planar.engine.grouping.greedy;
export import planar.engine.grouping.load;
