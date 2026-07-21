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
//! (coder→medium, reviewer→large, the rest→medium). The plan-540 shared resolver
//! (phase 2) is the eventual single source of truth for external harnesses and
//! skills render.
//!
//! No DB handle is required; discovery is filesystem/PATH + subprocess only.

const std = @import("std");
const config = @import("config.zig");

// ---------------------------------------------------------------------------
// Shared model resolver (plan 540 phase 2) — the single authority that maps
// (vendor, role|tier) to a concrete model, reading the effective config
// (models.<vendor>.<tier> tier maps + roles.<role> role→tier) produced by
// engine.config.resolve(). Consumers (external harnesses, skills render,
// `planar models`) resolve through this instead of carrying their own tables.
// ---------------------------------------------------------------------------

/// Failure modes of the resolver.
pub const ResolveError = error{
    /// No `models.<vendor>.*` keys exist for the requested vendor.
    UnknownVendor,
    /// The vendor is known but has no model at the requested tier.
    UnknownTier,
    /// No `roles.<role>` mapping exists for the requested role.
    UnknownRole,
    /// The tier key exists but its model id is empty.
    MissingModel,
};

/// A resolved (vendor, tier) → model with provenance. `tier`/`model` borrow
/// from the effective map and live as long as it does.
pub const Resolution = struct {
    vendor: []const u8,
    tier: []const u8,
    model: []const u8,
    source: config.Provenance,
};

/// True when the effective map carries a `models.<vendor>.medium` key — the
/// canonical presence probe for a known vendor.
fn vendorKnown(eff: *const config.EffectiveMap, vendor: []const u8) bool {
    var buf: [160]u8 = undefined;
    const key = std.fmt.bufPrint(&buf, "models.{s}.medium", .{vendor}) catch return false;
    return eff.get(key) != null;
}

/// Resolve `(vendor, tier)` to a concrete model from the effective config.
pub fn resolveTier(eff: *const config.EffectiveMap, vendor: []const u8, tier: []const u8) ResolveError!Resolution {
    var buf: [160]u8 = undefined;
    const key = std.fmt.bufPrint(&buf, "models.{s}.{s}", .{ vendor, tier }) catch return ResolveError.UnknownTier;
    if (eff.get(key)) |vws| {
        if (vws.value.len == 0) return ResolveError.MissingModel;
        return .{ .vendor = vendor, .tier = tier, .model = vws.value, .source = vws.source };
    }
    if (!vendorKnown(eff, vendor)) return ResolveError.UnknownVendor;
    return ResolveError.UnknownTier;
}

/// Resolve `(vendor, role)` to a concrete model: `roles.<role>` gives the tier,
/// then `(vendor, tier)` gives the model. The returned `tier` is the resolved
/// tier; `source` reflects the model entry's provenance.
pub fn resolveRole(eff: *const config.EffectiveMap, vendor: []const u8, role: []const u8) ResolveError!Resolution {
    var buf: [160]u8 = undefined;
    const rkey = std.fmt.bufPrint(&buf, "roles.{s}", .{role}) catch return ResolveError.UnknownRole;
    const tier_vws = eff.get(rkey) orelse return ResolveError.UnknownRole;
    if (tier_vws.value.len == 0) return ResolveError.UnknownRole;
    return resolveTier(eff, vendor, tier_vws.value);
}

/// The vendor a role routes to: `role_vendors.<role>` if set, else the global
/// `[defaults].vendor`, else the compiled-in `default_vendor`. Borrows from the
/// effective map (or a comptime constant).
pub fn vendorForRole(eff: *const config.EffectiveMap, role: []const u8) []const u8 {
    var buf: [160]u8 = undefined;
    if (std.fmt.bufPrint(&buf, "role_vendors.{s}", .{role})) |rvkey| {
        if (eff.get(rvkey)) |v| {
            if (v.value.len > 0) return v.value;
        }
    } else |_| {}
    if (eff.get("defaults.vendor")) |dv| {
        if (dv.value.len > 0) return dv.value;
    }
    return default_vendor;
}

/// Resolve a role to a concrete model deriving the vendor from config
/// (`role_vendors.<role>` → `[defaults].vendor`). This is what `planar models
/// routing` and external harnesses consume — the full role→(vendor, tier, model)
/// path with no caller-supplied vendor.
pub fn resolveRoleAuto(eff: *const config.EffectiveMap, role: []const u8) ResolveError!Resolution {
    return resolveRole(eff, vendorForRole(eff, role), role);
}

