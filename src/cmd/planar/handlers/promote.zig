//! handlers/promote.zig — `planar promote <kind:id> --to <assoc-slug>`
//!
//! Parses the kind:id ref, reads the current scope before promotion (for
//! output), calls engine.promotion.promote, then reads the new scope and
//! emits the result. Mirrors Go's identity.runPromote shape.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const db = @import("db");

pub const verb: cli.Cmd = .{
    .name = "promote",
    .desc = "Promote an entity to an association scope.",
    .long_desc = "Promote an entity from its current scope to a named association.\n\n  Valid entity kinds: plan, task, question, test_scenario (alias:\n  scenario), artifact, decision.\n\n  Examples:\n    planar promote task:42 --to org:acme\n    planar promote plan:7 --to project:planar",
    .flags = &.{
        .{ .long = "--to", .kind = .string, .required = true, .desc = "Target association slug" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "ref", .kind = .string, .required = true, .desc = "Entity ref (kind:id)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"promote"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Parse kind:id positional.
    const parsed = engine.entitylink.parseRef(args.ref) catch |e| switch (e) {
        error.InvalidRef => exit.die(ctx, error.InvalidInput, "invalid ref '{s}': expected kind:id", .{args.ref}),
        else => return e,
    };
    const kind_text = switch (parsed) {
        .id => |r| r.kind.toText(),
        .slug => exit.die(ctx, error.InvalidInput, "promote requires a numeric id (got slug '{s}')", .{args.ref}),
    };
    const entity_id = switch (parsed) {
        .id => |r| r.id,
        .slug => unreachable,
    };

    // Read current scope before promote (for output).
    const prev_scope = readEntityScope(d, ctx.allocator, kind_text, entity_id) catch |e|
        exit.die(ctx, e, "reading entity scope: {s}", .{@errorName(e)});
    defer ctx.allocator.free(prev_scope.scope_kind);

    // Call the promotion engine.
    engine.promotion.promote(d, ctx.allocator, .{
        .kind = kind_text,
        .id = entity_id,
        .to_scope = args.to,
    }) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no {s} with id {d}", .{ kind_text, entity_id }),
        error.SlugNotFound => exit.die(ctx, e, "no association with slug '{s}'", .{args.to}),
        error.ScopeUnchanged => exit.die(ctx, e, "{s}:{d} is already at scope '{s}'", .{ kind_text, entity_id, args.to }),
        error.UnsupportedScope => exit.die(ctx, e, "repo: scopes are not supported", .{}),
        error.InvalidScope => exit.die(ctx, e, "unsupported entity kind '{s}'", .{kind_text}),
        else => exit.die(ctx, e, "promote: {s}", .{@errorName(e)}),
    };

    // Read new scope after promote.
    const curr_scope = readEntityScope(d, ctx.allocator, kind_text, entity_id) catch |e|
        exit.die(ctx, e, "reading entity scope after promote: {s}", .{@errorName(e)});
    defer ctx.allocator.free(curr_scope.scope_kind);

    if (args.json) {
        try emitScopeChangeJSON(ctx, kind_text, entity_id, curr_scope, prev_scope);
    } else {
        const prev_display = if (prev_scope.scope_id) |sid|
            try std.fmt.allocPrint(ctx.allocator, "{s}:{d}", .{ prev_scope.scope_kind, sid })
        else
            try ctx.allocator.dupe(u8, prev_scope.scope_kind);
        defer ctx.allocator.free(prev_display);

        try ctx.stdout.print("{s}:{d} promoted to association {s}  (was: {s})\n", .{
            kind_text, entity_id, args.to, prev_display,
        });
    }
}

// ---- helpers ----------------------------------------------------------------

const ScopeInfo = struct {
    scope_kind: []const u8, // heap-allocated; caller must free
    scope_id: ?i64,
};

fn emitScopeChangeJSON(
    ctx: *const runtime.Ctx,
    kind: []const u8,
    id: i64,
    curr: ScopeInfo,
    prev: ScopeInfo,
) !void {
    // Manual JSON to handle optional scope_id correctly.
    try ctx.stdout.print("{{\"ok\":true,\"kind\":", .{});
    try std.json.Stringify.encodeJsonString(kind, .{}, ctx.stdout);
    try ctx.stdout.print(",\"id\":{d},\"scope_kind\":", .{id});
    try std.json.Stringify.encodeJsonString(curr.scope_kind, .{}, ctx.stdout);
    try ctx.stdout.print(",\"scope_id\":", .{});
    if (curr.scope_id) |sid| {
        try ctx.stdout.print("{d}", .{sid});
    } else {
        try ctx.stdout.print("null", .{});
    }
    try ctx.stdout.print(",\"previous_scope_kind\":", .{});
    try std.json.Stringify.encodeJsonString(prev.scope_kind, .{}, ctx.stdout);
    try ctx.stdout.print(",\"previous_scope_id\":", .{});
    if (prev.scope_id) |sid| {
        try ctx.stdout.print("{d}", .{sid});
    } else {
        try ctx.stdout.print("null", .{});
    }
    try ctx.stdout.print("}}\n", .{});
}

/// Read (scope_kind, scope_id) from the entity's table.
/// scope_kind is heap-allocated and must be freed by the caller.
fn readEntityScope(d: *db.sqlite.Db, allocator: std.mem.Allocator, kind: []const u8, id: i64) !ScopeInfo {
    const table = tableFor(kind) orelse return error.InvalidInput;

    const sql_raw = try std.fmt.allocPrint(
        allocator,
        "select scope_kind, scope_id from {s} where id = ?",
        .{table},
    );
    defer allocator.free(sql_raw);
    const sql = try allocator.dupeZ(u8, sql_raw);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return error.QueryFailed;

    return switch (stmt.step() catch return error.QueryFailed) {
        .done => error.NotFound,
        .row => .{
            .scope_kind = try stmt.columnTextAlloc(0, allocator),
            .scope_id = stmt.columnIntOpt(1),
        },
    };
}

fn tableFor(kind: []const u8) ?[]const u8 {
    if (std.mem.eql(u8, kind, "plan")) return "plans";
    if (std.mem.eql(u8, kind, "task")) return "tasks";
    if (std.mem.eql(u8, kind, "question")) return "questions";
    if (std.mem.eql(u8, kind, "test_scenario")) return "test_scenarios";
    if (std.mem.eql(u8, kind, "artifact")) return "artifacts";
    if (std.mem.eql(u8, kind, "decision")) return "decisions";
    return null;
}
