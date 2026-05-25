const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "local", "list" }, args_ptr);
    const ctx = runtime.current();

    const hr = common.resolveHomeAndRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving sandbox root: {s}", .{@errorName(e)});
    defer {
        ctx.allocator.free(hr.home_dir);
        ctx.allocator.free(hr.sandbox_root);
    }

    const rows = engine.local.link.list(hr.home_dir, ctx.allocator) catch |e|
        exit.die(ctx, e, "listing sandbox installs: {s}", .{@errorName(e)});
    defer engine.local.link.deinitListRecords(rows, ctx.allocator);

    if (args.json) {
        for (rows) |r| {
            if (args.vendor) |v| {
                if (!std.mem.eql(u8, r.record.vendor, v)) continue;
            }
            try std.json.Stringify.value(toGoListRecord(r), .{ .emit_null_optional_fields = false }, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
        return;
    }

    if (rows.len == 0) {
        try ctx.stdout.print("no sandbox installs recorded\n", .{});
        return;
    }

    try ctx.stdout.print("{s:<12}  {s:<7}  {s:<7}  {s:<7}  {s}\n", .{ "name", "kind", "vendor", "status", "target" });
    var any = false;
    for (rows) |r| {
        if (args.vendor) |v| {
            if (!std.mem.eql(u8, r.record.vendor, v)) continue;
        }
        any = true;
        try ctx.stdout.print(
            "{s:<12}  {s:<7}  {s:<7}  {s:<7}  {s}\n",
            .{ r.name, @tagName(r.kind), r.record.vendor, r.record.action, r.record.target_path },
        );
    }
    if (!any) {
        try ctx.stdout.print("no rows matched filter\n", .{});
    }
}

fn toGoListRecord(r: engine.local.link.ListRecord) struct {
    Name: []const u8,
    Kind: []const u8,
    Record: struct {
        vendor: []const u8,
        target_path: []const u8,
        source_path: []const u8,
        mode: []const u8,
        action: []const u8,
        warning: ?[]const u8,
        linked_at: ?[]const u8,
    },
} {
    return .{
        .Name = r.name,
        .Kind = @tagName(r.kind),
        .Record = .{
            .vendor = r.record.vendor,
            .target_path = r.record.target_path,
            .source_path = r.record.source_path,
            .mode = if (r.record.mode) |m| @tagName(m) else "",
            .action = r.record.action,
            .warning = if (r.record.warning.len > 0) r.record.warning else null,
            .linked_at = if (r.record.linked_at.len > 0) r.record.linked_at else null,
        },
    };
}
