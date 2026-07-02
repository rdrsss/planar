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
pub const terminal = @import("workbench/terminal.zig");
pub const gc = @import("workbench/gc.zig");

/// resolveRoot is the single-source workbench-root resolver.
/// Resolution order: $PLANAR_WORKBENCH_ROOT env var →
/// config file workbench.root ($PLANAR_CONFIG_PATH or $HOME/.planar/config.toml) →
/// $HOME/.planar/workbench default. A leading `~/` is expanded to $HOME.
/// Returns error.WorkbenchRootUnresolved when no layer produces a path.
pub const resolveRoot = sync.resolveRoot;

test {
    _ = parse;
    _ = render;
    _ = feature;
    _ = manifest;
    _ = sync;
    _ = terminal;
    _ = gc;
}
