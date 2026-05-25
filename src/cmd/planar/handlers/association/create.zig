//! handlers/association/create — `planar assoc create <slug> [--name --kind]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "create" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    var create_args: engine.identity.association.CreateArgs = .{
        .slug = args.slug,
        .name = args.name,
    };
    if (args.kind) |k| {
        create_args.kind = engine.identity.association.Kind.fromText(k) orelse
            exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{k});
    }

    const assoc = engine.identity.association.create(d, ctx.allocator, create_args) catch |e|
        exit.die(ctx, e, "association create: {s}", .{@errorName(e)});

    try output.emit(ctx, engine.identity.association, assoc, .{ .json = args.json });
}
