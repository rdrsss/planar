//! handlers/plan/create — `planar plan create <title> [--summary] [--slug] …`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");
const scope_mod = @import("../../scope.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "create" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const status = engine.planning.plan.Status.fromText(args.status) orelse
        exit.die(ctx, error.InvalidStatus, "unknown status '{s}'", .{args.status});

    // Resolve write scope: an explicit --scope wins; otherwise
    // cwd-derive looks up the project at cwd and uses its
    // single-association binding if one exists. Without this fall-
    // back, plan create silently lands plans in `global` even when
    // the operator is sitting inside a project bound to an
    // association — plan 352 task 2450.
    const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
        exit.die(ctx, e, "plan create: resolving scope failed: {s}", .{@errorName(e)});
    const effective_scope: ?[]const u8 = if (resolution.scope) |s| s else null;

    const plan = engine.planning.plan.create(d, ctx.allocator, .{
        .title = args.title,
        .slug = args.slug,
        .summary = args.summary,
        .status = status,
        .parent_plan_id = args.parent,
        .scope = effective_scope,
    }) catch |e| exit.die(ctx, e, "plan create: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.planning.plan, plan, .{ .json = args.json });
}
