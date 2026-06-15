//! engine.closure — derived-closure extractor (M2).
//!
//! Computes the symbol-level closure a task must hold resident, replacing
//! the declared-touch path proxy. M2.2 (this file's `symbols` submodule)
//! covers the first step: from a seed file, resolve its declarations into
//! the *modify-set* (the symbols the file itself defines). Later milestones
//! add the reference-edge walk, token weighting, and the `closures` store.

pub const symbols = @import("closure/symbols.zig");
pub const walk = @import("closure/walk.zig");
pub const weight = @import("closure/weight.zig");
pub const store = @import("closure/store.zig");

test {
    _ = symbols;
    _ = walk;
    _ = weight;
    _ = store;
}