// ---------------------------------------------------------------------------
// Work-type routing (plan 899 D1/D2/D4/D7/D9/D10/D11) — resolve(role,
// work_type). The plain resolveTier/resolveRole/resolveRoleAuto path above is
// UNCHANGED and keeps returning the tier default (list[0]) regardless of any
// routing entries; these are additive overloads-by-name for callers that also
// have a work type in hand (orchestrator dispatch, wired in a later
// milestone).
// ---------------------------------------------------------------------------

/// Resolve `(vendor, tier, work_type)` to a concrete model (plan 899 D9-D11).
/// Looks up the `[routing.<vendor>.<tier>]` map for `work_type`: on a hit whose
/// target is present in the tier's candidate list, returns the named
/// candidate; on a miss (including an unmapped `mechanical` under a custom
/// config) OR a hit whose target is NOT present in the tier's candidate list
/// (a stale/invalid routing entry — e.g. an operator narrowed
/// `[models.<vendor>.<tier>]` without also updating `[routing.*]`), falls back
/// to the tier default (`list[0]`, same value `resolveTier` returns). The
/// resolver is deliberately binary (tech-spec Architecture layer 2: hit → named
/// candidate, miss → `list[0]`) — there is no third error branch here.
/// Rejecting an invalid routing target is `planar config validate`'s job
/// (D10); this resolver treats a stale/invalid target exactly like a non-hit,
/// which is always a valid model (a member of the tier's own list).
pub fn resolveTierWorkType(
    eff: *const config.EffectiveMap,
    vendor: []const u8,
    tier: []const u8,
    work_type: []const u8,
) ResolveError!Resolution {
    var buf: [160]u8 = undefined;
    const model_key = std.fmt.bufPrint(&buf, "models.{s}.{s}", .{ vendor, tier }) catch return ResolveError.UnknownTier;
    const model_vws = eff.get(model_key) orelse {
        if (!vendorKnown(eff, vendor)) return ResolveError.UnknownVendor;
        return ResolveError.UnknownTier;
    };
    if (model_vws.value.len == 0) return ResolveError.MissingModel;

    var rbuf: [220]u8 = undefined;
    const routing_key = std.fmt.bufPrint(&rbuf, "routing.{s}.{s}.{s}", .{ vendor, tier, work_type }) catch
        return .{ .vendor = vendor, .tier = tier, .model = model_vws.value, .source = model_vws.source };

    if (eff.get(routing_key)) |route_vws| {
        if (route_vws.value.len > 0) {
            // candidates is always non-empty for a models.<vendor>.<tier> key
            // recorded by pickModelTierCandidates (candidates[0] == .value);
            // the single-element fallback below is defensive only.
            const candidates: []const []const u8 = if (model_vws.candidates.len > 0)
                model_vws.candidates
            else
                &[_][]const u8{model_vws.value};
            var found = false;
            for (candidates) |c| {
                if (std.mem.eql(u8, c, route_vws.value)) {
                    found = true;
                    break;
                }
            }
            if (found) return .{ .vendor = vendor, .tier = tier, .model = route_vws.value, .source = route_vws.source };
            // Stale/invalid routing target (absent from the tier's candidate
            // list): treat exactly like a non-hit and fall back to the tier
            // default. `planar config validate` (D10) is the enforcement
            // point that rejects this config in the first place; the
            // resolver never hard-errors here (tech-spec Architecture layer
            // 2 is binary: hit → candidate, miss → list[0]).
        }
    }

    // No routing entry for this work type (or a stale/invalid one): fall
    // back to the tier default.
    return .{ .vendor = vendor, .tier = tier, .model = model_vws.value, .source = model_vws.source };
}

/// Resolve `(vendor, role, work_type)`: `roles.<role>` gives the tier, then
/// `(vendor, tier, work_type)` gives the routed candidate. Mirrors
/// `resolveRole`'s role→tier lookup exactly.
pub fn resolveRoleWorkType(
    eff: *const config.EffectiveMap,
    vendor: []const u8,
    role: []const u8,
    work_type: []const u8,
) ResolveError!Resolution {
    var buf: [160]u8 = undefined;
    const rkey = std.fmt.bufPrint(&buf, "roles.{s}", .{role}) catch return ResolveError.UnknownRole;
    const tier_vws = eff.get(rkey) orelse return ResolveError.UnknownRole;
    if (tier_vws.value.len == 0) return ResolveError.UnknownRole;
    return resolveTierWorkType(eff, vendor, tier_vws.value, work_type);
}

