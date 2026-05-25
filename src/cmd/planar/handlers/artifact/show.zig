//! handlers/artifact/show — `planar artifact show <artifact-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "artifact", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.artifact_id, 10) catch
        exit.die(ctx, error.InvalidInput, "artifact id must be an integer, got '{s}'", .{args.artifact_id});

    const art = engine.planning.artifact.show(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no artifact with id {d}", .{id}),
        else => exit.die(ctx, e, "artifact show: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.artifact, art, .{ .json = args.json });
}
