const std = @import("std");
const engine = @import("engine");
const runtime = @import("runtime");

pub const ResolvedHomes = struct {
    planar_home: []const u8,
    home: []const u8,
    codex_home: []const u8,

    pub fn deinit(self: ResolvedHomes, allocator: std.mem.Allocator) void {
        allocator.free(self.planar_home);
        allocator.free(self.home);
        allocator.free(self.codex_home);
    }

    pub fn options(self: ResolvedHomes, vendor: ?[]const u8) engine.installedsurface.Options {
        return .{
            .planar_home = self.planar_home,
            .home = self.home,
            .codex_home = self.codex_home,
            .vendor = vendor,
        };
    }
};

pub fn resolveHomes(ctx: *const runtime.Ctx) !ResolvedHomes {
    const home = ctx.environ.getPosix("HOME") orelse return error.HomeNotSet;
    const planar_home = if (ctx.environ.getPosix("PLANAR_HOME")) |value|
        try ctx.allocator.dupe(u8, value)
    else
        try std.fs.path.join(ctx.allocator, &.{ home, ".planar" });
    errdefer ctx.allocator.free(planar_home);
    const codex_home = if (ctx.environ.getPosix("CODEX_HOME")) |value|
        try ctx.allocator.dupe(u8, value)
    else
        try std.fs.path.join(ctx.allocator, &.{ home, ".codex" });
    errdefer ctx.allocator.free(codex_home);
    return .{
        .planar_home = planar_home,
        .home = try ctx.allocator.dupe(u8, home),
        .codex_home = codex_home,
    };
}

pub fn emitStatusJson(ctx: *const runtime.Ctx, result: engine.installedsurface.StatusResult) !void {
    const manifest = .{
        .status = result.manifest_status,
        .path = result.manifest_path,
        .version = result.manifest_version,
        .build_id = result.build_id,
        .install_mode = result.install_mode,
        .reason = result.reason,
    };
    try std.json.Stringify.value(.{
        .manifest = manifest,
        .vendors = result.vendors,
        .projections = result.projections,
        .summary = result.summary,
        .repair_command = result.repair_command,
    }, .{ .emit_null_optional_fields = false }, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}

pub fn emitStatusText(ctx: *const runtime.Ctx, result: engine.installedsurface.StatusResult) !void {
    try ctx.stdout.print("Install manifest: {s} ({s})\n", .{ @tagName(result.manifest_status), result.manifest_path });
    if (result.reason) |reason| try ctx.stdout.print("  {s}\n", .{reason});
    for (result.vendors) |vendor| {
        try ctx.stdout.print("Vendor {s}: {s} ({d} managed)\n", .{ vendor.vendor, @tagName(vendor.status), vendor.managed_count });
    }
    for (result.projections) |projection| {
        try ctx.stdout.print("{s} {s} {s}: {s}\n  {s}\n", .{
            projection.vendor,
            projection.kind,
            projection.name,
            @tagName(projection.status),
            projection.installed_path,
        });
        if (projection.status != .fresh) try ctx.stdout.print("  {s}\n", .{projection.reason});
    }
    try ctx.stdout.print("Summary: fresh={d} stale={d} missing={d} unmanaged={d} unselected_vendors={d}\n", .{
        result.summary.fresh,
        result.summary.stale,
        result.summary.missing,
        result.summary.unmanaged,
        result.summary.unselected_vendors,
    });
    if (result.repair_command) |command| try ctx.stdout.print("Repair: {s}\n", .{command});
}

pub fn emitRepairJson(ctx: *const runtime.Ctx, result: engine.installedsurface.RepairResult) !void {
    try std.json.Stringify.value(.{
        .mode = result.mode,
        .outcome = result.outcome,
        .manifest_status = result.manifest_status,
        .attempted = result.attempted,
        .applied = result.applied,
        .skipped = result.skipped,
        .failed = result.failed,
        .actions = result.actions,
        .next_action = result.next_action,
    }, .{ .emit_null_optional_fields = false }, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}

pub fn emitRepairText(ctx: *const runtime.Ctx, result: engine.installedsurface.RepairResult) !void {
    try ctx.stdout.print("Skills repair ({s}): {s}\n", .{ result.mode, result.outcome });
    for (result.actions) |action| {
        try ctx.stdout.print("{s} {s} {s}: {s} ({s} -> {s})\n", .{
            action.vendor,
            action.kind,
            action.name,
            action.action,
            @tagName(action.before),
            @tagName(action.post_status),
        });
        if (action.error_name) |name| try ctx.stdout.print("  failed: {s}\n", .{name});
        if (action.next_action) |next| try ctx.stdout.print("  recovery: {s}\n", .{next});
    }
    try ctx.stdout.print("Actions: attempted={d} applied={d} skipped={d} failed={d}\n", .{
        result.attempted,
        result.applied,
        result.skipped,
        result.failed,
    });
    if (result.next_action) |next| try ctx.stdout.print("Next: {s}\n", .{next});
}
