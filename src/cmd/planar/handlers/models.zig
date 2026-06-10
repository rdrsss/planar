//! handlers/models.zig — `planar models` (plan 540/543 provider + model discovery)
//!
//! Two read-oriented subverbs, neither of which touches the database:
//!
//!   list    Probe installed provider CLIs (claude, codex) and print their
//!           curated model catalogs + the default role→tier→model routing.
//!   refresh Same probe, plus write the result to
//!           ${PLANAR_HOME:-~/.planar}/models/catalog.json as a cache.
//!
//! Discovery genuinely invokes `<bin> --version` per provider (instant +
//! auth-free); the model list itself comes from the curated catalog in
//! `engine.models` because the provider CLIs do not enumerate models.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("runtime");
const output = @import("../output.zig");
const exit = @import("../exit.zig");
const config_path = @import("config/path.zig");

pub const verb: cli.Cmd = .{
    .name = "models",
    .desc = "Discover installed provider CLIs and their model catalogs.",
    .long_desc = "Probe the supported provider CLIs (claude, codex) for\n  installed-state + version, and report their curated model\n  catalogs and the default role→tier→model routing.\n\n  The provider CLIs do not expose a machine-readable model list,\n  so the per-vendor model catalog is curated in-repo; discovery\n  confirms which CLIs are callable on this machine.\n\n  Subcommands:\n    list     Probe + print (read-only).\n    refresh  Probe + print, and write the cache to\n             ${PLANAR_HOME:-~/.planar}/models/catalog.json.",
    .cmds = &.{
        .{
            .name = "list",
            .desc = "Probe providers and print catalogs + default routing.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleList),
        },
        .{
            .name = "refresh",
            .desc = "Probe providers and write the catalog cache under ~/.planar/models/.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleRefresh),
        },
        .{
            .name = "routing",
            .desc = "Print the effective role→vendor/tier/model routing (resolved via config).",
            .long_desc = "Resolve each role through the shared model resolver against the\n  effective config ([models.<vendor>] tiers, [roles] role→tier,\n  [role_vendors] role→vendor, [defaults].vendor) and print the\n  result with provenance. This is the routing planar-execute\n  consumes to pick a worker model per role.",
            .flags = &.{
                .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleRouting),
        },
        .{
            .name = "apply",
            .desc = "Scaffold the [models]/[roles] config block into the config file.",
            .long_desc = "Write the curated model tier maps + role routing into the\n  resolved config file as an editable starting point. Skips when a\n  [models] section is already present unless --force appends anyway.",
            .flags = &.{
                .{ .long = "--force", .kind = .bool, .default = .{ .bool = false } },
            },
            .run = cli.handler(handleApply),
        },
    },
};

fn handleList(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "list" }, args_ptr);
    const ctx = runtime.current();
    const report = engine.models.discover(ctx.allocator, ctx.io) catch |e| exit.die(
        ctx,
        e,
        "model discovery failed: {s}",
        .{@errorName(e)},
    );
    try output.emit(ctx, engine.models, report, .{ .json = args.json });
}

fn handleRefresh(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "refresh" }, args_ptr);
    const ctx = runtime.current();

    const report = engine.models.discover(ctx.allocator, ctx.io) catch |e| exit.die(
        ctx,
        e,
        "model discovery failed: {s}",
        .{@errorName(e)},
    );

    const home = engine.identity.workspace.planarHome(ctx.allocator, ctx.environ) catch |e| exit.die(
        ctx,
        e,
        "resolving PLANAR_HOME: {s}",
        .{@errorName(e)},
    );
    const path = engine.models.writeCache(ctx.allocator, ctx.io, home, report) catch |e| exit.die(
        ctx,
        e,
        "writing model cache: {s}",
        .{@errorName(e)},
    );

    try output.emit(ctx, engine.models, report, .{ .json = args.json });
    // Provenance note on stderr so JSON stdout stays clean for scripts.
    try ctx.stderr.print("wrote model cache: {s}\n", .{path});
}

fn handleRouting(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "routing" }, args_ptr);
    const ctx = runtime.current();

    // Resolve the effective config (file-over-default), then build the routing.
    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const file_content: ?[]u8 = std.Io.Dir.cwd().readFileAlloc(ctx.io, path, ctx.allocator, .unlimited) catch |e| switch (e) {
        error.FileNotFound => null,
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer if (file_content) |fc| ctx.allocator.free(fc);

    var resolved = engine.config.resolve(ctx.allocator, file_content, ctx.environ, null) catch |e|
        exit.die(ctx, e, "resolving configuration: {s}", .{@errorName(e)});
    defer resolved.deinit(ctx.allocator);

    const rows = engine.models.buildRouting(ctx.allocator, &resolved.effective) catch |e|
        exit.die(ctx, e, "building model routing: {s}", .{@errorName(e)});
    defer ctx.allocator.free(rows);

    if (args.json) {
        try std.json.Stringify.value(rows, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        for (rows) |r| {
            try ctx.stdout.print(
                "  {s: <10} → {s: <6} {s: <22} ({s}) [{s}]\n",
                .{ r.role, r.vendor, r.model, r.tier, r.source.label() },
            );
        }
    }
}

fn handleApply(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "models", "apply" }, args_ptr);
    const ctx = runtime.current();

    const path = config_path.resolveConfigPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving config path: {s}", .{@errorName(e)});
    defer ctx.allocator.free(path);

    const existing: []u8 = std.Io.Dir.cwd().readFileAlloc(ctx.io, path, ctx.allocator, .unlimited) catch |e| switch (e) {
        error.FileNotFound => try ctx.allocator.dupe(u8, ""),
        else => exit.die(ctx, e, "reading config file: {s}", .{@errorName(e)}),
    };
    defer ctx.allocator.free(existing);

    const already = std.mem.indexOf(u8, existing, "[models.") != null or std.mem.indexOf(u8, existing, "[models]") != null;
    if (already and !args.force) {
        try ctx.stdout.print("models config already present in {s} (use --force to append)\n", .{path});
        return;
    }

    const block = engine.models.renderConfigBlock(ctx.allocator) catch |e|
        exit.die(ctx, e, "rendering model config: {s}", .{@errorName(e)});
    defer ctx.allocator.free(block);

    // Append the block to existing content (with a separating newline).
    var out: std.ArrayList(u8) = .empty;
    defer out.deinit(ctx.allocator);
    out.appendSlice(ctx.allocator, existing) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});
    if (existing.len > 0 and existing[existing.len - 1] != '\n') out.append(ctx.allocator, '\n') catch {};
    if (existing.len > 0) out.append(ctx.allocator, '\n') catch {};
    out.appendSlice(ctx.allocator, block) catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    std.Io.Dir.cwd().writeFile(ctx.io, .{ .sub_path = path, .data = out.items }) catch |e|
        exit.die(ctx, e, "writing config file: {s}", .{@errorName(e)});
    try ctx.stdout.print("wrote model routing config to {s}\n", .{path});
}
