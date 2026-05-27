//! handlers/links/remove — `planar links remove <link-id>`
//!
//! Deletes an entity_links row by its primary key.
//!
//! JSON shape (mirrors Go's linksRemoveJSON):
//!   { "ok": true, "id": <link-id> }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const LinksRemoveResult = struct {
    ok: bool,
    id: i64,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "remove" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const link_id = std.fmt.parseInt(i64, args.link_id, 10) catch
        exit.die(ctx, error.InvalidInput, "link id must be an integer, got '{s}'", .{args.link_id});

    engine.entitylink.remove(d, ctx.allocator, link_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "entity link {d} not found", .{link_id}),
        else => exit.die(ctx, e, "links remove: {s}", .{@errorName(e)}),
    };

    if (args.json) {
        const result = LinksRemoveResult{ .ok = true, .id = link_id };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("entity link {d} removed\n", .{link_id});
    }
}