/// Resolve `(role, work_type)` deriving the vendor from config exactly like
/// `resolveRoleAuto` — this is the `resolve(role, work_type)` entry point the
/// plan-899 tech-spec describes (Architecture layer 2): the full
/// role→(vendor, tier, routed-candidate) path with no caller-supplied vendor.
pub fn resolveRoleAutoWorkType(
    eff: *const config.EffectiveMap,
    role: []const u8,
    work_type: []const u8,
) ResolveError!Resolution {
    return resolveRoleWorkType(eff, vendorForRole(eff, role), role, work_type);
}

/// One row of the effective role routing table.
pub const RoutingRow = struct {
    role: []const u8,
    vendor: []const u8,
    tier: []const u8,
    model: []const u8,
    source: config.Provenance,
};

/// The canonical roles whose routing `planar models routing` reports.
pub const routing_roles = [_][]const u8{ "coder", "reviewer", "test-coder", "documenter", "doc-author", "sync-reconciler" };

/// Returns true when `role` is one of the built-in canonical roles.
fn isBuiltinRole(role: []const u8) bool {
    for (routing_roles) |r| {
        if (std.mem.eql(u8, r, role)) return true;
    }
    return false;
}

/// Build the effective routing table (role → vendor/tier/model + provenance)
/// for the canonical roles UNION any user-configured roles in `[roles]` /
/// `[role_vendors]`. Built-ins always appear first (in their declared order);
/// custom roles follow in the iteration order of the effective map. Rows borrow
/// from `eff`; the returned slice is owned by `allocator`. A role that fails to
/// resolve is skipped (should not happen for well-formed configs).
pub fn buildRouting(allocator: std.mem.Allocator, eff: *const config.EffectiveMap) std.mem.Allocator.Error![]RoutingRow {
    var list: std.ArrayListUnmanaged(RoutingRow) = try .initCapacity(allocator, routing_roles.len + 4);
    errdefer list.deinit(allocator);

    // 1. Built-in roles (guaranteed by embedded defaults).
    for (routing_roles) |role| {
        const r = resolveRoleAuto(eff, role) catch continue;
        try list.append(allocator, .{
            .role = role,
            .vendor = r.vendor,
            .tier = r.tier,
            .model = r.model,
            .source = r.source,
        });
    }

    // 2. Custom roles: any `roles.<name>` key whose name is not a built-in.
    //    Enumerate the effective map; skip non-roles keys and built-ins.
    var it = eff.iterator();
    while (it.next()) |entry| {
        const key = entry.key_ptr.*;
        const prefix = "roles.";
        if (!std.mem.startsWith(u8, key, prefix)) continue;
        const role = key[prefix.len..];
        if (isBuiltinRole(role)) continue;
        if (role.len == 0) continue;
        // Attempt resolution; skip silently if the config is malformed
        // (e.g. tier key present in [roles] but no models.<vendor>.<tier>).
        const r = resolveRoleAuto(eff, role) catch continue;
        try list.append(allocator, .{
            .role = role,
            .vendor = r.vendor,
            .tier = r.tier,
            .model = r.model,
            .source = r.source,
        });
    }

    return list.toOwnedSlice(allocator);
}

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

/// One curated model and the tier it belongs to. `id` is the spawn-safe model
/// identifier passed to the provider CLI's `--model`; `label` is a human display
/// name (plan 540 task 3633 — display labels are kept separate from spawn ids).
/// `label` defaults to empty, in which case consumers display `id`.
pub const CatalogModel = struct {
    id: []const u8,
    tier: Tier,
    label: []const u8 = "",
};

