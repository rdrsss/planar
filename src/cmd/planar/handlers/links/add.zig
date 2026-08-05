//! handlers/links/add — `planar links add <from-ref> <to-ref> --relationship <rel>`
//!
//! Creates an entity_links row between two arbitrary Planar entities.
//! Both arguments are typed entity references of the form kind:integer-id.
//! Slug refs are not supported (mirrors Go's ParseKindID which only accepts
//! integer ids; engine.entitylink.resolveRef for slugs has a compile-time
//! gap in Zig 0.16, tracked as follow-up engine-resolveref-slug-zig16).
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard).
//!
//! JSON shape (mirrors Go's linksAddJSON):
//!   { "ok": true, "id": <link-id>, "from_kind": "...", "from_id": <n>,
//!     "to_kind": "...", "to_id": <n>, "relationship": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const LinksAddResult = struct {
    ok: bool,
    id: i64,
    from_kind: []const u8,
    from_id: i64,
    to_kind: []const u8,
    to_id: i64,
    relationship: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "links", "add" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // --relationship is required in cmd.zig, so args.relationship is []const u8.
    const rel_text = args.relationship;

    const relationship = engine.entitylink.Relationship.fromText(rel_text) orelse
        exit.die(ctx, error.InvalidInput, "unknown relationship '{s}'", .{rel_text});

    // Parse from-ref.
    const from_parsed = engine.entitylink.parseRef(args.from_ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid from-ref '{s}': expected kind:integer-id", .{args.from_ref}),
        else => return e,
    };
    const from_kind = switch (from_parsed) {
        .id => |r| r.kind,
        .slug => exit.die(ctx, error.InvalidInput, "slug refs are not supported; use kind:integer-id (e.g. task:42)", .{}),
    };
    const from_id = switch (from_parsed) {
        .id => |r| r.id,
        .slug => unreachable,
    };

    // Parse to-ref.
    const to_parsed = engine.entitylink.parseRef(args.to_ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid to-ref '{s}': expected kind:integer-id", .{args.to_ref}),
        else => return e,
    };
    const to_kind = switch (to_parsed) {
        .id => |r| r.kind,
        .slug => exit.die(ctx, error.InvalidInput, "slug refs are not supported; use kind:integer-id (e.g. plan:42)", .{}),
    };
    const to_id = switch (to_parsed) {
        .id => |r| r.id,
        .slug => unreachable,
    };

    const add_args: engine.entitylink.AddArgs = .{
        .from_kind = from_kind,
        .from_id = from_id,
        .to_kind = to_kind,
        .to_id = to_id,
        .relationship = relationship,
    };

    const link = engine.entitylink.add(d, ctx.allocator, add_args) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, e, "link {s}:{d} -> {s}:{d} [{s}] already exists", .{ from_kind.toText(), from_id, to_kind.toText(), to_id, rel_text }),
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        // Name the offending side and ref. A bare "endpoint not found" would
        // leave the operator re-reading both halves of the command to work
        // out which id was wrong — and a mistyped id is the whole reason
        // this check exists.
        error.EndpointNotFound => switch (engine.entitylink.missingEndpoint(d, add_args) orelse .from) {
            .from => exit.die(ctx, e, "{s}:{d} not found", .{ from_kind.toText(), from_id }),
            .to => exit.die(ctx, e, "{s}:{d} not found", .{ to_kind.toText(), to_id }),
        },
        else => exit.die(ctx, e, "links add: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinit(link, ctx.allocator);

    if (args.json) {
        const result = LinksAddResult{
            .ok = true,
            .id = link.id,
            .from_kind = from_kind.toText(),
            .from_id = from_id,
            .to_kind = to_kind.toText(),
            .to_id = to_id,
            .relationship = rel_text,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("created entity_link: {s}:{d} --[{s}]--> {s}:{d}  (link id: {d})\n", .{
            from_kind.toText(), from_id, rel_text, to_kind.toText(), to_id, link.id,
        });
    }
}
