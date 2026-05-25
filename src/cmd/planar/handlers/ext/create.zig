//! handlers/ext/create — `planar ext create <system-slug> --from <kind:id> [--type …] [--role …] [--sync …]`
//!
//! Render the local entity to a remote payload via the adapter, POST it, and
//! record the resulting external_links row. Mirrors Go's runExtCreate.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const db = @import("db");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const adapter_factory = @import("adapter_factory.zig");
const remote = @import("remote.zig");
const extsync = @import("engine").extsync;

const CreateJSON = struct {
    ok: bool,
    link_id: i64,
    external_id: []const u8,
    external_url: []const u8,
    sync_direction: []const u8,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "ext", "create" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const ref = parseKindIDRef(args.from) catch
        exit.die(ctx, error.InvalidInput, "invalid --from value '{s}'; expected kind:integer-id", .{args.from});

    const sys = engine.external.system.showBySlug(d, ctx.allocator, args.system_slug) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "external system '{s}' not found", .{args.system_slug}),
        else => exit.die(ctx, e, "ext create: lookup system: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(sys, ctx.allocator);

    var h = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, sys) catch |e| switch (e) {
        error.TokenEnvVarMissing => exit.die(ctx, error.InvalidInput, "token env var '{s}' is not set", .{sys.auth_ref}),
        error.UnsupportedAuthMethod => exit.die(ctx, error.InvalidInput, "oauth-stored auth not yet supported (matches Go: deferred)", .{}),
        error.GhCliNotFound => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh` binary not on PATH", .{}),
        error.GhCliFailed => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned non-zero (run `gh auth login`)", .{}),
        error.GhCliEmptyToken => exit.die(ctx, error.InvalidInput, "gh-cli auth: `gh auth token` returned empty output", .{}),
        error.UnsupportedSystemKind => exit.die(ctx, error.InvalidInput, "system kind '{s}' is not supported", .{sys.kind.toText()}),
        else => exit.die(ctx, e, "ext create: build adapter: {s}", .{@errorName(e)}),
    };
    defer {
        h.deinit();
        ctx.allocator.destroy(h);
    }

    const local = readLocalEntity(d, ctx.allocator, ref) catch |e|
        exit.die(ctx, e, "ext create: read local {s}:{d}: {s}", .{ ref.kind, ref.id, @errorName(e) });
    defer freeLocalEntity(local, ctx.allocator);

    const create_opts: extsync.common.CreateOptions = .{
        .issue_type = args.type,
        .project = sys.default_project,
    };

    const payload = renderPayload(h, ctx.allocator, local, create_opts) catch |e|
        exit.die(ctx, e, "ext create: render payload: {s}", .{@errorName(e)});
    defer ctx.allocator.free(payload);

    const created = remote.createRemote(h, ctx.allocator, sys, payload) catch |e|
        exit.die(ctx, e, "ext create: remote create: {s}", .{@errorName(e)});
    defer ctx.allocator.free(created.external_id);
    defer ctx.allocator.free(created.external_url);

    const role_text = args.role orelse "mirror";
    const sync_text = args.sync orelse "two-way";

    const role = engine.external.link.LinkRole.fromText(role_text) orelse
        exit.die(ctx, error.InvalidInput, "invalid --role '{s}'", .{role_text});
    const sync_dir = engine.external.link.SyncDirection.fromText(sync_text) orelse
        exit.die(ctx, error.InvalidInput, "invalid --sync '{s}'", .{sync_text});

    const entity_kind = engine.external.link.ExternalEntityKind.fromText(ref.kind) orelse
        exit.die(ctx, error.InvalidInput, "unsupported entity kind '{s}' for ext create", .{ref.kind});

    const link = engine.external.link.create(d, ctx.allocator, .{
        .entity_kind = entity_kind,
        .entity_id = ref.id,
        .system_id = sys.id,
        .external_id = created.external_id,
        .external_url = if (created.external_url.len > 0) created.external_url else null,
        .link_role = role,
        .sync_direction = sync_dir,
        .initial_status = .ok,
    }) catch |e| switch (e) {
        error.LinkExists => exit.die(ctx, error.AlreadyExists, "external link for {s}:{d} on {s} already exists", .{ ref.kind, ref.id, args.system_slug }),
        else => exit.die(ctx, e, "ext create: insert link: {s}", .{@errorName(e)}),
    };
    defer engine.external.link.deinit(link, ctx.allocator);

    if (args.json) {
        const out = CreateJSON{
            .ok = true,
            .link_id = link.id,
            .external_id = created.external_id,
            .external_url = created.external_url,
            .sync_direction = sync_text,
        };
        try std.json.Stringify.value(out, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print("created {s} on {s} for {s}:{d}\n", .{ created.external_id, args.system_slug, ref.kind, ref.id });
        try ctx.stdout.print("link id: {d}  ({s} {s})\n", .{ link.id, sync_text, role_text });
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

fn freeLocalEntity(local: extsync.common.LocalEntity, allocator: std.mem.Allocator) void {
    allocator.free(local.title);
    allocator.free(local.body);
    allocator.free(local.status);
}

fn readLocalEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    ref: KindID,
) !extsync.common.LocalEntity {
    const sql: [:0]const u8 = if (std.mem.eql(u8, ref.kind, "task"))
        "select coalesce(title,''), coalesce(body,''), coalesce(status,''), coalesce(priority,0) from tasks where id = ?"
    else if (std.mem.eql(u8, ref.kind, "plan"))
        "select coalesce(title,''), coalesce(summary,''), coalesce(status,''), 0 from plans where id = ?"
    else if (std.mem.eql(u8, ref.kind, "question"))
        "select coalesce(title,''), coalesce(body,''), coalesce(status,''), 0 from questions where id = ?"
    else if (std.mem.eql(u8, ref.kind, "artifact"))
        "select coalesce(title,''), coalesce(body,''), coalesce(status,''), 0 from artifacts where id = ?"
    else
        return error.InvalidInput;

    var stmt = try d.prepare(sql);
    defer stmt.finalize();
    try stmt.bind(&.{.{ .int = ref.id }});
    return switch (try stmt.step()) {
        .done => error.NotFound,
        .row => .{
            .kind = ref.kind,
            .id = ref.id,
            .title = try stmt.columnTextAlloc(0, allocator),
            .body = try stmt.columnTextAlloc(1, allocator),
            .status = try stmt.columnTextAlloc(2, allocator),
            .priority = stmt.columnInt(3),
        },
    };
}

fn renderPayload(
    h: *adapter_factory.Handle,
    allocator: std.mem.Allocator,
    local: extsync.common.LocalEntity,
    opts: extsync.common.CreateOptions,
) ![]const u8 {
    return switch (h.kind) {
        .jira => try h.jira_adapter.?.render(allocator, local, opts),
        .github => try h.github_adapter.?.render(allocator, local, opts),
    };
}