/// A provider's binary name plus its curated model catalog.
pub const VendorCatalog = struct {
    vendor: []const u8,
    /// The CLI binary discovery probes on PATH (also the spawn name in
    /// an external harness's spawn argument builder).
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
            .{ .id = "claude-fable-5", .tier = .large, .label = "Claude Fable 5 — Mythos-class, above opus; routable candidate, not the tier default" },
            .{ .id = "claude-sonnet-5", .tier = .medium },
            .{ .id = "claude-haiku-4-5", .tier = .small },
        },
    },
    .{
        .vendor = "codex",
        .bin = "codex",
        .models = &.{
            .{ .id = "gpt-5.6-sol", .tier = .large, .label = "GPT-5.6-sol (current) — frontier coding/research" },
            .{ .id = "gpt-5.5", .tier = .large, .label = "GPT-5.5 — prior frontier" },
            .{ .id = "gpt-5.6-terra", .tier = .medium, .label = "GPT-5.6-terra — strong everyday coding" },
            .{ .id = "gpt-5.4", .tier = .medium, .label = "GPT-5.4 — prior everyday coding" },
            .{ .id = "gpt-5.6-luna", .tier = .small, .label = "GPT-5.6-luna — fast, cost-efficient" },
            .{ .id = "gpt-5.4-mini", .tier = .small, .label = "GPT-5.4-mini — prior fast tier" },
            .{ .id = "gpt-5.3-codex-spark", .tier = .small, .label = "GPT-5.3-codex-spark — ultra-fast" },
        },
    },
};

/// The default role→tier routing (plan 541).
pub const RoleTier = struct { role: []const u8, tier: Tier };
pub const default_role_tiers: []const RoleTier = &.{
    .{ .role = "coder", .tier = .medium },
    .{ .role = "reviewer", .tier = .large },
    .{ .role = "test-coder", .tier = .medium },
    .{ .role = "documenter", .tier = .medium },
    .{ .role = "doc-author", .tier = .large },
    .{ .role = "sync-reconciler", .tier = .large },
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

/// Render the `[models.<vendor>]` tier maps + `[roles]` role→tier block as TOML
/// text from the curated catalog + default role tiers — the scaffold
/// `planar models apply` writes into a config file (plan 540 task 3740). The
/// values equal the embedded config defaults; materializing them gives an
/// operator an editable starting point. Caller owns the returned bytes.
pub fn renderConfigBlock(allocator: std.mem.Allocator) std.mem.Allocator.Error![]u8 {
    var ab: std.Io.Writer.Allocating = .init(allocator);
    defer ab.deinit();
    const w = &ab.writer;
    const tiers = [_]Tier{ .small, .medium, .large };
    w.print("# Model tier maps + role routing (plan 540). Generated by `planar models apply`.\n", .{}) catch return error.OutOfMemory;
    for (catalog) |vc| {
        w.print("[models.{s}]\n", .{vc.vendor}) catch return error.OutOfMemory;
        for (tiers) |t| {
            const id = modelForTier(vc.vendor, t);
            if (id.len > 0) w.print("{s} = \"{s}\"\n", .{ t.name(), id }) catch return error.OutOfMemory;
        }
        w.print("\n", .{}) catch return error.OutOfMemory;
    }
    w.print("[roles]\n", .{}) catch return error.OutOfMemory;
    for (default_role_tiers) |rt| {
        w.print("{s} = \"{s}\"\n", .{ rt.role, rt.tier.name() }) catch return error.OutOfMemory;
    }
    w.print("\n# Optional per-role vendor override (defaults to [defaults].vendor):\n# [role_vendors]\n# coder = \"codex\"\n", .{}) catch return error.OutOfMemory;
    return allocator.dupe(u8, ab.writer.buffered());
}

/// Whether `model_id` is a known (curated) model for `vendor`. A resolved model
/// that is NOT known is an operator-custom id — consumers mark it "unverified"
/// rather than treating it as a typo (plan 540 task 3633).
pub fn isKnownModel(vendor: []const u8, model_id: []const u8) bool {
    for (catalog) |vc| {
        if (!std.mem.eql(u8, vc.vendor, vendor)) continue;
        for (vc.models) |m| {
            if (std.mem.eql(u8, m.id, model_id)) return true;
        }
    }
    return false;
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
            try writer.print("      {s: <8} {s: <22}", .{ m.tier.name(), m.id });
            if (m.label.len > 0) try writer.print("  {s}", .{m.label});
            try writer.print("\n", .{});
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
            try testing.expectEqualStrings("claude-sonnet-5", r.model);
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
    try testing.expect(std.mem.indexOf(u8, out, "gpt-5.5") != null);
    try testing.expect(std.mem.indexOf(u8, out, "gpt-5.3-codex-spark") != null);
    try testing.expect(std.mem.indexOf(u8, out, "coder") != null);
    try testing.expect(std.mem.indexOf(u8, out, "claude-opus-4-8") != null);
}

// ---------------------------------------------------------------------------
// Resolver unit tests (plan 540 phase 2 / task 3621)
// ---------------------------------------------------------------------------

test "resolver: default resolution — role→tier→model from embedded defaults" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);

    const coder = try resolveRole(&res.effective, "claude", "coder");
    try testing.expectEqualStrings("medium", coder.tier);
    try testing.expectEqualStrings("claude-sonnet-5", coder.model);
    try testing.expectEqual(config.Provenance.embedded_default, coder.source);

    const reviewer = try resolveRole(&res.effective, "claude", "reviewer");
    try testing.expectEqualStrings("large", reviewer.tier);
    try testing.expectEqualStrings("claude-opus-4-8", reviewer.model);

    const codex_coder = try resolveRole(&res.effective, "codex", "coder");
    try testing.expectEqualStrings("gpt-5.6-terra", codex_coder.model);
}

