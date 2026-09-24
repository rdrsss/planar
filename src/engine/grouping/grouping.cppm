/// @file grouping.cppm
/// @brief `planar.engine.grouping` — umbrella re-export for the
/// engine/grouping bucket, matching the pattern
/// `src/engine/runtime/runtime.cppm` and `src/engine/runs/runs.cppm`
/// establish. The optional `mtkahypar` solver arm is linked in only when
/// `-DPLANAR_WITH_MTKAHYPAR=ON`; see mtkahypar.cppm and load.cppm's
/// `recommend_with`.
module;

export module planar.engine.grouping;

export import planar.engine.grouping.greedy;
export import planar.engine.grouping.load;
export import planar.engine.grouping.mtkahypar;
