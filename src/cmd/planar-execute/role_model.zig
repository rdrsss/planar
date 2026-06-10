//! role_model.zig — Per-role model tier table (plan 492 tasks 3176, 3708, 3709).
//!
//! The `agent(prompt, opts)` host function spawns `claude --print --model
//! <tier>` workers. The model is selected by the worker's ROLE (`opts.role`),
//! NOT by a per-call override path. This module owns the role→model mapping:
//! a compiled-in default table (`ModelTable{}`) plus a thin operator override
//! layer read from `${PLANAR_HOME:-~/.planar}/execute-config.toml`.
//!
//! ## Default routing — sonnet coder, opus reviewer (task 3709)
//!
//! - `coder`       → sonnet tier (structured authoring against a brief; the
//!                   opus reviewer is the load-bearing quality net behind it).
//! - `reviewer`    → opus tier (adversarial defect-finding; opus earns its cost).
//! - `test-coder`  → sonnet tier (mechanical test authoring is sonnet-suitable).
//! - `documenter`  → sonnet tier (docs polish is sonnet-suitable).
//!
//! All-opus everywhere doubled spend without doubling the quality signal; the
//! coder runs against a brief with the opus reviewer catching defects, so sonnet
//! is the right default there.
//!
//! ## Operator override (task 3709)
//!
//! `parseConfig` overlays a `[models]` TOML table onto the defaults:
//!
//!     [models]
//!     coder    = "claude-opus-4-8"   # opt the coder back up to opus
//!     reviewer = "claude-opus-4-8"
//!
//! Only the four known role keys are honored; unknown keys and malformed lines
//! are ignored (lenient — a typo never aborts a run). Unset roles keep their
//! default. The resolver is pure (bytes in, table out); the file read + path
//! resolution live in `main.zig` so this module stays free of I/O coupling.
//! Bumping a default still requires editing the tier constants below AND the
//! argv assertion tests so the contract change is loud rather than silent.
//!
//! ## Visibility (task 3708)
//!
//! `planar-execute run --dry-run` prints the effective role→model table (after
//! the config overlay), so an operator no longer has to grep this file to learn
//! what will spawn.
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
// ModelTable — the effective role→model mapping for a run (defaults + overlay)
// ---------------------------------------------------------------------------

