const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "local", "unlink" }, args_ptr);
    const ctx = runtime.current();

    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    var total: usize = 0;
    const maybe_kind = common.lookupKindForName(hr.sandbox_root, args.name, ctx.allocator) catch |e|
        exit.die(ctx, e, "determining sandbox kind failed: {s}", .{@errorName(e)});

    if (maybe_kind) |kind| {
        total += try runOne(ctx, hr.home_dir, args.name, kind, args.purge, args.json);
    } else {
        total += try runOne(ctx, hr.home_dir, args.name, .skill, args.purge, args.json);
        total += try runOne(ctx, hr.home_dir, args.name, .agent, args.purge, args.json);
    }

    if (!args.json and total == 0) {
        try ctx.stdout.print("no installs found for \"{s}\" (already unlinked, or no such name)\n", .{args.name});
    }
}

fn runOne(
    ctx: *const runtime.Ctx,
    home_dir: []const u8,
    name: []const u8,
    kind: engine.local.manifest.Kind,
    purge: bool,
    json: bool,
) !usize {
    const res = engine.local.link.unlink(name, kind, .{ .home_dir = home_dir, .purge = purge }, ctx.allocator) catch |e|
        exit.die(ctx, e, "unlinking {s} failed: {s}", .{ name, @errorName(e) });
    defer engine.local.link.deinitUnlinkResult(res, ctx.allocator);

    if (res.removed.len == 0 and res.purged_file.len == 0) return 0;

    if (json) {
        const GoRemoved = struct {
            vendor: []const u8,
            target_path: []const u8,
            source_path: []const u8,
            mode: []const u8,
            action: []const u8,
            warning: ?[]const u8,
            linked_at: ?[]const u8,
        };
        var removed_rows: std.ArrayList(GoRemoved) = .empty;
        defer removed_rows.deinit(ctx.allocator);
        for (res.removed) |row| {
            try removed_rows.append(ctx.allocator, .{
                .vendor = row.vendor,
                .target_path = row.target_path,
                .source_path = row.source_path,
                .mode = if (row.mode) |m| @tagName(m) else "",
                .action = row.action,
                .warning = if (row.warning.len > 0) row.warning else null,
                .linked_at = if (row.linked_at.len > 0) row.linked_at else null,
            });
        }
        try std.json.Stringify.value(.{
            .result = .{
                .Name = res.name,
                .Kind = @tagName(res.kind),
                .Removed = removed_rows.items,
                .PurgedFile = res.purged_file,
            },
        }, .{ .emit_null_optional_fields = false }, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("{s} ({s})\n", .{ name, @tagName(kind) });
        for (res.removed) |rec| {
            try ctx.stdout.print("  {s:<7}  removed  <-  {s}\n", .{ rec.vendor, rec.target_path });
        }
        if (res.purged_file.len > 0) {
            try ctx.stdout.print("  purged source file: {s}\n", .{res.purged_file});
        }
    }
    return res.removed.len;
}
