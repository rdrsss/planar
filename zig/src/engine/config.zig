//! engine/config — configuration plane for Planar.
//!
//! Provides:
//!   - parse:     minimal TOML parser for defaults.toml + user config files
//!   - effective: defaults + file + env → resolved Config + EffectiveMap
//!
//! D-engine-pattern: pure functions, no DB, no goroutines, no interfaces.

pub const parse = @import("config/parse.zig");
pub const effective = @import("config/effective.zig");

// Re-export the most commonly used types at the barrel level for convenience.
pub const Config = effective.Config;
pub const Resolved = effective.Resolved;
pub const Provenance = effective.Provenance;
pub const ValueWithSource = effective.ValueWithSource;
pub const EffectiveMap = effective.EffectiveMap;
pub const sensitiveName = effective.sensitiveName;
pub const sortedKeys = effective.sortedKeys;
pub const resolve = effective.resolve;
pub const vendors = effective.vendors;
pub const tiers = effective.tiers;
pub const work_types = effective.work_types;

/// Embedded raw bytes of the defaults.toml file. Used by `config show --defaults`.
pub const defaults_toml: []const u8 = @embedFile("config/defaults.toml");

// Pull both submodules into the test build so per-file test blocks are found.
test {
    _ = parse;
    _ = effective;
}
