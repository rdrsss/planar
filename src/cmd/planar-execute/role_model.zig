//! role_model.zig — Per-role model tier table (plan 492 task 3176).
//!
//! The `agent(prompt, opts)` host function spawns `claude --print --model
//! <tier>` workers. The model tier is selected by the worker's ROLE
//! (`opts.role`), NOT by a per-call override path. This module is the hardcoded
//! role→tier table that serves as the M4 "config singleton".
//!
//! ## Why hardcoded
//!
//! The tech spec § "Per-role model from config, no override" specifies that the
//! tier is sourced from harness config with no per-call override. At M4 the
//! config is a small enum table compiled into the binary; an operator-editable
//! `~/.planar/execute-config.toml` is OUT OF SCOPE for this milestone (filed as
//! a follow-up). Hardcoding makes the contract explicit and trivially auditable:
//! grep for the model id strings to find every dispatch path.
//!
//! ## Roles supported
//!
//! - `coder`       → opus tier (heavy lifting, schema-aware work).
//! - `reviewer`    → opus tier (must catch defects the coder missed).
//! - `test-coder`  → sonnet tier (mechanical test authoring is sonnet-suitable).
//! - `documenter`  → sonnet tier (docs polish is sonnet-suitable).
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

/// Opus tier — heavy reasoning roles (coder, reviewer).
pub const OPUS_TIER: []const u8 = "claude-opus-4-8";

/// Sonnet tier — mechanical roles (test-coder, documenter).
pub const SONNET_TIER: []const u8 = "claude-sonnet-4-6";

/// Returns the `--model` tier string for `role`. The returned slice points at
/// a comptime string constant and does NOT need freeing.
pub fn modelForRole(role: Role) []const u8 {
    return switch (role) {
        .coder, .reviewer => OPUS_TIER,
        .@"test-coder", .documenter => SONNET_TIER,
    };
}

/// Convenience: look up the tier directly from a role string. Returns
/// `RoleError.UnknownRole` if the role string is unrecognized.
pub fn modelForRoleString(s: []const u8) RoleError![]const u8 {
    const role = try Role.fromString(s);
    return modelForRole(role);
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

test "role_model: each known role maps to expected tier" {
    try std.testing.expectEqualStrings(OPUS_TIER, modelForRole(.coder));
    try std.testing.expectEqualStrings(OPUS_TIER, modelForRole(.reviewer));
    try std.testing.expectEqualStrings(SONNET_TIER, modelForRole(.@"test-coder"));
    try std.testing.expectEqualStrings(SONNET_TIER, modelForRole(.documenter));
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
    try std.testing.expectEqualStrings(OPUS_TIER, try modelForRoleString("coder"));
    try std.testing.expectEqualStrings(SONNET_TIER, try modelForRoleString("documenter"));
    try std.testing.expectError(RoleError.UnknownRole, modelForRoleString("nobody"));
}

test "role_model: tier constants are non-empty and stable" {
    // Pin the exact constants so a stealth edit fails the gate loudly.
    try std.testing.expectEqualStrings("claude-opus-4-8", OPUS_TIER);
    try std.testing.expectEqualStrings("claude-sonnet-4-6", SONNET_TIER);
}
