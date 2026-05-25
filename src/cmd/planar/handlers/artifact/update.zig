//! handlers/artifact/update — `planar artifact update <artifact-id> [--title --body --source-path --status --scope]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "artifact", "update" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.artifact_id, 10) catch
        exit.die(ctx, error.InvalidInput, "artifact id must be an integer, got '{s}'", .{args.artifact_id});

    // --body supports @path; mirrors Go.
    var body_owned: ?[]u8 = null;
    defer if (body_owned) |b| ctx.allocator.free(b);
    var body_value: ?[]const u8 = null;
    if (args.body) |b| {
        body_owned = engine.planning.artifact.readBody(ctx.allocator, ctx.io, b) catch |e|
            exit.die(ctx, error.InvalidInput, "read --body: {s}", .{@errorName(e)});
        body_value = body_owned;
    }

    var patch: engine.planning.artifact.UpdateArgs = .{
        .title = args.title,
        .body = body_value,
        .source_path = args.source_path,
        .scope = args.scope,
    };
    if (args.status) |s| {
        patch.status = engine.planning.artifact.Status.fromText(s) orelse
            exit.die(ctx, error.InvalidInput, "unknown status '{s}'", .{s});
    }

    const art = engine.planning.artifact.update(d, ctx.allocator, id, patch) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no artifact with id {d}", .{id}),
        error.NoFields => exit.die(ctx, e, "at least one field must be specified for update", .{}),
        else => exit.die(ctx, e, "artifact update: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.artifact, art, .{ .json = args.json });
}
