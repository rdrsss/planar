//! engine/workbench — Workbench engine barrel.
//!
//! Exports the three workbench subsystem modules:
//!   parse   — front-matter parser (FrontMatter struct + parse())
//!   render  — entity → Markdown rendering (render())
//!   feature — feature-directory layout helpers (featureDir, storedPath, …)
//!
//! Design: these modules are pure data-transformation functions with no
//! database dependency (feature.zig has no DB calls in Cycle A). They are
//! placed under the engine barrel so Cycle B handlers can @import("engine")
//! and reach workbench.parse / workbench.render / workbench.feature without
//! additional import wiring.

pub const parse = @import("workbench/parse.zig");
pub const render = @import("workbench/render.zig");
pub const feature = @import("workbench/feature.zig");
pub const manifest = @import("workbench/manifest.zig");
pub const sync = @import("workbench/sync.zig");

test {
    _ = parse;
    _ = render;
    _ = feature;
    _ = manifest;
    _ = sync;
}
