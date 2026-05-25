//! handlers/link.zig — `planar link <kind:id> --to <system-slug>:<external-id>`
//!
//! Top-level link verb: records an external_links row linking a local entity
//! to an existing external ticket. Does NOT push data to the remote (that's
//! `ext create`); use this when the external ticket was created out-of-band.
//!
//! Distinct from `<entity> link …`, which creates intra-DB entity_links.
//! Cross-scope guard applies — writing external_links is a mutation against
//! the local entity's external surface. The `--propagate` flag from the Go
//! binary is M10 territory (templates + extsync engine); we accept the flag
//! to keep CLI parity but refuse with NotImplemented when set.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");
const exit = @import("../exit.zig");
const propagate_handler = @import("ext/propagate.zig");

pub const verb: cli.Cmd = .{
    .name = "link",
    .desc = "Link a local entity to an external-system ticket.",
    .flags = &.{
        .{ .long = "--to", .kind = .string, .required = true, .desc = "<system-slug>:<external-id>" },
        .{ .long = "--role", .kind = .string, .desc = "Link role: mirror, parent, child, reference (default: reference)" },
        .{ .long = "--sync", .kind = .string, .desc = "Sync direction: read-only, write-back, two-way (default: read-only)" },
        .{ .long = "--propagate", .kind = .bool, .default = .{ .bool = false }, .desc = "Propagate feature after linking (M10)" },
        .{ .long = "--scope", .kind = .string },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "ref", .kind = .string, .required = true, .desc = "Entity ref (kind:id)" },
    },
    .run = cli.handler(handle),
};

const LinkCreateJSON = struct {
    ok: bool,
    link_id: i64,
    entity_kind: []const u8,
    entity_id: i64,
    external_id: []const u8,
    system_id: i64,
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"link"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    _ = args.scope;

    const entity = parseKindIDRef(args.ref) catch
        exit.die(ctx, error.InvalidInput, "invalid entity ref '{s}'; expected kind:integer-id", .{args.ref});

    const slug_and_id = parseToRef(args.to) catch
        exit.die(ctx, error.InvalidInput, "invalid --to value '{s}'; expected <system-slug>:<external-id>", .{args.to});

    const sys = engine.external.system.showBySlug(d, ctx.allocator, slug_and_id.slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "external system '{s}' not found", .{slug_and_id.slug}),
        else => exit.die(ctx, e, "link: lookup system: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    const role_text = args.role orelse "reference";
    const sync_text = args.sync orelse "read-only";

    const role = engine.external.link.LinkRole.fromText(role_text) orelse
        exit.die(ctx, error.InvalidInput, "invalid --role '{s}'", .{role_text});
    const sync_dir = engine.external.link.SyncDirection.fromText(sync_text) orelse
        exit.die(ctx, error.InvalidInput, "invalid --sync '{s}'", .{sync_text});
    const entity_kind = engine.external.link.ExternalEntityKind.fromText(entity.kind) orelse
        exit.die(ctx, error.InvalidInput, "unsupported entity kind '{s}'", .{entity.kind});

    const link = engine.external.link.create(d, ctx.allocator, .{
        .entity_kind = entity_kind,
        .entity_id = entity.id,
        .system_id = sys.id,
        .external_id = slug_and_id.external_id,
        .link_role = role,
        .sync_direction = sync_dir,
        .initial_status = .never,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, error.AlreadyExists, "link already exists for {s}:{d} on {s}", .{ entity.kind, entity.id, slug_and_id.slug }),
        else => exit.die(ctx, e, "link create: {s}", .{@errorName(e)}),
    };
    defer engine.external.link.deinit(link, ctx.allocator);

    if (args.json) {
        const out = LinkCreateJSON{
            .ok = true,
            .link_id = link.id,
            .entity_kind = entity.kind,
            .entity_id = entity.id,
            .external_id = slug_and_id.external_id,
            .system_id = sys.id,
        };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print(
            "linked {s}:{d} → {s}:{s}  (link id: {d}, {s} {s})\n",
            .{ entity.kind, entity.id, slug_and_id.slug, slug_and_id.external_id, link.id, sync_text, role_text },
        );
    }

    // --propagate: locate the anchor plan for the linked entity and run the
    // happy-path propagation against the system we just linked to.
    if (args.propagate) {
        const anchor = anchorPlanFor(d, ctx.allocator, entity.kind, entity.id) catch |e|
            exit.die(ctx, e, "--propagate: locating anchor plan for {s}:{d}: {s}", .{ entity.kind, entity.id, @errorName(e) });
        try propagate_handler.runForPlan(ctx, d, anchor, .{
            .system_slug = slug_and_id.slug,
            .dry_run = false,
            .sync = sync_text,
            .json = args.json,
        });
    }
}

/// anchorPlanFor walks derives-from links + parent_plan_id to find the
/// top-level plan reachable from `kind:id`. For plans the walk starts at
/// the plan itself; for tasks/questions/scenarios it follows a single
/// derives-from edge into the plan space first.
fn anchorPlanFor(
    d: anytype,
    allocator: std.mem.Allocator,
    kind: []const u8,
    id: i64,
) !i64 {
    _ = allocator;
    var current: i64 = id;
    if (!std.mem.eql(u8, kind, "plan")) {
        var stmt = try d.prepare(
            \\select to_id from entity_links
            \\where from_kind = ? and from_id = ? and to_kind = 'plan' and relationship = 'derives-from'
            \\order by id limit 1
        );
        defer stmt.finalize();
        try stmt.bind(&.{ .{ .text = kind }, .{ .int = id } });
        const step = try stmt.step();
        if (step == .done) return error.AnchorPlanNotFound;
        current = stmt.columnInt(0);
    }
    while (true) {
        var stmt = try d.prepare("select coalesce(parent_plan_id, 0) from plans where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = current }});
        const step = try stmt.step();
        if (step == .done) return error.AnchorPlanNotFound;
        const parent = stmt.columnInt(0);
        if (parent == 0) return current;
        current = parent;
    }
}

const KindID = struct { kind: []const u8, id: i64 };

fn parseKindIDRef(s: []const u8) !KindID {
    var i: usize = s.len;
    while (i > 0) : (i -= 1) {
        if (s[i - 1] == ':') {
            const kind = s[0 .. i - 1];
            const id_str = s[i..];
            if (kind.len == 0 or id_str.len == 0) break;
            const id = std.fmt.parseInt(i64, id_str, 10) catch break;
            return .{ .kind = kind, .id = id };
        }
    }
    return error.InvalidInput;
}

const ToRef = struct { slug: []const u8, external_id: []const u8 };

/// Split `<slug>:<external-id>` on the FIRST colon. external-id may itself
/// contain colons (e.g. `owner/repo#42` for github is colon-free, but a
/// future hierarchy notation could include them).
fn parseToRef(s: []const u8) !ToRef {
    const idx = std.mem.indexOfScalar(u8, s, ':') orelse return error.InvalidInput;
    if (idx == 0 or idx + 1 >= s.len) return error.InvalidInput;
    return .{ .slug = s[0..idx], .external_id = s[idx + 1 ..] };
}
