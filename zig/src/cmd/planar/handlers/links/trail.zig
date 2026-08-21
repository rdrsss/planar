//! handlers/links/trail — `planar links trail <link-id>`
//!
//! Shows the audit trail for the given entity_link id. Returns all audit_log
//! rows where entity_kind='entity_link' and entity_id=<link-id>.
//!
//! JSON shape per row (AuditRow fields):
//!   { "id": <n>, "verb": "...", "entity_kind": "...", "entity_id": <n>,
//!     "actor": <string|null>, "scope": <string|null>, "summary": <string|null>,
//!     "recorded_at": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "trail" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const link_id = std.fmt.parseInt(i64, args.link_id, 10) catch
        exit.die(ctx, error.InvalidInput, "link id must be an integer, got '{s}'", .{args.link_id});

    const rows = engine.entitylink.trail(d, ctx.allocator, link_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "entity link {d} not found", .{link_id}),
        else => exit.die(ctx, e, "links trail: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinitAuditRows(rows, ctx.allocator);

    if (args.json) {
        for (rows) |r| {
            try std.json.Stringify.value(r, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
    } else {
        if (rows.len == 0) {
            try ctx.stdout.print("no audit trail for entity_link:{d}\n", .{link_id});
            return;
        }
        try ctx.stdout.print("{s:<6} {s:<12} {s:<12} {s}\n", .{ "id", "verb", "actor", "recorded_at" });
        for (rows) |r| {
            const actor = r.actor orelse "(none)";
            try ctx.stdout.print("{d:<6} {s:<12} {s:<12} {s}\n", .{ r.id, r.verb, actor, r.recorded_at });
        }
    }
}
