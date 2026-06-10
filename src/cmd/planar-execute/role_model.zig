//! role_model.zig — Per-role model tier table (plan 492 tasks 3176, 3708, 3709).
//!
//! The `agent(prompt, opts)` host function spawns a worker (`claude --print`
//! or `codex exec`) whose model is selected by the worker's ROLE (`opts.role`),
//! NOT by a per-call override path. This module owns the in-memory role→dispatch
//! types (`Role`, `Vendor`, `Dispatch`, `ModelTable`).
//!
//! ## Source of routing — the shared resolver (plan 540 phase 4)
//!
//! The effective `ModelTable` for a run is built in `main.zig` by shelling
//! `planar models routing --json`, which resolves the operator's
//! `~/.planar/config.toml` ([models] tier maps, [roles] role→tier,
//! [role_vendors] role→vendor, [defaults].vendor) through the shared model
//! resolver. There is no longer a separate `execute-config.toml`; operators
//! override routing in the main Planar config.
//!
//! ## Compiled fallback defaults — sonnet coder, opus reviewer (task 3709)
//!
//! `ModelTable{}` is a last-resort fallback used ONLY when `planar models
//! routing` is unreachable (e.g. `planar` not on PATH). It mirrors the config
//! defaults so the fallback and the resolved table agree:
//!
//! - `coder`       → sonnet (structured authoring against a brief; the opus
//!                   reviewer is the load-bearing quality net behind it).
//! - `reviewer`    → opus (adversarial defect-finding; opus earns its cost).
//! - `test-coder`  → sonnet (mechanical test authoring is sonnet-suitable).
//! - `documenter`  → sonnet (docs polish is sonnet-suitable).
//!
//! Keep these in sync with `src/engine/config/defaults.toml`.
//!
//! ## Visibility (task 3708)
//!
//! `planar-execute run --dry-run` prints the effective role→vendor/model table,
//! so an operator never has to grep source to learn what will spawn.
//!
//! Unknown roles return `RoleError.UnknownRole`. The caller (hostAgent) maps
//! that into a Lua-visible error.

const std = @import("std");

/// All errors this module can surface.
pub const RoleError = error{
    /// The opts.role string did not match any known role in the table.
    UnknownRole,
};

/// The set of roles recognized by the M4 dispatcher. Each maps to a model tier
/// via `modelForRole`.
pub const Role = enum {
    coder,
    reviewer,
    @"test-coder",
    documenter,

    /// Parse a role from the wire string the workflow uses (e.g. "coder",
    /// "reviewer", "test-coder", "documenter"). Returns `RoleError.UnknownRole`
    /// for any unrecognized string.
    pub fn fromString(s: []const u8) RoleError!Role {
        if (std.mem.eql(u8, s, "coder")) return .coder;
        if (std.mem.eql(u8, s, "reviewer")) return .reviewer;
        if (std.mem.eql(u8, s, "test-coder")) return .@"test-coder";
        if (std.mem.eql(u8, s, "documenter")) return .documenter;
        return RoleError.UnknownRole;
    }

    /// Stringify back to the wire form (the inverse of `fromString`).
    pub fn name(self: Role) []const u8 {
        return switch (self) {
            .coder => "coder",
            .reviewer => "reviewer",
            .@"test-coder" => "test-coder",
            .documenter => "documenter",
        };
    }
};

// ---------------------------------------------------------------------------
// Tier constants — pinned model identifiers per the M4 contract.
//
// These are the literal `--model <tier>` argument the worker is spawned with.
// Bumping a tier requires editing this file AND updating the argv assertion
// tests so the contract change is loud rather than silent.
// ---------------------------------------------------------------------------

/// Opus tier — adversarial / high-judgment roles (reviewer).
pub const OPUS_TIER: []const u8 = "claude-opus-4-8";

/// Sonnet tier — authoring + mechanical roles (coder, test-coder, documenter).
pub const SONNET_TIER: []const u8 = "claude-sonnet-4-6";

/// Returns the default `--model` string for `role` (no config overlay). The
/// returned slice points at a comptime string constant and does NOT need
/// freeing. Equivalent to `(ModelTable{}).forRole(role)`.
pub fn modelForRole(role: Role) []const u8 {
    return (ModelTable{}).forRole(role);
}

/// Convenience: look up the default model directly from a role string. Returns
/// `RoleError.UnknownRole` if the role string is unrecognized.
pub fn modelForRoleString(s: []const u8) RoleError![]const u8 {
    const role = try Role.fromString(s);
    return modelForRole(role);
}

// ---------------------------------------------------------------------------
// Vendor + ModelTable — effective role→(vendor, model) mapping for a run
// ---------------------------------------------------------------------------

/// The worker CLI a role dispatches to. `claude` is the historical default
/// (`claude --print …`); `codex` spawns the `codex exec` headless CLI
/// (vendor-aware routing — plan 540). The vendor selects the argv shape and the
/// binary name in `spawn.zig`.
pub const Vendor = enum {
    claude,
    codex,

    /// Parse a vendor from its wire string; null for anything unrecognized.
    pub fn fromString(s: []const u8) ?Vendor {
        if (std.mem.eql(u8, s, "claude")) return .claude;
        if (std.mem.eql(u8, s, "codex")) return .codex;
        return null;
    }

    /// The wire form (inverse of `fromString`). Also the spawn binary name.
    pub fn name(self: Vendor) []const u8 {
        return switch (self) {
            .claude => "claude",
            .codex => "codex",
        };
    }
};

