//! handlers/links/list — `planar links list <kind:id>`
//!
//! Lists all entity_links rows where the given entity appears as either the
//! source (from_kind/from_id) or target (to_kind/to_id), mirroring Go's
//! `links list` which uses an OR-based query.
//!
//! Implementation: two engine.entitylink.list calls (from-side, to-side)
//! are merged and de-duplicated by link-id. This preserves the D-no-engine-edits
//! constraint while matching Go's OR semantics.
//!
//! JSON output is NDJSON (one entityLinkJSON object per line), matching Go.
//! Text output is a table: id, direction (from/to), relationship, peer.
//!
//! JSON shape per row (mirrors Go's entityLinkJSON):
//!   { "id": <n>, "from_kind": "...", "from_id": <n>, "to_kind": "...",
//!     "to_id": <n>, "relationship": "...", "created_at": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const EntityLinkJSON = struct {
    id: i64,
    from_kind: []const u8,
    from_id: i64,
    to_kind: []const u8,
    to_id: i64,
    relationship: []const u8,
    created_at: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "list" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Parse the kind:id positional.
    const parsed = engine.entitylink.parseRef(args.ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid ref '{s}': expected kind:id (id must be an integer)", .{args.ref}),
        else => return e,
    };
    const entity_id = switch (parsed) {
        .id => |r| r.id,
        .slug => exit.die(ctx, error.InvalidInput, "links list requires a numeric id (got slug '{s}')", .{args.ref}),
    };
    const entity_kind = switch (parsed) {
        .id => |r| r.kind,
        .slug => unreachable,
    };

    // Fetch links where entity is the source.
    const from_links = engine.entitylink.list(d, ctx.allocator, .{
        .from_kind = entity_kind,
        .from_id = entity_id,
    }) catch |e|
        exit.die(ctx, e, "links list (from-side): {s}", .{@errorName(e)});
    defer engine.entitylink.deinitMany(from_links, ctx.allocator);

    // Fetch links where entity is the target.
    const to_links = engine.entitylink.list(d, ctx.allocator, .{
        .to_kind = entity_kind,
        .to_id = entity_id,
    }) catch |e|
        exit.die(ctx, e, "links list (to-side): {s}", .{@errorName(e)});
    defer engine.entitylink.deinitMany(to_links, ctx.allocator);

    if (args.json) {
        // Emit from-side rows.
        for (from_links) |l| {
            const row = EntityLinkJSON{
                .id = l.id,
                .from_kind = l.from_kind.toText(),
                .from_id = l.from_id,
                .to_kind = l.to_kind.toText(),
                .to_id = l.to_id,
                .relationship = l.relationship.toText(),
                .created_at = l.created_at,
            };
            try std.json.Stringify.value(row, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
        // Emit to-side rows, de-duplicating against from-side by id.
        for (to_links) |l| {
            var dup = false;
            for (from_links) |f| {
                if (f.id == l.id) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            const row = EntityLinkJSON{
                .id = l.id,
                .from_kind = l.from_kind.toText(),
                .from_id = l.from_id,
                .to_kind = l.to_kind.toText(),
                .to_id = l.to_id,
                .relationship = l.relationship.toText(),
                .created_at = l.created_at,
            };
            try std.json.Stringify.value(row, .{}, ctx.stdout);
            try ctx.stdout.print("\n", .{});
        }
    } else {
        const total = blk: {
            var count: usize = from_links.len;
            for (to_links) |l| {
                var dup = false;
                for (from_links) |f| {
                    if (f.id == l.id) {
                        dup = true;
                        break;
                    }
                }
                if (!dup) count += 1;
            }
            break :blk count;
        };

        if (total == 0) {
            try ctx.stdout.print("no links for {s}:{d}\n", .{ entity_kind.toText(), entity_id });
            return;
        }

        try ctx.stdout.print("{s:<6} {s:<10} {s:<16} {s}\n", .{ "id", "direction", "relationship", "peer" });

        for (from_links) |l| {
            const peer = l.to_kind.toText();
            try ctx.stdout.print("{d:<6} {s:<10} {s:<16} {s}:{d}\n", .{
                l.id, "from", l.relationship.toText(), peer, l.to_id,
            });
        }
        for (to_links) |l| {
            var dup = false;
            for (from_links) |f| {
                if (f.id == l.id) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            const peer = l.from_kind.toText();
            try ctx.stdout.print("{d:<6} {s:<10} {s:<16} {s}:{d}\n", .{
                l.id, "to", l.relationship.toText(), peer, l.from_id,
            });
        }
    }
}
