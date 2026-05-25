const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "local", "migrate" }, args_ptr);
    const ctx = runtime.current();

    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    const res = engine.local.manifest.migrate(
        .{ .sandbox_root = hr.sandbox_root, .dry_run = args.dry_run },
        ctx.allocator,
    ) catch |e| exit.die(ctx, e, "migrate failed: {s}", .{@errorName(e)});
    defer engine.local.manifest.deinitMigrateResult(res, ctx.allocator);

    if (args.json) {
        try emitGoMigrateJSON(ctx, res);
        return;
    }

    if (res.migrated.len == 0 and res.skipped.len == 0) {
        try ctx.stdout.print("migrate: no legacy flat skills found; sandbox is already dir-shape\n", .{});
        return;
    }

    const verb = if (args.dry_run) "would migrate" else "migrated";
    for (res.migrated) |m| {
        try ctx.stdout.print("{s:<20}  {s}  {s} -> {s}\n", .{ m.name, verb, m.old_path, m.new_path });
    }
    for (res.skipped) |s| {
        try ctx.stdout.print("{s:<20}  skipped       {s}\n", .{ s.name, s.reason });
    }
    try ctx.stdout.print("\n", .{});
    try ctx.stdout.print("done: {s} {d} skill(s); skipped {d}\n", .{ verb, res.migrated.len, res.skipped.len });
}

fn emitGoMigrateJSON(ctx: *const runtime.Ctx, res: engine.local.manifest.MigrateResult) !void {
    const GoRecord = struct {
        Name: []const u8,
        OldPath: []const u8,
        NewPath: []const u8,
        Reason: []const u8,
    };
    var migrated: std.ArrayList(GoRecord) = .empty;
    defer migrated.deinit(ctx.allocator);
    var skipped: std.ArrayList(GoRecord) = .empty;
    defer skipped.deinit(ctx.allocator);
    for (res.migrated) |r| {
        try migrated.append(ctx.allocator, .{
            .Name = r.name,
            .OldPath = r.old_path,
            .NewPath = r.new_path,
            .Reason = r.reason,
        });
    }
    for (res.skipped) |r| {
        try skipped.append(ctx.allocator, .{
            .Name = r.name,
            .OldPath = r.old_path,
            .NewPath = r.new_path,
            .Reason = r.reason,
        });
    }
    try std.json.Stringify.value(.{
        .Migrated = migrated.items,
        .Skipped = skipped.items,
    }, .{}, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
