//! engine/models.zig — provider + model capability discovery (plan 540/543).
//!
//! Backs the `planar models` verb. Two responsibilities:
//!
//!   1. A curated, in-repo catalog of the model identifiers each supported
//!      provider CLI exposes, classified into the canonical `small` / `medium`
//!      / `large` tiers (plan 541). The provider CLIs (`claude`, `codex`) do
//!      NOT expose a machine-readable "list models" command, so enumeration is
//!      impossible — the catalog is the authoritative known-models source and
//!      is maintained here by hand.
//!
//!   2. Local discovery: probe whether each provider CLI is installed by
//!      actually invoking `<bin> --version` (instant + auth-free for both
//!      claude and codex), recording installed-state + version. This is the
//!      "call claude and codex" half; it confirms callability, while the model
//!      list comes from the curated catalog.
//!
//! ## Tier model (plan 541)
//!
//! Roles map to tiers, tiers map to per-vendor models. The default routing here
//! (coder→medium, reviewer→large, the rest→medium) mirrors planar-execute's
//! `role_model.zig` defaults (sonnet coder, opus reviewer). These two tables
//! agree by construction today; the plan-540 shared resolver (phase 2) is the
//! eventual single source — until then, keep them in sync.
//!
//! No DB handle is required; discovery is filesystem/PATH + subprocess only.

const std = @import("std");

/// Canonical capability tiers (plan 541). `small` = cheap/fast, `large` =
/// heavy reasoning.
pub const Tier = enum {
    small,
    medium,
    large,

    pub fn name(self: Tier) []const u8 {
        return switch (self) {
            .small => "small",
            .medium => "medium",
            .large => "large",
        };
    }
};

/// One curated model id and the tier it belongs to.
pub const CatalogModel = struct {
    id: []const u8,
    tier: Tier,
};

/// A provider's binary name plus its curated model catalog.
pub const VendorCatalog = struct {
    vendor: []const u8,
    /// The CLI binary discovery probes on PATH (also the spawn name in
    /// planar-execute's `buildSpawnArgv`).
    bin: []const u8,
    models: []const CatalogModel,
};

/// The curated provider/model catalog. Hand-maintained — the provider CLIs do
/// not enumerate models. Tier assignments reflect the canonical v1 routing.
pub const catalog: []const VendorCatalog = &.{
    .{
        .vendor = "claude",
        .bin = "claude",
        .models = &.{
            .{ .id = "claude-opus-4-8", .tier = .large },
            .{ .id = "claude-sonnet-4-6", .tier = .medium },
            .{ .id = "claude-haiku-4-5", .tier = .small },
        },
    },
    .{
        .vendor = "codex",
        .bin = "codex",
        .models = &.{
            .{ .id = "gpt-5-codex", .tier = .large },
            .{ .id = "gpt-5", .tier = .medium },
            .{ .id = "o4-mini", .tier = .small },
        },
    },
};

/// The default role→tier routing (plan 541). Mirrors planar-execute's
/// role_model.zig defaults via the tier indirection.
pub const RoleTier = struct { role: []const u8, tier: Tier };
pub const default_role_tiers: []const RoleTier = &.{
    .{ .role = "coder", .tier = .medium },
    .{ .role = "reviewer", .tier = .large },
    .{ .role = "test-coder", .tier = .medium },
    .{ .role = "documenter", .tier = .medium },
};

/// The default vendor every role routes to unless overridden.
pub const default_vendor: []const u8 = "claude";

/// The outcome of probing a single provider binary.
pub const ProbeResult = struct {
    installed: bool,
    /// First line of `<bin> --version` output, owned by the caller's allocator;
    /// null when not installed or the version could not be read.
    version: ?[]const u8 = null,
};

/// A provider's discovered state plus its curated catalog (the wire shape).
pub const Provider = struct {
    vendor: []const u8,
    bin: []const u8,
    installed: bool,
    version: ?[]const u8,
    models: []const CatalogModel,
};

/// One resolved default-routing row: role → tier → (vendor, model).
pub const RoleRoute = struct {
    role: []const u8,
    tier: Tier,
    vendor: []const u8,
    model: []const u8,
};

