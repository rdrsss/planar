//! handlers/search.zig — `planar search`
//!
//! Full-text search across plans, tasks, questions, scenarios, decisions,
//! and artifacts. Backed by the FTS5 indexes created in migration 00011.
//!
//! Text output (default):
//!   <kind>:<id> [<slug>] [<status>] — <title>
//!     <snippet>
//!   "(no results)" when the result set is empty.
//!
//! JSON output (--json):
//!   A JSON array of hit objects, one per line.
//!   Empty result set: "[]" (not null, not omitted).
//!
//! Exit code 0 always — an empty result is not an error.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const scope_mod = @import("../scope.zig");

pub const verb: cli.Cmd = .{
    .name = "search",
    .desc = "Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts.",
    .long_desc = "Run a full-text search across every searchable entity kind.\n\n  Queries are passed to SQLite's FTS5 MATCH operator directly.\n  Multi-word queries are AND'd unless the operator is given\n  explicitly (OR, NOT, NEAR, \"phrase\"). Tokens are unicode61-folded\n  (case-insensitive, diacritic-stripped).",
    .flags = &.{
        .{ .long = "--kind", .kind = .string, .desc = "Restrict to a single kind" },
        .{ .long = "--status", .kind = .string, .desc = "Restrict to a single status" },
        .{ .long = "--scope", .kind = .string, .desc = "Restrict to a scope slug" },
        .{ .long = "--plan", .kind = .int, .desc = "Restrict to a plan id" },
        .{ .long = "--limit", .kind = .int, .default = .{ .int = 50 }, .desc = "Max results" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "query", .kind = .string, .required = true, .desc = "FTS5 query string" },
    },
    .run = cli.handler(handle),
};

/// HitJSON is the stable wire format for a single search hit in JSON mode.
/// Field names mirror Go's searchHitJSON for pipeline compatibility.
const HitJSON = struct {
    kind: []const u8,
    id: i64,
    slug: []const u8,
    title: []const u8,
    status: []const u8,
    snippet: []const u8,
    rank: f64,
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"search"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

    // Build the filter. The CLI currently delivers a single string for
    // --kind and --status (the flag scaffolding is not repeated/array).
    // Map single-string to a one-element slice; empty string means "all".
    var kinds_buf: [1][]const u8 = undefined;
    var kinds_slice: []const []const u8 = &.{};
    if (args.kind) |k| {
        // Validate the kind against the engine's known-kinds list.
        var found = false;
        for (engine.search.valid_kinds) |vk| {
            if (std.mem.eql(u8, k, vk)) {
                found = true;
                break;
            }
        }
        if (!found) exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{k});
        kinds_buf[0] = k;
        kinds_slice = kinds_buf[0..1];
    }

    var statuses_buf: [1][]const u8 = undefined;
    var statuses_slice: []const []const u8 = &.{};
    if (args.status) |s| {
        statuses_buf[0] = s;
        statuses_slice = statuses_buf[0..1];
    }

    var read_scope_slugs: []const []const u8 = &.{};
    defer if (read_scope_slugs.len > 0) scope_mod.deinitReadScopeFilterSlugs(ctx.allocator, read_scope_slugs);
    if (args.scope == null) {
        const cwd = try scope_mod.operatorCwd(ctx.allocator, ctx.io);
        defer ctx.allocator.free(cwd);
        const read_scopes = try scope_mod.resolveForReadSet(ctx, cwd, null);
        defer ctx.allocator.free(read_scopes);
        if (read_scopes.len == 0) {
            exit.die(
                ctx,
                error.NoReadScope,
                "cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global",
                .{},
            );
        }
        read_scope_slugs = try scope_mod.readScopeFilterSlugs(ctx, read_scopes);
    }

    // --plan: 0 means "not provided" (cli delivers default int as 0).
    const plan_id_opt: ?i64 = if (args.plan != 0) args.plan else null;

    const limit: i64 = args.limit;

    const hits = if (args.scope) |s| blk: {
        const scope_opt: ?[]const u8 = if (s.len > 0) s else null;
        break :blk engine.search.query(d, ctx.allocator, args.query, .{
            .kinds = kinds_slice,
            .statuses = statuses_slice,
            .scope = scope_opt,
            .plan_id = plan_id_opt,
            .limit = limit,
        }) catch |e| switch (e) {
            error.UnsupportedScope => exit.die(ctx, error.InvalidInput, "unsupported scope form", .{}),
            error.SlugNotFound => exit.die(ctx, error.NotFound, "scope slug not found", .{}),
            error.UnknownKind => exit.die(ctx, error.InvalidInput, "unknown kind", .{}),
            error.InvalidQuery => exit.die(ctx, error.InvalidInput, "invalid FTS5 query syntax", .{}),
            else => exit.die(ctx, e, "search failed: {s}", .{@errorName(e)}),
        };
    } else blk: {
        break :blk searchReadScopes(ctx, d, args.query, kinds_slice, statuses_slice, plan_id_opt, limit, read_scope_slugs) catch |e| switch (e) {
            error.UnsupportedScope => exit.die(ctx, error.InvalidInput, "unsupported scope form", .{}),
            error.SlugNotFound => exit.die(ctx, error.NotFound, "scope slug not found", .{}),
            error.UnknownKind => exit.die(ctx, error.InvalidInput, "unknown kind", .{}),
            error.InvalidQuery => exit.die(ctx, error.InvalidInput, "invalid FTS5 query syntax", .{}),
            else => exit.die(ctx, e, "search failed: {s}", .{@errorName(e)}),
        };
    };
    defer engine.search.deinitHits(hits, ctx.allocator);

    if (args.json) {
        // JSON output: emit an array. Empty result → "[]".
        // Build a slice of HitJSON values, then stringify the whole array.
        var json_hits = ctx.allocator.alloc(HitJSON, hits.len) catch
            exit.die(ctx, error.OutOfMemory, "out of memory building JSON hits", .{});
        defer ctx.allocator.free(json_hits);
        for (hits, 0..) |h, i| {
            json_hits[i] = .{
                .kind = h.kind,
                .id = h.id,
                .slug = h.slug,
                .title = h.title,
                .status = h.status,
                .snippet = h.snippet,
                .rank = h.rank,
            };
        }
        try std.json.Stringify.value(json_hits, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }

    // Text output.
    try engine.search.renderListText(hits, ctx.stdout);
}

