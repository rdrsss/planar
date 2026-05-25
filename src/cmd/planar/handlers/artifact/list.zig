//! handlers/artifact/list — `planar artifact list [--kind --status --scope --plan]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "artifact", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var statuses: std.ArrayList(engine.planning.artifact.Status) = .empty;
    defer statuses.deinit(ctx.allocator);
    if (args.status) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            const st = engine.planning.artifact.Status.fromText(trimmed) orelse
                exit.die(ctx, error.InvalidInput, "unknown status '{s}'", .{trimmed});
            try statuses.append(ctx.allocator, st);
        }
    }
    var kinds: std.ArrayList(engine.planning.artifact.Kind) = .empty;
    defer kinds.deinit(ctx.allocator);
    if (args.kind) |k| {
        var it = std.mem.splitScalar(u8, k, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            const kind = engine.planning.artifact.Kind.fromText(trimmed) orelse
                exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{trimmed});
            try kinds.append(ctx.allocator, kind);
        }
    }

    var scopes: std.ArrayList([]const u8) = .empty;
    defer scopes.deinit(ctx.allocator);
    if (args.scope) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            try scopes.append(ctx.allocator, trimmed);
        }
    }

    const filter: engine.planning.artifact.ListFilter = .{
        .statuses = statuses.items,
        .kinds = kinds.items,
        .plan_id = args.plan,
        .scopes = scopes.items,
    };

    const items = engine.planning.artifact.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "artifact list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.artifact, items, .{ .json = args.json });
}