/// The full `planar models` report.
pub const Report = struct {
    providers: []const Provider,
    default_routing: []const RoleRoute,
};

/// The claude model id for `tier` from the curated catalog (the default
/// vendor). Returns "" if the catalog has no model at that tier (should not
/// happen for the curated table).
pub fn modelForTier(vendor: []const u8, tier: Tier) []const u8 {
    for (catalog) |vc| {
        if (!std.mem.eql(u8, vc.vendor, vendor)) continue;
        for (vc.models) |m| {
            if (m.tier == tier) return m.id;
        }
    }
    return "";
}

/// Build the report from per-vendor probe results, given in `catalog` order.
/// Pure: no I/O. `probes.len` must equal `catalog.len`. The returned slices
/// borrow comptime catalog data and `probes`; they live as long as both do.
pub fn buildReport(allocator: std.mem.Allocator, probes: []const ProbeResult) std.mem.Allocator.Error!Report {
    std.debug.assert(probes.len == catalog.len);

    const providers = try allocator.alloc(Provider, catalog.len);
    for (catalog, probes, 0..) |vc, probe, i| {
        providers[i] = .{
            .vendor = vc.vendor,
            .bin = vc.bin,
            .installed = probe.installed,
            .version = probe.version,
            .models = vc.models,
        };
    }

    const routing = try allocator.alloc(RoleRoute, default_role_tiers.len);
    for (default_role_tiers, 0..) |rt, i| {
        routing[i] = .{
            .role = rt.role,
            .tier = rt.tier,
            .vendor = default_vendor,
            .model = modelForTier(default_vendor, rt.tier),
        };
    }

    return .{ .providers = providers, .default_routing = routing };
}

/// Probe one provider binary by invoking `<bin> --version`. A spawn failure
/// (binary not on PATH) means not-installed; a clean run captures the first
/// line of stdout as the version. The returned version (if any) is owned by
/// `allocator`.
pub fn probeBinary(allocator: std.mem.Allocator, io: std.Io, bin: []const u8) ProbeResult {
    const res = std.process.run(allocator, io, .{
        .argv = &.{ bin, "--version" },
    }) catch return .{ .installed = false, .version = null };
    defer allocator.free(res.stdout);
    defer allocator.free(res.stderr);

    const ok = switch (res.term) {
        .exited => |code| code == 0,
        else => false,
    };
    if (!ok) return .{ .installed = false, .version = null };

    // Version = first non-empty line of stdout, trimmed.
    var lines = std.mem.splitScalar(u8, res.stdout, '\n');
    while (lines.next()) |raw| {
        const line = std.mem.trim(u8, raw, " \t\r");
        if (line.len == 0) continue;
        const owned = allocator.dupe(u8, line) catch return .{ .installed = true, .version = null };
        return .{ .installed = true, .version = owned };
    }
    return .{ .installed = true, .version = null };
}

/// Discover all providers: probe each catalog binary, then build the report.
/// Caller owns the returned report's allocations (the provider slice, the
/// routing slice, and each non-null `version` string).
pub fn discover(allocator: std.mem.Allocator, io: std.Io) std.mem.Allocator.Error!Report {
    var probes = try allocator.alloc(ProbeResult, catalog.len);
    defer allocator.free(probes);
    for (catalog, 0..) |vc, i| {
        probes[i] = probeBinary(allocator, io, vc.bin);
    }
    return buildReport(allocator, probes);
}

/// Write the report to `${planar_home}/models/catalog.json` as deterministic
/// JSON (creating the `models/` dir). Returns the written path (owned by
/// `allocator`).
pub fn writeCache(
    allocator: std.mem.Allocator,
    io: std.Io,
    planar_home: []const u8,
    report: Report,
) ![]const u8 {
    const dir = try std.fs.path.join(allocator, &.{ planar_home, "models" });
    defer allocator.free(dir);
    std.Io.Dir.cwd().createDirPath(io, dir) catch {};

    const path = try std.fs.path.join(allocator, &.{ dir, "catalog.json" });
    errdefer allocator.free(path);

    var buf: std.Io.Writer.Allocating = .init(allocator);
    defer buf.deinit();
    try std.json.Stringify.value(report, .{ .whitespace = .indent_2 }, &buf.writer);
    try buf.writer.writeByte('\n');

    var f = try std.Io.Dir.cwd().createFile(io, path, .{});
    defer f.close(io);
    try f.writeStreamingAll(io, buf.writer.buffered());

    return path;
}

