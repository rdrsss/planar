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
const runtime = @import("../runtime.zig");
const exit = @import("../exit.zig");
const scope_mod = @import("../scope.zig");

pub const verb: cli.Cmd = .{
    .name = "tree",
    .desc = "Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree.",
    .flags = &.{
        .{ .long = "--scope",      .kind = .string, .desc = "Limit to a single scope slug" },
        .{ .long = "--all-scopes", .kind = .bool, .default = .{ .bool = false }, .desc = "Include every scope" },
        .{ .long = "--depth",      .kind = .int,  .default = .{ .int = -1 }, .desc = "Max tree depth (-1 = unbounded)" },
        .{ .long = "--kind",       .kind = .string, .desc = "Restrict to one kind (repeatable in Go; single here for now)" },
        .{ .long = "--status",     .kind = .string, .desc = "Restrict by status (repeatable in Go; single here for now)" },
        .{ .long = "--sort",       .kind = .string, .desc = "Sort key" },
        .{ .long = "--json",       .kind = .bool, .default = .{ .bool = false } },
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
            if (std.mem.eql(u8, k, vk)) { found = true; break; }
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

    // Scope: --scope wins; otherwise derive from cwd (matches the Go binary's
    // cli.ResolveReadScope invariant from plan 153 M4 — read verbs honor cwd
    // when --scope isn't given). --all-scopes bypasses resolution entirely.
    var scope_opt: ?[]const u8 = null;
    if (!args.all_scopes) {
        const resolution = scope_mod.resolve(ctx, args.scope) catch |e|
            exit.die(ctx, e, "resolving scope: {s}", .{@errorName(e)});
        scope_opt = resolution.scope;
    }

    // --depth default = -1 (unbounded).
    const max_depth: i64 = args.depth;

    const filter = engine.tree.Filter{
        .scope      = scope_opt,
        .all_scopes = args.all_scopes,
        .max_depth  = max_depth,
        .kinds      = kinds_slice,
        .statuses   = statuses_slice,
        .sort       = if (args.sort) |s| (if (s.len > 0) s else null) else null,
    };

    const roots = engine.tree.build(d, ctx.allocator, filter) catch |e| switch (e) {
        error.UnsupportedScope => exit.die(ctx, error.InvalidInput, "unsupported scope form", .{}),
        error.SlugNotFound     => exit.die(ctx, error.NotFound,     "scope slug not found", .{}),
        error.UnknownKind      => exit.die(ctx, error.InvalidInput, "unknown kind", .{}),
        else                   => exit.die(ctx, e, "tree walk failed: {s}", .{@errorName(e)}),
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
