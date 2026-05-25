//! handlers/association/add — `planar assoc add <slug> <repo-path>`
//!
//! Looks up (or auto-registers) the project at `repo-path`, then joins
//! it to the association via the `project_associations` table with
//! source='user'.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    engine.identity.association.addMember(d, ctx.allocator, args.slug, args.repo_path, .user) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no association named '{s}'", .{args.slug}),
        error.AlreadyMember => exit.die(ctx, e, "project at '{s}' is already a member of '{s}'", .{ args.repo_path, args.slug }),
        else => exit.die(ctx, e, "association add: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try ctx.stdout.print(
            \\{{"status":"added","association":"{s}","repo_path":"{s}"}}
        ++ "\n",
            .{ args.slug, args.repo_path },
        );
    } else {
        try ctx.stdout.print("added project at {s} to {s}\n", .{ args.repo_path, args.slug });
    }
}