/// Human-readable rendering for `output.emit` (the JSON path is automatic).
pub fn renderText(report: Report, writer: *std.Io.Writer) std.Io.Writer.Error!void {
    try writer.print("providers:\n", .{});
    for (report.providers) |p| {
        const state = if (p.installed) "installed" else "not found";
        try writer.print("  {s: <8} [{s}]", .{ p.vendor, state });
        if (p.version) |v| try writer.print(" {s}", .{v});
        try writer.print("\n", .{});
        for (p.models) |m| {
            try writer.print("      {s: <8} {s}\n", .{ m.tier.name(), m.id });
        }
    }
    try writer.print("default routing (role → tier → vendor model):\n", .{});
    for (report.default_routing) |r| {
        try writer.print("  {s: <10} → {s: <6} {s: <6} {s}\n", .{ r.role, r.tier.name(), r.vendor, r.model });
    }
}

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

const testing = std.testing;

test "models: catalog has the two known vendors, each with a model at every tier" {
    try testing.expectEqual(@as(usize, 2), catalog.len);
    inline for ([_][]const u8{ "claude", "codex" }) |vendor| {
        inline for ([_]Tier{ .small, .medium, .large }) |tier| {
            try testing.expect(modelForTier(vendor, tier).len > 0);
        }
    }
}

test "models: default routing mirrors role_model (sonnet coder, opus reviewer)" {
    const a = testing.allocator;
    const probes = [_]ProbeResult{ .{ .installed = true }, .{ .installed = false } };
    const report = try buildReport(a, &probes);
    defer a.free(report.providers);
    defer a.free(report.default_routing);

    var saw_coder = false;
    var saw_reviewer = false;
    for (report.default_routing) |r| {
        if (std.mem.eql(u8, r.role, "coder")) {
            saw_coder = true;
            try testing.expectEqualStrings("claude", r.vendor);
            try testing.expectEqualStrings("claude-sonnet-4-6", r.model);
        }
        if (std.mem.eql(u8, r.role, "reviewer")) {
            saw_reviewer = true;
            try testing.expectEqualStrings("claude-opus-4-8", r.model);
        }
    }
    try testing.expect(saw_coder and saw_reviewer);
}

test "models: buildReport reflects probe installed-state and version per vendor" {
    const a = testing.allocator;
    const probes = [_]ProbeResult{
        .{ .installed = true, .version = "1.2.3" },
        .{ .installed = false, .version = null },
    };
    const report = try buildReport(a, &probes);
    defer a.free(report.providers);
    defer a.free(report.default_routing);

    try testing.expectEqual(@as(usize, 2), report.providers.len);
    try testing.expectEqualStrings("claude", report.providers[0].vendor);
    try testing.expect(report.providers[0].installed);
    try testing.expectEqualStrings("1.2.3", report.providers[0].version.?);
    try testing.expectEqualStrings("codex", report.providers[1].vendor);
    try testing.expect(!report.providers[1].installed);
    try testing.expect(report.providers[1].version == null);
}

test "models: renderText lists providers + default routing" {
    const a = testing.allocator;
    const probes = [_]ProbeResult{ .{ .installed = true, .version = "v1" }, .{ .installed = false } };
    const report = try buildReport(a, &probes);
    defer a.free(report.providers);
    defer a.free(report.default_routing);

    var buf: std.Io.Writer.Allocating = .init(a);
    defer buf.deinit();
    try renderText(report, &buf.writer);
    const out = buf.writer.buffered();
    try testing.expect(std.mem.indexOf(u8, out, "claude") != null);
    try testing.expect(std.mem.indexOf(u8, out, "codex") != null);
    try testing.expect(std.mem.indexOf(u8, out, "gpt-5-codex") != null);
    try testing.expect(std.mem.indexOf(u8, out, "coder") != null);
    try testing.expect(std.mem.indexOf(u8, out, "claude-opus-4-8") != null);
}
