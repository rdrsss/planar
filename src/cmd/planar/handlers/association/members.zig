//! handlers/association/members — `planar assoc members <slug>`
//!
//! Returns a Project list rather than an Association — output rendering
//! goes through `renderProjectListText` on the association module
//! directly (the generic `emitList` is keyed on `renderListText`).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "assoc", "members" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const projects = engine.identity.association.members(d, ctx.allocator, args.slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no association named '{s}'", .{args.slug}),
        else => exit.die(ctx, e, "association members: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        try std.json.Stringify.value(projects, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try engine.identity.association.renderProjectListText(projects, ctx.stdout);
    }
}
