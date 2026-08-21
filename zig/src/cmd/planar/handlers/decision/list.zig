//! handlers/decision/list — `planar decision list [--scope --status]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var read_scope_slugs: []const []const u8 = &.{};
    defer if (read_scope_slugs.len > 0) scope_mod.deinitReadScopeFilterSlugs(ctx.allocator, read_scope_slugs);
    if (args.scope == null) {
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

    var filter: engine.planning.decision.ListFilter = .{
        .scope = args.scope,
        .scopes = read_scope_slugs,
        .plan_id = args.plan,
    };
    if (args.status) |s| {
        filter.status = engine.planning.decision.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidInput, "unknown status '{s}'", .{s});
    }

    const items = engine.planning.decision.list(d, ctx.allocator, filter) catch |e|
        exit.die(ctx, e, "decision list: {s}", .{@errorName(e)});

    try output.emitList(ctx, engine.planning.decision, items, .{ .json = args.json });
}
