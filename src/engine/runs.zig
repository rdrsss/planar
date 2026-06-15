//! engine/runs — domain barrel for the measurement-rig substrate.
//!
//! The run-record subsystem lives entirely in the engine, reached by a
//! CLI verb group, never in any execution-mechanism layer (see
//! docs/research/run-record-schema.md §1). It is orthogonal to how a run
//! is driven: the identical recording surface measures a Lua-driven run, a
//! bare instrumented loop, and a host-native driver.
//!
//!   lifecycle — start / event / touch / finish / show over runs,
//!               run_events, run_touches (migration 00025).
//!   harvest   — git-diff → run_touches kind='actual' ground-truth
//!               capture at fan-in.

pub const lifecycle = @import("runs/runs.zig");
pub const harvest = @import("runs/harvest.zig");

test {
    _ = lifecycle;
    _ = harvest;
}
