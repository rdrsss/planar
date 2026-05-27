//! handlers/association/remove — `planar assoc remove <slug> <repo-path>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "remove" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    engine.identity.association.removeMember(d, ctx.allocator, args.slug, args.repo_path) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no association named '{s}'", .{args.slug}),
        error.NotAMember => exit.die(ctx, e, "no project registered at '{s}'", .{args.repo_path}),
        else => exit.die(ctx, e, "association remove: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try ctx.stdout.print(
            \\{{"status":"removed","association":"{s}","repo_path":"{s}"}}
        ++ "\n",
            .{ args.slug, args.repo_path },
        );
    } else {
        try ctx.stdout.print("removed project at {s} from {s}\n", .{ args.repo_path, args.slug });
    }
}
