//! handlers/artifact/list — `planar artifact list [--kind --status --scope --plan]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

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
    var read_scope_slugs: []const []const u8 = &.{};
    defer if (read_scope_slugs.len > 0) scope_mod.deinitReadScopeFilterSlugs(ctx.allocator, read_scope_slugs);
    if (args.scope) |s| {
        var it = std.mem.splitScalar(u8, s, ',');
        while (it.next()) |tok| {
            const trimmed = std.mem.trim(u8, tok, " ");
            if (trimmed.len == 0) continue;
            try scopes.append(ctx.allocator, trimmed);
        }
    } else {
        const cwd = try scope_mod.operatorCwd(ctx.allocator, ctx.io);
        defer ctx.allocator.free(cwd);
        const read_scopes = try scope_mod.resolveForReadSet(ctx, cwd, null);
        defer ctx.allocator.free(read_scopes);
        if (read_scopes.len == 0) {
            exit.die(
                ctx,
                error.NoReadScope,
                "cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global",
                .{},
            );
        }
        read_scope_slugs = try scope_mod.readScopeFilterSlugs(ctx, read_scopes);
    }

    const filter: engine.planning.artifact.ListFilter = .{
        .statuses = statuses.items,
        .kinds = kinds.items,
        .plan_id = args.plan,
        .scopes = if (args.scope != null) scopes.items else read_scope_slugs,
    };

    const items = engine.planning.artifact.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "artifact list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.artifact, items, .{ .json = args.json });
}
