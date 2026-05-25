//! engine/ingestor — Spec-to-task-graph ingestion.
//!
//! Reads the tech-spec / roadmap / test-spec markdown artifacts on disk,
//! decomposes them into the task graph, and applies the diff against the
//! database. Mirrors Go's `internal/ingestor/` package.
//!
//! Module layout:
//!   parse     — markdown → typed structs (no DB).
//!   diff      — typed structs + DB read to compute proposed Diff.
//!   coverage  — gate semantics for `--strict` (uncovered slug tracking).
//!   apply     — write Diff to DB, flip anchor draft→active, auto-draft scenarios.
//!   render    — text + JSON rendering of a Diff for preview output.
//!   scenarios — `draftScenario` auto-draft helper (called by apply for
//!               non-trivial tasks; the `isNonTrivial` heuristic itself
//!               lives in diff.zig, mirroring Go's diff.go layout).

pub const parse = @import("ingestor/parse.zig");
pub const diff = @import("ingestor/diff.zig");
pub const coverage = @import("ingestor/coverage.zig");
pub const apply = @import("ingestor/apply.zig");
pub const render = @import("ingestor/render.zig");
pub const scenarios = @import("ingestor/scenarios.zig");

test {
    _ = parse;
    _ = diff;
    _ = coverage;
    _ = apply;
    _ = render;
    _ = scenarios;
}
