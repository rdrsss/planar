/// @file closure.cppm
/// @brief `planar.engine.closure` — umbrella re-export for the engine/closure
/// bucket, matching the pattern `lib/engine/runtime/runtime.cppm` and
/// `lib/engine/runs/runs.cppm` establish. Only the read side (`store`) is
/// ported; see store.cppm's cut list for why the extractor is not.
module;

export module planar.engine.closure;

export import planar.engine.closure.store;
export import planar.engine.closure.compute;
