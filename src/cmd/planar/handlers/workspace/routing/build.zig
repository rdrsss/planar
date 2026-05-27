const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workspace", "routing", "build" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const org = engine.identity.workspace.resolveOrg(d, ctx.allocator, args.workspace) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "no org associations registered; create one with `planar workspace init`", .{}),
        error.InvalidInput => exit.die(ctx, error.InvalidInput, "multiple org associations registered; pass the workspace slug or id explicitly", .{}),
        else => exit.die(ctx, e, "resolving workspace failed: {s}", .{@errorName(e)}),
    };
    defer engine.identity.workspace.deinitWorkspace(org, ctx.allocator);

    const layout = engine.identity.workspace.ensureLayout(ctx.allocator, ctx.io, ctx.environ, org.id) catch |e|
        exit.die(ctx, e, "ensuring workspace state directory failed: {s}", .{@errorName(e)});
    defer engine.identity.workspace.deinitLayout(layout, ctx.allocator);

    const rules_path = capabilityRulesPath(ctx.allocator, ctx.environ) catch |e|
        exit.die(ctx, e, "resolving capability rules path failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(rules_path);
    var rules = engine.workspace.routing.loadCapabilityRules(ctx.allocator, rules_path) catch |e|
        exit.die(ctx, e, "loading capability rules failed: {s}", .{@errorName(e)});
    if (rules.rules.len == 0) {
        engine.workspace.routing.deinitCapabilityRules(rules, ctx.allocator);
        rules = engine.workspace.routing.defaultCapabilityRules(ctx.allocator) catch |e|
            exit.die(ctx, e, "loading default capability rules failed: {s}", .{@errorName(e)});
    }
    defer engine.workspace.routing.deinitCapabilityRules(rules, ctx.allocator);

    const overrides_path = std.fs.path.join(ctx.allocator, &.{ layout.dir, "routing-table-overrides.json" }) catch |e|
        exit.die(ctx, e, "resolving routing overrides path failed: {s}", .{@errorName(e)});
    defer ctx.allocator.free(overrides_path);
    const overrides = engine.workspace.routing.loadOverrides(ctx.allocator, overrides_path) catch |e|
        exit.die(ctx, e, "loading routing overrides failed: {s}", .{@errorName(e)});
    defer engine.workspace.routing.deinitOverrides(overrides, ctx.allocator);

    var table = engine.workspace.routing.buildWithRules(d, ctx.allocator, org.id, rules) catch |e|
        exit.die(ctx, e, "building routing table failed: {s}", .{@errorName(e)});
    defer engine.workspace.routing.deinit(table, ctx.allocator);
    engine.workspace.routing.applyOverrides(&table, overrides, ctx.allocator) catch |e|
        exit.die(ctx, e, "applying routing overrides failed: {s}", .{@errorName(e)});

    engine.workspace.routing.write(layout.routing_table, table, ctx.allocator) catch |e|
        exit.die(ctx, e, "writing routing table failed: {s}", .{@errorName(e)});

    const enrich_enabled = false;
    if (args.enrich and !args.json) {
        try ctx.stdout.print("warning: --enrich is not yet implemented in Zig; skipping enrichment pass\n", .{});
    }

    if (args.json) {
        try std.json.Stringify.value(.{
            .path = layout.routing_table,
            .projects = table.projects.len,
            .dependency_edges = table.cross_repo.dependency_edges.len,
            .enrich_enabled = enrich_enabled,
            .enrich_misses = @as(i64, 0),
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
        return;
    }
    try ctx.stdout.print("built {s} ({d} projects, {d} cross-repo deps)\n", .{
        layout.routing_table,
        table.projects.len,
        table.cross_repo.dependency_edges.len,
    });
}

fn capabilityRulesPath(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
) ![]u8 {
    const home = try engine.identity.workspace.planarHome(allocator, environ);
    defer allocator.free(home);
    return try std.fs.path.join(allocator, &.{ home, "templates", "workspace-capabilities.toml" });
}
