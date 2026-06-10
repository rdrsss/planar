//! handlers/tree.zig — `planar tree`
//!
//! Renders plans, tasks, artifacts, decisions, scenarios, and questions as a
//! hierarchical tree rooted at the current scope (or filtered by --scope /
//! --all-scopes).
//!
//! Text output (default):
//!   <scope-label>
//!   ├── plan:<id> [<status>]  <title>
//!   │   ├── task:<id>  <title>  [<status>, pri:<priority>]
//!   │   └── question:<id>  <title>  [<status>]
//!   …
//!
//! JSON output (--json):
//!   engine.tree.Node slice serialised via std.json.Stringify.
//!   Field names are snake_case (artifact_kind, scope_kind, etc.) matching
//!   Go's render_json.go `jsonNode` wire shape. Single root → JSON object;
//!   multiple roots → JSON array.
//!
//! Exit code 0 always — an empty tree is not an error.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const scope_mod = @import("../scope.zig");

pub const verb: cli.Cmd = .{
    .name = "tree",
    .desc = "Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree.",
    .long_desc = "Render a hierarchical view of Planar entities for one or all\n  scopes.\n\n  Walks plans (via parent_plan_id), tasks (via plan_id and\n  parent_task_id), and entity_links(derives-from) to gather\n  artifacts, decisions, scenarios, and questions linked to each plan.",
    .flags = &.{
        .{ .long = "--scope", .kind = .string, .desc = "Limit to a single scope slug" },
        .{ .long = "--all-scopes", .kind = .bool, .default = .{ .bool = false }, .desc = "Include every scope" },
        .{ .long = "--depth", .kind = .int, .default = .{ .int = -1 }, .desc = "Max tree depth (-1 = unbounded)" },
        .{ .long = "--kind", .kind = .string, .desc = "Restrict to a single kind" },
        .{ .long = "--status", .kind = .string, .desc = "Restrict to a single status" },
        .{ .long = "--sort", .kind = .string, .desc = "Sort key" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"tree"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDb() catch |e|
        exit.die(ctx, e, "opening database: {s}", .{@errorName(e)});

    // --kind: single string → one-element slice (or empty = all kinds).
    var kinds_buf: [1][]const u8 = undefined;
    var kinds_slice: []const []const u8 = &.{};
    if (args.kind) |k| {
        var found = false;
        for (engine.tree.valid_kinds) |vk| {
            if (std.mem.eql(u8, k, vk)) {
                found = true;
                break;
            }
        }
        if (!found) exit.die(ctx, error.InvalidInput, "unknown kind '{s}'", .{k});
        kinds_buf[0] = k;
        kinds_slice = kinds_buf[0..1];
    }

    // --status: single string → one-element slice (or empty = any status).
    var statuses_buf: [1][]const u8 = undefined;
    var statuses_slice: []const []const u8 = &.{};
    if (args.status) |s| {
        statuses_buf[0] = s;
        statuses_slice = statuses_buf[0..1];
    }

    // --depth default = -1 (unbounded).
    const max_depth: i64 = args.depth;

    const base_filter = engine.tree.Filter{
        .all_scopes = args.all_scopes,
        .max_depth = max_depth,
        .kinds = kinds_slice,
        .statuses = statuses_slice,
        .sort = if (args.sort) |s| (if (s.len > 0) s else null) else null,
    };

    const roots = buildRoots(ctx, d, args.scope, args.all_scopes, base_filter) catch |e| switch (e) {
        error.UnsupportedScope => exit.die(ctx, error.InvalidInput, "unsupported scope form", .{}),
        error.SlugNotFound => exit.die(ctx, error.NotFound, "scope slug not found", .{}),
        error.UnknownKind => exit.die(ctx, error.InvalidInput, "unknown kind", .{}),
        error.NoReadScope => exit.die(ctx, e, "cwd is not inside any registered Planar scope; cd into a registered scope or pass --scope global", .{}),
        else => exit.die(ctx, e, "tree walk failed: {s}", .{@errorName(e)}),
    };
    defer engine.tree.deinitNodes(roots, ctx.allocator);

    if (args.json) {
        // JSON mode: serialize the root(s) directly. engine.tree.Node has
        // snake_case field names matching Go's render_json.go `jsonNode` shape.
        // Single root → JSON object; multiple roots → JSON array (mirrors Go).
        if (roots.len == 1) {
            try std.json.Stringify.value(roots[0], .{}, ctx.stdout);
        } else {
            try std.json.Stringify.value(roots, .{}, ctx.stdout);
        }
        try ctx.stdout.print("\n", .{});
        return;
    }

    // Text mode.
    try engine.tree.renderText(roots, ctx.stdout);
}

fn buildRoots(
    ctx: *const runtime.Ctx,
    d: anytype,
    scope_arg: ?[]const u8,
    all_scopes: bool,
    base_filter: engine.tree.Filter,
) ![]engine.tree.Node {
    if (all_scopes) return try engine.tree.build(d, ctx.allocator, base_filter);
    if (scope_arg) |scope| {
        var filter = base_filter;
        filter.scope = if (scope.len > 0) scope else null;
        return try engine.tree.build(d, ctx.allocator, filter);
    }

    const cwd = try scope_mod.operatorCwd(ctx.allocator, ctx.io);
    defer ctx.allocator.free(cwd);
    const read_scopes = try scope_mod.resolveForReadSet(ctx, cwd, null);
    defer ctx.allocator.free(read_scopes);
    if (read_scopes.len == 0) return error.NoReadScope;
    const scope_slugs = try scope_mod.readScopeFilterSlugs(ctx, read_scopes);
    defer scope_mod.deinitReadScopeFilterSlugs(ctx.allocator, scope_slugs);

    var roots: std.ArrayList(engine.tree.Node) = .empty;
    errdefer {
        for (roots.items) |root| engine.tree.deinitNode(root, ctx.allocator);
        roots.deinit(ctx.allocator);
    }
    for (scope_slugs) |scope| {
        var filter = base_filter;
        filter.scope = scope;
        const scope_roots = try engine.tree.build(d, ctx.allocator, filter);
        defer ctx.allocator.free(scope_roots);
        try roots.appendSlice(ctx.allocator, scope_roots);
    }
    return try roots.toOwnedSlice(ctx.allocator);
}
