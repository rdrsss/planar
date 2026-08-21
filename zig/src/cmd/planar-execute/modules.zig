//! modules.zig — Test aggregator for the spawn-free `planar-execute` modules.
//!
//! Pulls the three salvaged modules into a single test compilation unit so
//! their `test` blocks are exercised by `zig build test`.  This file does NOT
//! define the `planar-execute` binary (that is P0.2c); it is the minimal
//! build-graph anchor that keeps the modules compiled and their unit tests
//! green.
//!
//! Module inventory (P0.2a):
//!   - schema.zig  — CLI schema ingestion (shells `<bin> schema`, pure JSON parse)
//!   - state.zig   — Planner state reads  (shells `planar …`, pure JSON parse)
//!   - brief.zig   — Brief compiler        (pure function, no subprocess/IO)
//!
//! All three are spawn-free in the sense that their unit tests are pure
//! fixture-parse tests (no live binary required).  The public subprocess
//! helpers (`loadSchema`, `planShow`, etc.) use `std.process.run` to call
//! `planar` / `planar-agent` — those are CLI shells, NOT headless LLM spawns,
//! and are acceptable in the deterministic engine core.

const std = @import("std");

// Import the three modules so their test blocks are included in this
// compilation unit.  The `refAllDecls` blocks ensure the compiler type-checks
// every declaration even when no test specifically calls it.
const schema = @import("schema.zig");
const state = @import("state.zig");
const brief = @import("brief.zig");

test {
    // Pull all declarations from each module into the test binary so that:
    //   a) Every `test` block in each module runs.
    //   b) Every exported declaration is type-checked (catches unused imports,
    //      unreachable fields, etc. that a narrower test wouldn't surface).
    std.testing.refAllDecls(schema);
    std.testing.refAllDecls(state);
    std.testing.refAllDecls(brief);
}
