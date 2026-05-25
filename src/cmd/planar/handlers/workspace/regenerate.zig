const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workspace", "regenerate" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const org = engine.identity.workspace.resolveOrg(d, ctx.allocator, args.workspace) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "no org associations registered; create one with `planar workspace init`", .{}),
        error.InvalidInput => exit.die(ctx, error.InvalidInput, "multiple org associations registered; pass the workspace slug or id explicitly", .{}),
        else => exit.die(ctx, e, "resolving workspace failed: {s}", .{@errorName(e)}),
    };
    defer engine.identity.workspace.deinitWorkspace(org, ctx.allocator);

    const result = engine.workspace.regenerate.regenerate(d, ctx.allocator, ctx.io, ctx.environ, org.id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "routing table not found; run `planar workspace routing build` first", .{}),
        else => exit.die(ctx, e, "regenerating AGENTS.md failed: {s}", .{@errorName(e)}),
    };
    defer engine.workspace.regenerate.deinitResult(result, ctx.allocator);

    if (args.json) {
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }
    try ctx.stdout.print(
        "regenerated AGENTS.md for org:{s} ({d} projects, {d} bytes)\n",
        .{ org.slug, result.project_count, result.bytes_written },
    );
}