test "resolver: config override resolution carries config-file provenance" {
    const a = testing.allocator;
    const file =
        \\[models.codex]
        \\medium = "gpt-5.5"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);

    const r = try resolveTier(&res.effective, "codex", "medium");
    try testing.expectEqualStrings("gpt-5.5", r.model);
    try testing.expectEqual(config.Provenance.config_file, r.source);

    // role path picks up the override too (coder=medium).
    const codex_coder = try resolveRole(&res.effective, "codex", "coder");
    try testing.expectEqualStrings("gpt-5.5", codex_coder.model);
    try testing.expectEqual(config.Provenance.config_file, codex_coder.source);
}

test "resolver: unknown vendor / unknown tier / unknown role errors" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);

    try testing.expectError(ResolveError.UnknownVendor, resolveTier(&res.effective, "gemini", "medium"));
    try testing.expectError(ResolveError.UnknownTier, resolveTier(&res.effective, "claude", "xl"));
    try testing.expectError(ResolveError.UnknownRole, resolveRole(&res.effective, "claude", "planner"));
}

test "resolver: missing model (empty value) surfaces MissingModel" {
    const a = testing.allocator;
    // Hand-build an effective map with an empty model id — not reachable via
    // real config (empty file values fall through to defaults), so construct
    // it directly to pin the MissingModel branch.
    var eff: config.EffectiveMap = .{};
    defer eff.deinit(a);
    const key = try a.dupe(u8, "models.claude.medium");
    try eff.put(a, key, .{ .value = "", .source = .config_file, .env_var_name = "" });
    defer a.free(key);

    try testing.expectError(ResolveError.MissingModel, resolveTier(&eff, "claude", "medium"));
}

test "resolver: resolveRoleAuto derives vendor (default → role_vendors override)" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);
    const coder = try resolveRoleAuto(&res.effective, "coder");
    try testing.expectEqualStrings("claude", coder.vendor);
    try testing.expectEqualStrings("claude-sonnet-5", coder.model);

    const file =
        \\[role_vendors]
        \\coder = "codex"
    ;
    var res2 = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res2.deinit(a);
    const coder2 = try resolveRoleAuto(&res2.effective, "coder");
    try testing.expectEqualStrings("codex", coder2.vendor);
    try testing.expectEqualStrings("gpt-5.6-terra", coder2.model);
    // A role without a per-role vendor stays on the default vendor.
    const reviewer2 = try resolveRoleAuto(&res2.effective, "reviewer");
    try testing.expectEqualStrings("claude", reviewer2.vendor);
}

test "resolver: vendorForRole falls back to [defaults].vendor" {
    const a = testing.allocator;
    const file =
        \\[defaults]
        \\vendor = "codex"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);
    // No role_vendors → every role uses the global default vendor (codex).
    try testing.expectEqualStrings("codex", vendorForRole(&res.effective, "coder"));
    const coder = try resolveRoleAuto(&res.effective, "coder");
    try testing.expectEqualStrings("gpt-5.6-terra", coder.model);
}

test "resolver: buildRouting returns a row per canonical role" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);
    const rows = try buildRouting(a, &res.effective);
    defer a.free(rows);
    try testing.expectEqual(routing_roles.len, rows.len);
    try testing.expectEqualStrings("coder", rows[0].role);
    try testing.expectEqualStrings("claude", rows[0].vendor);
    try testing.expectEqualStrings("claude-sonnet-5", rows[0].model);
}

