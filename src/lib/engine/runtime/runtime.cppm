/// @file runtime.cppm
/// @brief `planar.engine.runtime` — umbrella re-export for the
/// engine/runtime bucket, matching the pattern
/// `lib/engine/identity/identity.cppm` and
/// `lib/engine/planning/planning.cppm` already establish.
module;

export module planar.engine.runtime;

export import planar.engine.runtime.session;
export import planar.engine.runtime.snapshot;
export import planar.engine.runtime.capture;
