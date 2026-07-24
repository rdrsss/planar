//! handlers/init.zig — `planar init`
//!
//! Mirrors `cmd/planar/internal/identity/init.go`:
//!   1. `runtime.ensureDb()` opens the DB, applies migrations, runs the
//!      schema-version-too-new guard. That's the "init" half.
//!   2. Unless `--skip-project` is passed, register cwd as a project via
//!      `engine.init.registerCwd` (INSERT OR IGNORE — idempotent).
//!
//! Idempotent end-to-end: a second `planar init` from the same cwd
//! against the same DB is a no-op apart from the migration check.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const exit = @import("../exit.zig");
const scope_mod = @import("../scope.zig");

pub const verb: cli.Cmd = .{
    .name = "init",
    .desc = "Initialize the Planar database and register the current directory as a project.",
    .flags = &.{
        .{ .long = "--name", .kind = .string, .desc = "Project name (defaults to repo dir)" },
        .{ .long = "--skip-project", .kind = .bool, .default = .{ .bool = false }, .desc = "Only init the DB; skip project registration" },
        .{ .long = "--allow-no-repo", .kind = .bool, .default = .{ .bool = false }, .desc = "Allow initialization outside a git repo" },
        .{ .long = "--force", .kind = .bool, .default = .{ .bool = false }, .desc = "Overwrite an existing project registration" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false }, .desc = "Emit machine-readable output" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"init"}, args_ptr);
    const ctx = runtime.current();

    // Step 1: open + migrate + schema-version guard.
    const d = try runtime.ensureDb();

    // Read the schema version back so we can print it.
    const schema_version: u32 = if (d.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    )) |v| @intCast(v) else |_| 0;

    if (args.skip_project) {
        try emit(ctx, .{ .db = ctx.db_path, .schema_version = schema_version }, args.json);
        return;
    }

    // Step 2: resolve cwd and register the project.
    //
    // Match cwd-derived scope resolution: preserve the shell-provided
    // PWD spelling only when it resolves to the actual process cwd.
    // This keeps /var-style symlink spelling stable without letting
    // stale parent PWD values leak into child processes spawned with
    // an explicit cwd.
    const cwd = scope_mod.operatorCwd(ctx.allocator, ctx.io) catch |e|
        exit.die(ctx, e, "getting working directory: {s}", .{@errorName(e)});
    defer ctx.allocator.free(cwd);

    // Best-effort git remote read (handler-layer IO).
    const remote_owned: ?[]u8 = gitRemoteOrigin(ctx, cwd);
    defer if (remote_owned) |b| ctx.allocator.free(b);
    const remote: ?[]const u8 = if (remote_owned) |b|
        std.mem.trim(u8, b, " \t\r\n")
    else
        null;

    const p = engine.init.registerCwd(d, ctx.allocator, .{
        .cwd = cwd,
        .name = args.name,
        .git_remote = remote,
        .force = args.force,
    }) catch |e| exit.die(ctx, e, "registering project: {s}", .{@errorName(e)});
    defer engine.identity.project.deinit(p, ctx.allocator);

    try emit(ctx, .{
        .db = ctx.db_path,
        .schema_version = schema_version,
        .project_id = p.id,
        .project_slug = p.slug,
        .project_name = p.name,
        .root_path = p.root_path,
        .git_remote = p.git_remote,
    }, args.json);
}

/// InitResult captures the data emitted by `planar init` — used by both
/// text and JSON emitters so the two surfaces share one schema. The JSON
/// emitter routes every string field through `std.json.Stringify.value`
/// (proper escaping for embedded quotes, backslashes, control chars),
/// replacing the previous hand-rolled `{{"…":"{s}"}}` template that would
/// produce invalid JSON when a project name contained a quote.
const InitResult = struct {
    db: []const u8,
    schema_version: u32,
    project_id: ?i64 = null,
    project_slug: ?[]const u8 = null,
    project_name: ?[]const u8 = null,
    root_path: ?[]const u8 = null,
    git_remote: ?[]const u8 = null,
};

fn emit(ctx: *const runtime.Ctx, r: InitResult, json: bool) !void {
    if (json) {
        try emitJson(ctx, r);
        return;
    }
    try ctx.stdout.print("planar initialized\n", .{});
    try ctx.stdout.print("  db:      {s}\n", .{r.db});
    try ctx.stdout.print("  schema:  {d}\n", .{r.schema_version});
    if (r.project_slug) |slug| {
        try ctx.stdout.print("  project: {s} (id: {d})\n", .{ slug, r.project_id.? });
        if (r.root_path) |root| {
            try ctx.stdout.print("  next:    `planar assoc create project:{s} --kind project`\n", .{slug});
            try ctx.stdout.print("           `planar assoc add project:{s} {s}`\n", .{ slug, root });
        }
    }
}

fn emitJson(ctx: *const runtime.Ctx, r: InitResult) !void {
    const w = ctx.stdout;
    try w.print("{{\"ok\":true,\"db\":", .{});
    try std.json.Stringify.value(r.db, .{}, w);
    try w.print(",\"schema_version\":{d}", .{r.schema_version});
    if (r.project_id) |id| try w.print(",\"project_id\":{d}", .{id});
    if (r.project_slug) |slug| {
        try w.print(",\"project_slug\":", .{});
        try std.json.Stringify.value(slug, .{}, w);
    }
    if (r.project_name) |name| {
        try w.print(",\"project_name\":", .{});
        try std.json.Stringify.value(name, .{}, w);
    }
    if (r.root_path) |rp| {
        try w.print(",\"root_path\":", .{});
        try std.json.Stringify.value(rp, .{}, w);
    }
    if (r.git_remote) |g| {
        try w.print(",\"git_remote\":", .{});
        try std.json.Stringify.value(g, .{}, w);
    }
    try w.print("}}\n", .{});
}

/// gitRemoteOrigin — best-effort `git -C cwd remote get-url origin`.
/// Returns null when git is missing, the cwd is not a repo, or no
/// origin is configured. Caller owns the returned slice.
fn gitRemoteOrigin(ctx: *const runtime.Ctx, cwd: []const u8) ?[]u8 {
    const argv = [_][]const u8{ "git", "-C", cwd, "remote", "get-url", "origin" };
    const result = std.process.run(ctx.allocator, ctx.io, .{ .argv = &argv }) catch return null;
    defer ctx.allocator.free(result.stderr);
    switch (result.term) {
        .exited => |code| {
            if (code != 0) {
                ctx.allocator.free(result.stdout);
                return null;
            }
        },
        else => {
            ctx.allocator.free(result.stdout);
            return null;
        },
    }
    if (std.mem.trim(u8, result.stdout, " \t\r\n").len == 0) {
        ctx.allocator.free(result.stdout);
        return null;
    }
    return result.stdout;
}