test "resolver: buildRouting with custom role includes it after built-ins" {
    const a = testing.allocator;
    const file =
        \\[roles]
        \\compactor = "small"
        \\[role_vendors]
        \\compactor = "claude"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);
    const rows = try buildRouting(a, &res.effective);
    defer a.free(rows);
    // Built-ins plus one custom.
    try testing.expectEqual(routing_roles.len + 1, rows.len);
    // Built-ins remain first.
    try testing.expectEqualStrings("coder", rows[0].role);
    try testing.expectEqualStrings("reviewer", rows[1].role);
    // Last row is the custom role.
    const last = rows[rows.len - 1];
    try testing.expectEqualStrings("compactor", last.role);
    try testing.expectEqualStrings("claude", last.vendor);
    try testing.expectEqualStrings("small", last.tier);
    try testing.expectEqualStrings("claude-haiku-4-5", last.model);
}

test "resolver: buildRouting empty config — exactly six built-in rows" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);
    const rows = try buildRouting(a, &res.effective);
    defer a.free(rows);
    try testing.expectEqual(routing_roles.len, rows.len);
    try testing.expectEqualStrings("coder", rows[0].role);
    try testing.expectEqualStrings("claude-sonnet-5", rows[0].model);
    try testing.expectEqualStrings("reviewer", rows[1].role);
    try testing.expectEqualStrings("claude-opus-4-8", rows[1].model);
}

test "models: isKnownModel recognizes curated ids, rejects custom" {
    try testing.expect(isKnownModel("claude", "claude-sonnet-5"));
    try testing.expect(isKnownModel("codex", "gpt-5.5"));
    try testing.expect(!isKnownModel("codex", "gpt-9-imaginary"));
    try testing.expect(!isKnownModel("nope", "claude-sonnet-5"));
}

test "models: renderConfigBlock emits the tier maps + roles scaffold" {
    const a = testing.allocator;
    const block = try renderConfigBlock(a);
    defer a.free(block);
    for ([_][]const u8{
        "[models.claude]",      "[models.codex]", "claude-sonnet-5",
        "gpt-5.6-sol",          "[roles]",        "coder = \"medium\"",
        "reviewer = \"large\"",
    }) |needle| {
        try testing.expect(std.mem.indexOf(u8, block, needle) != null);
    }
}

// ---------------------------------------------------------------------------
// Work-type routing resolver tests (plan 899, task worktype-routing-map)
// ---------------------------------------------------------------------------

test "resolveTierWorkType: a routing-map hit returns the designated (non-default) candidate" {
    const a = testing.allocator;
    const file =
        \\[models.codex]
        \\large = ["gpt-5.5", "gpt-5.3-codex-spark"]
        \\[routing.codex.large]
        \\schema = "gpt-5.3-codex-spark"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);

    const r = try resolveTierWorkType(&res.effective, "codex", "large", "schema");
    try testing.expectEqualStrings("gpt-5.3-codex-spark", r.model);
    try testing.expectEqual(config.Provenance.config_file, r.source);

    // The role-level and auto-vendor overloads compose the same result.
    const via_role = try resolveRoleWorkType(&res.effective, "codex", "reviewer", "schema");
    try testing.expectEqualStrings("gpt-5.3-codex-spark", via_role.model);
}

test "resolveTierWorkType: unmapped work type falls back to the tier default (list[0])" {
    // Hand-build the effective map directly (mirrors the "missing model"
    // resolver test below) so this pins the resolver's fallback logic in
    // isolation from the shipped embedded default's per-vendor-tier
    // "mechanical" routing entry — going through config.resolve() would
    // always populate a "mechanical" entry, which is a separate scenario
    // (see "mechanical resolves via the shipped embedded default" below).
    const a = testing.allocator;
    var eff: config.EffectiveMap = .{};
    defer eff.deinit(a);

    const model_key = try a.dupe(u8, "models.codex.large");
    defer a.free(model_key);
    const candidates = try a.dupe([]const u8, &.{ "a-model", "b-model" });
    defer a.free(candidates);
    try eff.put(a, model_key, .{ .value = "a-model", .source = .config_file, .env_var_name = "", .candidates = candidates });

    const routing_key = try a.dupe(u8, "routing.codex.large.schema");
    defer a.free(routing_key);
    try eff.put(a, routing_key, .{ .value = "b-model", .source = .config_file, .env_var_name = "" });

    // "feature" has no routing entry at all — falls back to list[0], no error.
    const feature = try resolveTierWorkType(&eff, "codex", "large", "feature");
    try testing.expectEqualStrings("a-model", feature.model);

    // "mechanical" behaves identically — a routing key like any other, no
    // resolver carve-out (D11): also unmapped here, also falls back cleanly.
    const mechanical = try resolveTierWorkType(&eff, "codex", "large", "mechanical");
    try testing.expectEqualStrings("a-model", mechanical.model);

    // The mapped work type still resolves to its designated candidate.
    const schema = try resolveTierWorkType(&eff, "codex", "large", "schema");
    try testing.expectEqualStrings("b-model", schema.model);
}

