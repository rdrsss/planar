//! handlers/artifact/link — `planar artifact link <artifact-id> <ref> --relationship <rel>`
//!
//! Creates an entity_links row from an artifact to another entity.
//! ("kind:integer-id" form only — slug refs not supported per Go parity).
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! JSON shape (mirrors Go's artifactLinkJSON):
//!   { "ok": true, "id": <link-id>, "artifact_id": <n>, "to_kind": "...",
//!     "to_id": <n>, "relationship": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const EntityLinkResult = struct {
    ok: bool,
    id: i64,
    artifact_id: i64,
    to_kind: []const u8,
    to_id: i64,
    relationship: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "artifact", "link" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const artifact_id = std.fmt.parseInt(i64, args.artifact_id, 10) catch
        exit.die(ctx, error.InvalidInput, "artifact id must be an integer, got '{s}'", .{args.artifact_id});

    const rel_text = args.relationship orelse
        exit.die(ctx, error.InvalidInput, "--relationship is required", .{});

    const relationship = engine.entitylink.Relationship.fromText(rel_text) orelse
        exit.die(ctx, error.InvalidInput, "unknown relationship '{s}'", .{rel_text});

    const parsed_ref = engine.entitylink.parseRef(args.ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid ref '{s}': expected kind:integer-id", .{args.ref}),
        else => return e,
    };
    const to_kind = switch (parsed_ref) {
        .id => |r| r.kind,
        .slug => exit.die(ctx, error.InvalidInput, "slug refs are not supported; use kind:integer-id (e.g. plan:42)", .{}),
    };
    const to_id = switch (parsed_ref) {
        .id => |r| r.id,
        .slug => unreachable,
    };

    _ = args.scope;

    const link = engine.entitylink.add(d, ctx.allocator, .{
        .from_kind = .artifact,
        .from_id = artifact_id,
        .to_kind = to_kind,
        .to_id = to_id,
        .relationship = relationship,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, e, "link artifact:{d} -> {s}:{d} [{s}] already exists", .{ artifact_id, to_kind.toText(), to_id, rel_text }),
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        else => exit.die(ctx, e, "artifact link: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinit(link, ctx.allocator);

    if (args.json) {
        const result = EntityLinkResult{
            .ok = true,
            .id = link.id,
            .artifact_id = artifact_id,
            .to_kind = to_kind.toText(),
            .to_id = to_id,
            .relationship = rel_text,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("linked artifact:{d} -> {s}:{d}  [{s}]  (link id: {d})\n", .{
            artifact_id, to_kind.toText(), to_id, rel_text, link.id,
        });
    }
}
