/// @file ingest.cppm
/// @brief `planar.engine.ingest` — single import point re-exporting the whole
/// `lib/engine/ingest` surface landed so far (plan 996, task 6035, M4).
/// Matches `lib/engine/planning/planning.cppm`'s umbrella pattern.
///
/// This is the READ side of the spec-ingest pipeline: workbench markdown in,
/// proposed diff plus strict-gate coverage object plus provenance-bearing
/// routing facts out. The apply step, and the import/synthesize flows built on
/// top of it, are NOT ported by this task — see this bucket's CMakeLists.txt
/// for the D15/D18 layering reason, which is architectural rather than a
/// matter of remaining budget.
module;

export module planar.engine.ingest;

export import planar.engine.ingest.parse;
export import planar.engine.ingest.diff;
export import planar.engine.ingest.coverage;
export import planar.engine.ingest.render;
export import planar.engine.ingest.materialize;