/// The effective per-role model mapping. Field defaults encode the task-3709
/// routing (sonnet coder, opus reviewer). A field may point at either a comptime
/// tier constant (the default) or an arena-owned override string produced by
/// `parseConfig`; either way it outlives the spawn that borrows it.
pub const ModelTable = struct {
    coder: []const u8 = SONNET_TIER,
    reviewer: []const u8 = OPUS_TIER,
    @"test-coder": []const u8 = SONNET_TIER,
    documenter: []const u8 = SONNET_TIER,

    /// The model string for `role`.
    pub fn forRole(self: ModelTable, role: Role) []const u8 {
        return switch (role) {
            .coder => self.coder,
            .reviewer => self.reviewer,
            .@"test-coder" => self.@"test-coder",
            .documenter => self.documenter,
        };
    }

    /// Assign `model` to the field matching `role`.
    fn set(self: *ModelTable, role: Role, model: []const u8) void {
        switch (role) {
            .coder => self.coder = model,
            .reviewer => self.reviewer = model,
            .@"test-coder" => self.@"test-coder" = model,
            .documenter => self.documenter = model,
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

/// Overlay a `[models]` TOML table onto the default routing.
///
/// `bytes` is the raw `execute-config.toml` content. The parser is deliberately
/// minimal and lenient: it walks lines, tracks the current `[section]`, and
/// while inside `[models]` reads `key = "value"` (or `key = value`) pairs whose
/// key is one of the four known roles. Comments (`#`), blank lines, unknown
/// keys, unknown sections, and malformed lines are skipped — a typo in the
/// config never aborts a run, it just falls through to the default.
///
/// Override values are duped into a returned arena so they outlive `bytes`.
/// Returns only `error.OutOfMemory`. Caller owns the result and must
/// `deinit()` it.
pub fn parseConfig(gpa: std.mem.Allocator, bytes: []const u8) std.mem.Allocator.Error!ResolvedTable {
    var arena = std.heap.ArenaAllocator.init(gpa);
    errdefer arena.deinit();
    var table = ModelTable{};
    var applied = false;

    var in_models = false;
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw_line| {
        // Strip a trailing comment and surrounding whitespace.
        const no_comment = stripComment(raw_line);
        const line = std.mem.trim(u8, no_comment, " \t\r");
        if (line.len == 0) continue;

        if (line[0] == '[') {
            // Section header: [name]. Anything that isn't exactly [models]
            // takes us out of the models section.
            const close = std.mem.indexOfScalar(u8, line, ']') orelse {
                in_models = false;
                continue;
            };
            const name = std.mem.trim(u8, line[1..close], " \t");
            in_models = std.mem.eql(u8, name, "models");
            continue;
        }

        if (!in_models) continue;

        const eq = std.mem.indexOfScalar(u8, line, '=') orelse continue;
        const key = std.mem.trim(u8, line[0..eq], " \t");
        const role = Role.fromString(key) catch continue; // unknown key → skip
        const val = parseValue(std.mem.trim(u8, line[eq + 1 ..], " \t")) orelse continue;
        if (val.len == 0) continue;
        const owned = try arena.allocator().dupe(u8, val);
        table.set(role, owned);
        applied = true;
    }

    return .{
        .table = table,
        .arena = if (applied) arena else blk: {
            arena.deinit();
            break :blk null;
        },
    };
}

/// Drop an unquoted trailing `#…` comment from a TOML line. A `#` inside a
/// double-quoted value is preserved.
fn stripComment(line: []const u8) []const u8 {
    var in_quote = false;
    for (line, 0..) |ch, i| {
        switch (ch) {
            '"' => in_quote = !in_quote,
            '#' => if (!in_quote) return line[0..i],
            else => {},
        }
    }
    return line;
}

/// Extract the string content of a TOML scalar value: a double-quoted string
/// yields its inner bytes; a bare token is returned as-is. Returns null only
/// for an unterminated quote.
fn parseValue(tok: []const u8) ?[]const u8 {
    if (tok.len >= 2 and tok[0] == '"') {
        const end = std.mem.lastIndexOfScalar(u8, tok, '"') orelse return null;
        if (end == 0) return null;
        return tok[1..end];
    }
    return tok;
}

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

test "parseConfig: empty / no [models] section yields defaults, no arena" {
    const a = std.testing.allocator;
    inline for ([_][]const u8{ "", "# just a comment\n", "[other]\ncoder = \"x\"\n" }) |src| {
        var r = try parseConfig(a, src);
        defer r.deinit();
        try std.testing.expect(r.arena == null);
        try std.testing.expectEqualStrings(SONNET_TIER, r.table.coder);
        try std.testing.expectEqualStrings(OPUS_TIER, r.table.reviewer);
    }
}

test "parseConfig: overrides only the listed roles; unset roles keep defaults" {
    const a = std.testing.allocator;
    var r = try parseConfig(a,
        \\[models]
        \\coder = "claude-opus-4-8"
        \\documenter = "custom-model-x"
    );
    defer r.deinit();
    try std.testing.expect(r.arena != null);
    try std.testing.expectEqualStrings("claude-opus-4-8", r.table.coder);
    try std.testing.expectEqualStrings("custom-model-x", r.table.documenter);
    // Unset roles fall through to defaults.
    try std.testing.expectEqualStrings(OPUS_TIER, r.table.reviewer);
    try std.testing.expectEqualStrings(SONNET_TIER, r.table.@"test-coder");
}

test "parseConfig: lenient — comments, blanks, unknown keys, bare values, whitespace" {
    const a = std.testing.allocator;
    var r = try parseConfig(a,
        \\# header comment
        \\
        \\[models]
        \\  reviewer   =   "claude-opus-4-8"   # inline comment
        \\planner = "ignored-unknown-role"
        \\test-coder = bare-token-value
        \\garbage line with no equals
    );
    defer r.deinit();
    try std.testing.expectEqualStrings("claude-opus-4-8", r.table.reviewer);
    try std.testing.expectEqualStrings("bare-token-value", r.table.@"test-coder");
    // Unknown key did not leak into any field; coder stays default.
    try std.testing.expectEqualStrings(SONNET_TIER, r.table.coder);
}

test "parseConfig: section scoping — keys outside [models] are ignored" {
    const a = std.testing.allocator;
    var r = try parseConfig(a,
        \\[models]
        \\coder = "in-models"
        \\[budgets]
        \\reviewer = "should-be-ignored-not-a-model"
    );
    defer r.deinit();
    try std.testing.expectEqualStrings("in-models", r.table.coder);
    try std.testing.expectEqualStrings(OPUS_TIER, r.table.reviewer);
}

test "parseConfig: '#' inside a quoted value is preserved" {
    const a = std.testing.allocator;
    var r = try parseConfig(a,
        \\[models]
        \\coder = "model#with#hash"
    );
    defer r.deinit();
    try std.testing.expectEqualStrings("model#with#hash", r.table.coder);
}
