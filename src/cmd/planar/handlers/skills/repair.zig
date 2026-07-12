const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "skills", "repair" }, args_ptr);
    const ctx = runtime.current();
    if (args.apply and args.dry_run) exit.die(ctx, error.InvalidInput, "skills repair: --apply and --dry-run are mutually exclusive", .{});
    const names = collectNames(ctx.allocator, ctx.argv) catch |e| exit.die(ctx, e, "skills repair: parsing names: {s}", .{@errorName(e)});
    defer {
        for (names) |name| ctx.allocator.free(name);
        ctx.allocator.free(names);
    }
    const homes = common.resolveHomes(ctx) catch |e| exit.die(ctx, e, "skills repair: resolving install homes: {s}", .{@errorName(e)});
    defer homes.deinit(ctx.allocator);
    var result = engine.installedsurface.repair(ctx.allocator, .{
        .status = homes.options(args.vendor),
        .names = names,
        .apply = args.apply,
    }) catch |e| switch (e) {
        error.InvalidVendor => exit.die(ctx, error.InvalidInput, "skills repair: --vendor must be claude, codex, or copilot", .{}),
        error.UnmanagedOrUnknownProjection => exit.die(ctx, error.InvalidInput, "skills repair: every name must identify a manifest-owned projection", .{}),
        else => exit.die(ctx, e, "skills repair: {s}", .{@errorName(e)}),
    };
    defer result.deinit();
    if (args.json) try common.emitRepairJson(ctx, result) else try common.emitRepairText(ctx, result);
    if (result.failed > 0) exit.die(ctx, error.RepairFailed, "skills repair completed with {d} failure(s); follow the reported recovery action", .{result.failed});
}

fn collectNames(allocator: std.mem.Allocator, argv: []const []const u8) ![][]const u8 {
    var start: ?usize = null;
    var i: usize = 1;
    while (i + 1 < argv.len) : (i += 1) {
        if (std.mem.eql(u8, argv[i], "skills") and std.mem.eql(u8, argv[i + 1], "repair")) {
            start = i + 2;
            break;
        }
    }
    var names: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (names.items) |name| allocator.free(name);
        names.deinit(allocator);
    }
    var cursor = start orelse return names.toOwnedSlice(allocator);
    var passthrough = false;
    while (cursor < argv.len) : (cursor += 1) {
        const token = argv[cursor];
        if (!passthrough and std.mem.eql(u8, token, "--")) {
            passthrough = true;
            continue;
        }
        if (!passthrough and std.mem.eql(u8, token, "--vendor")) {
            if (cursor + 1 < argv.len) cursor += 1;
            continue;
        }
        if (!passthrough and std.mem.startsWith(u8, token, "--vendor=")) continue;
        if (!passthrough and (std.mem.eql(u8, token, "--apply") or std.mem.eql(u8, token, "--dry-run") or std.mem.eql(u8, token, "--json"))) continue;
        try names.append(allocator, try allocator.dupe(u8, token));
    }
    return names.toOwnedSlice(allocator);
}