fn searchReadScopes(
    ctx: *const runtime.Ctx,
    d: anytype,
    query: []const u8,
    kinds: []const []const u8,
    statuses: []const []const u8,
    plan_id: ?i64,
    limit: i64,
    scopes: []const []const u8,
) ![]engine.search.Hit {
    var all: std.ArrayList(engine.search.Hit) = .empty;
    errdefer {
        for (all.items) |hit| engine.search.deinitHit(hit, ctx.allocator);
        all.deinit(ctx.allocator);
    }

    for (scopes) |scope| {
        const hits = try engine.search.query(d, ctx.allocator, query, .{
            .kinds = kinds,
            .statuses = statuses,
            .scope = scope,
            .plan_id = plan_id,
            .limit = limit,
        });
        defer ctx.allocator.free(hits);
        try all.appendSlice(ctx.allocator, hits);
    }

    const merged = try all.toOwnedSlice(ctx.allocator);
    std.mem.sort(engine.search.Hit, merged, {}, hitLessThan);
    const max: usize = if (limit > 0) @intCast(limit) else @intCast(engine.search.default_limit);
    const visible_len = @min(merged.len, max);
    const visible = try ctx.allocator.alloc(engine.search.Hit, visible_len);
    @memcpy(visible, merged[0..visible_len]);
    for (merged[visible_len..]) |hit| engine.search.deinitHit(hit, ctx.allocator);
    ctx.allocator.free(merged);
    return visible;
}

fn hitLessThan(_: void, a: engine.search.Hit, b: engine.search.Hit) bool {
    if (a.rank != b.rank) return a.rank > b.rank;
    const kind_order = std.mem.order(u8, a.kind, b.kind);
    if (kind_order != .eq) return kind_order == .lt;
    return a.id < b.id;
}
