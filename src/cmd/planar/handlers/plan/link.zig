//! handlers/plan/link — `planar plan link <plan-id> <ref> --relationship <rel>`
//!
//! Creates an entity_links row from a plan to another entity. The from_kind
//! is always 'plan'; the to-kind and to-id come from the <ref> positional
//! ("kind:integer-id" form — slug refs are not supported; mirrors Go's
//! ParseKindID which only accepts integer ids).
//!
//! Link verbs are documented UNGUARDED (CLAUDE.md §Cross-scope guard). No
//! scope_guard.check call is made here. --scope is accepted for CLI parity.
//!
//! JSON shape (mirrors Go's planLinkJSON):
//!   { "ok": true, "id": <link-id>, "plan_id": <n>, "to_kind": "...",
//!     "to_id": <n>, "relationship": "..." }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

const EntityLinkResult = struct {
    ok: bool,
    id: i64,
    plan_id: i64,
    to_kind: []const u8,
    to_id: i64,
    relationship: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "link" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    // --relationship is required for disambiguation.
    const rel_text = args.relationship orelse
        exit.die(ctx, error.InvalidInput, "--relationship is required", .{});

    const relationship = engine.entitylink.Relationship.fromText(rel_text) orelse
        exit.die(ctx, error.InvalidInput, "unknown relationship '{s}'", .{rel_text});

    // Parse the to-ref as kind:integer-id (mirrors Go ParseKindID — slug refs
    // are not supported here; engine.entitylink.resolveRef for slugs has a
    // compile-time gap in Zig 0.16, tracked as follow-up engine-resolveref-slug-zig16).
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

    // --scope accepted for parity; link verbs are unguarded.
    _ = args.scope;

    const link = engine.entitylink.add(d, ctx.allocator, .{
        .from_kind = .plan,
        .from_id = plan_id,
        .to_kind = to_kind,
        .to_id = to_id,
        .relationship = relationship,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, e, "link plan:{d} \u{2192} {s}:{d} [{s}] already exists", .{ plan_id, to_kind.toText(), to_id, rel_text }),
        error.UnsupportedScope => exit.die(ctx, e, "scoped entity links not yet supported (M3)", .{}),
        else => exit.die(ctx, e, "plan link: {s}", .{@errorName(e)}),
    };
    defer engine.entitylink.deinit(link, ctx.allocator);

    if (args.json) {
        const result = EntityLinkResult{
            .ok = true,
            .id = link.id,
            .plan_id = plan_id,
            .to_kind = to_kind.toText(),
            .to_id = to_id,
            .relationship = rel_text,
        };
        try std.json.Stringify.value(result, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("linked plan:{d} -> {s}:{d}  [{s}]  (link id: {d})\n", .{
            plan_id, to_kind.toText(), to_id, rel_text, link.id,
        });
    }
}
