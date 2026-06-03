//! handlers/scenario/add — `planar scenario add <title> [--body --related --plan --editor --scope]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "scenario", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // cwd-derive fallback (plan 352 task 2450).
    const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
        exit.die(ctx, e, "scenario add: resolving scope failed: {s}", .{@errorName(e)});
    const effective_scope: ?[]const u8 = if (resolution.scope) |s| s else null;

    if (args.editor) {
        try ctx.stderr.print("warning: --editor not yet implemented; falling back to inline create\n", .{});
    }

    const s = engine.planning.scenario.create(d, ctx.allocator, .{
        .title = args.title,
        .body = args.body,
        .related_artifact_id = args.related,
        .plan_id = args.plan,
        .scope = effective_scope,
    }) catch |e| exit.die(ctx, e, "scenario add: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.scenario, s, .{ .json = args.json });
}
