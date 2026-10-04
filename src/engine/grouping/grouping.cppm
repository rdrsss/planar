/// @file grouping.cppm
/// @brief `planar.engine.grouping` — umbrella re-export for the
/// engine/grouping bucket, matching the pattern
/// `src/engine/runtime/runtime.cppm` and `src/engine/runs/runs.cppm`
/// establish. Greedy is the only partitioner on master; the optional optimal
/// arm is an abstract seam, see optimal_arm.cppm and load.cppm's `recommend_with`.
/// This branch also carries the Mt-KaHyPar arm (mtkahypar.cppm), linked in only
/// when `-DPLANAR_WITH_MTKAHYPAR=ON`.
module;

export module planar.engine.grouping;

export import planar.engine.grouping.greedy;
export import planar.engine.grouping.load;
export import planar.engine.grouping.mtkahypar;
export import planar.engine.grouping.optimal_arm;
