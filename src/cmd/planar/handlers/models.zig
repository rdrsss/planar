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