test "resolveTierWorkType: mechanical resolves via the shipped embedded default" {
    const a = testing.allocator;
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);

    // Embedded default routes mechanical -> tier default for every vendor/tier.
    const r = try resolveTierWorkType(&res.effective, "claude", "medium", "mechanical");
    try testing.expectEqualStrings("claude-sonnet-5", r.model);
    try testing.expectEqual(config.Provenance.embedded_default, r.source);
}

test "resolveTierWorkType: a routing entry naming a model absent from the candidate list falls back to list[0] (resolver is binary; validation is D10's job)" {
    // The resolver never hard-errors on a stale/invalid routing target — that
    // rejection belongs to `planar config validate` (D10). Here the resolver
    // treats the invalid hit exactly like a non-hit and returns the tier
    // default, which is always a valid model (a member of the tier's own
    // list) — this is the scenario an operator hits by narrowing
    // `[models.<vendor>.<tier>]` without also updating `[routing.*]`.
    const a = testing.allocator;
    const file =
        \\[models.codex]
        \\large = ["a-model", "b-model"]
        \\[routing.codex.large]
        \\schema = "c-model"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);

    const r = try resolveTierWorkType(&res.effective, "codex", "large", "schema");
    try testing.expectEqualStrings("a-model", r.model);
    try testing.expectEqual(config.Provenance.config_file, r.source);
}

test "resolveTierWorkType: back-compat — a scalar tier resolves identically for every work type" {
    const a = testing.allocator;
    // Every embedded default is a scalar; no config file needed.
    var res = try config.resolve(a, null, std.process.Environ.empty, null);
    defer res.deinit(a);

    const work_types_local = [_][]const u8{ "schema", "engine", "architectural", "cli", "feature", "mechanical" };
    for (work_types_local) |wt| {
        const r = try resolveTierWorkType(&res.effective, "claude", "medium", wt);
        try testing.expectEqualStrings("claude-sonnet-5", r.model);
    }
}

test "resolveTierWorkType == plain resolveTier for the tier default (back-compat, unaffected by routing)" {
    const a = testing.allocator;
    const file =
        \\[models.codex]
        \\large = ["gpt-5.5", "gpt-5.3-codex-spark"]
        \\[routing.codex.large]
        \\schema = "gpt-5.3-codex-spark"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);

    // The plain resolveTier/resolveRole path is untouched by the routing map:
    // it always returns the tier default regardless of any routing entries.
    const plain = try resolveTier(&res.effective, "codex", "large");
    try testing.expectEqualStrings("gpt-5.5", plain.model);

    const plain_role = try resolveRole(&res.effective, "codex", "reviewer");
    try testing.expectEqualStrings("gpt-5.5", plain_role.model);

    // The work-type-aware path with a work type that has NO routing entry
    // agrees with the plain path.
    const routed_unmapped = try resolveTierWorkType(&res.effective, "codex", "large", "cli");
    try testing.expectEqualStrings(plain.model, routed_unmapped.model);
}

test "resolveRoleAutoWorkType: derives vendor exactly like resolveRoleAuto" {
    const a = testing.allocator;
    const file =
        \\[role_vendors]
        \\coder = "codex"
        \\[models.codex]
        \\medium = ["gpt-5.4", "gpt-5.4-mini"]
        \\[routing.codex.medium]
        \\engine = "gpt-5.4-mini"
    ;
    var res = try config.resolve(a, file, std.process.Environ.empty, null);
    defer res.deinit(a);

    const r = try resolveRoleAutoWorkType(&res.effective, "coder", "engine");
    try testing.expectEqualStrings("codex", r.vendor);
    try testing.expectEqualStrings("gpt-5.4-mini", r.model);

    // A work type with no routing entry falls back to resolveRoleAuto's result.
    const auto = try resolveRoleAuto(&res.effective, "coder");
    const routed_default = try resolveRoleAutoWorkType(&res.effective, "coder", "feature");
    try testing.expectEqualStrings(auto.model, routed_default.model);
}