/// A single role's effective dispatch target: which vendor CLI and which model.
/// `model` may be a comptime tier constant (the default) or an arena-owned
/// override string from `parseConfig`; either outlives the spawn that borrows it.
pub const Dispatch = struct {
    vendor: Vendor = .claude,
    model: []const u8,
};

/// The effective per-role dispatch mapping. Field defaults encode the task-3709
/// routing (sonnet coder, opus reviewer), all on the claude vendor.
pub const ModelTable = struct {
    coder: Dispatch = .{ .vendor = .claude, .model = SONNET_TIER },
    reviewer: Dispatch = .{ .vendor = .claude, .model = OPUS_TIER },
    @"test-coder": Dispatch = .{ .vendor = .claude, .model = SONNET_TIER },
    documenter: Dispatch = .{ .vendor = .claude, .model = SONNET_TIER },

    /// The full dispatch (vendor + model) for `role`.
    pub fn dispatchForRole(self: ModelTable, role: Role) Dispatch {
        return switch (role) {
            .coder => self.coder,
            .reviewer => self.reviewer,
            .@"test-coder" => self.@"test-coder",
            .documenter => self.documenter,
        };
    }

    /// The model string for `role` (convenience over `dispatchForRole`).
    pub fn forRole(self: ModelTable, role: Role) []const u8 {
        return self.dispatchForRole(role).model;
    }

    /// The vendor for `role` (convenience over `dispatchForRole`).
    pub fn vendorForRole(self: ModelTable, role: Role) Vendor {
        return self.dispatchForRole(role).vendor;
    }

    /// Assign a dispatch to the field matching `role`.
    pub fn set(self: *ModelTable, role: Role, d: Dispatch) void {
        switch (role) {
            .coder => self.coder = d,
            .reviewer => self.reviewer = d,
            .@"test-coder" => self.@"test-coder" = d,
            .documenter => self.documenter = d,
        }
    }
};

/// A `ModelTable` plus the arena that owns any override strings parsed from
/// config. Call `deinit` to release the overrides; the defaults are comptime
/// constants and are unaffected. When no override was applied, `arena` is null
/// and `deinit` is a no-op.
pub const ResolvedTable = struct {
    table: ModelTable = .{},
    arena: ?std.heap.ArenaAllocator = null,

    pub fn deinit(self: *ResolvedTable) void {
        if (self.arena) |*a| a.deinit();
    }
};

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

test "role_model: each known role maps to expected default tier (sonnet coder, opus reviewer)" {
    try std.testing.expectEqualStrings(SONNET_TIER, modelForRole(.coder));
    try std.testing.expectEqualStrings(OPUS_TIER, modelForRole(.reviewer));
    try std.testing.expectEqualStrings(SONNET_TIER, modelForRole(.@"test-coder"));
    try std.testing.expectEqualStrings(SONNET_TIER, modelForRole(.documenter));
}

test "role_model: default ModelTable matches modelForRole for every role" {
    const t = ModelTable{};
    inline for ([_]Role{ .coder, .reviewer, .@"test-coder", .documenter }) |r| {
        try std.testing.expectEqualStrings(modelForRole(r), t.forRole(r));
    }
}

test "role_model: fromString round-trips for every variant" {
    try std.testing.expectEqual(Role.coder, try Role.fromString("coder"));
    try std.testing.expectEqual(Role.reviewer, try Role.fromString("reviewer"));
    try std.testing.expectEqual(Role.@"test-coder", try Role.fromString("test-coder"));
    try std.testing.expectEqual(Role.documenter, try Role.fromString("documenter"));
}

test "role_model: name() inverts fromString" {
    inline for ([_]Role{ .coder, .reviewer, .@"test-coder", .documenter }) |r| {
        try std.testing.expectEqual(r, try Role.fromString(r.name()));
    }
}

test "role_model: unknown role string surfaces UnknownRole" {
    try std.testing.expectError(RoleError.UnknownRole, Role.fromString("planner"));
    try std.testing.expectError(RoleError.UnknownRole, Role.fromString(""));
    try std.testing.expectError(RoleError.UnknownRole, Role.fromString("CODER")); // case-sensitive
}

test "role_model: modelForRoleString convenience path" {
    try std.testing.expectEqualStrings(OPUS_TIER, try modelForRoleString("reviewer"));
    try std.testing.expectEqualStrings(SONNET_TIER, try modelForRoleString("coder"));
    try std.testing.expectEqualStrings(SONNET_TIER, try modelForRoleString("documenter"));
    try std.testing.expectError(RoleError.UnknownRole, modelForRoleString("nobody"));
}

test "role_model: tier constants are non-empty and stable" {
    // Pin the exact constants so a stealth edit fails the gate loudly.
    try std.testing.expectEqualStrings("claude-opus-4-8", OPUS_TIER);
    try std.testing.expectEqualStrings("claude-sonnet-4-6", SONNET_TIER);
}
