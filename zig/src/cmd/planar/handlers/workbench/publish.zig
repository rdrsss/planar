//! Publish a manifest-ordered workbench feature as one external mirror.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const common = @import("common.zig");
const adapter_factory = @import("../ext/adapter_factory.zig");
const remote = @import("../ext/remote.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workbench", "publish" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const resolved = common.resolvePlanArg(d, ctx.allocator, args.plan) catch |e| switch (e) {
        error.InvalidInput => exit.die(ctx, e, "invalid plan '{s}'", .{args.plan}),
        error.NotFound => exit.die(ctx, e, "plan not found: {s}", .{args.plan}),
        else => exit.die(ctx, e, "resolving plan '{s}' failed: {s}", .{ args.plan, @errorName(e) }),
    };
    defer resolved.deinit(ctx.allocator);
    const plan = engine.planning.plan.show(d, ctx.allocator, resolved.id) catch |e|
        exit.die(ctx, e, "workbench publish: loading plan: {s}", .{@errorName(e)});
    defer engine.planning.plan.deinit(plan, ctx.allocator);

    const system = engine.external.system.showBySlug(d, ctx.allocator, args.system) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "external system '{s}' not found", .{args.system}),
        else => exit.die(ctx, e, "workbench publish: loading system: {s}", .{@errorName(e)}),
    };
    defer engine.external.system.deinit(system, ctx.allocator);

    const existing = engine.external.link.list(d, ctx.allocator, .{
        .entity_kind = .plan,
        .entity_id = plan.id,
        .system_id = system.id,
    }) catch |e| exit.die(ctx, e, "workbench publish: checking existing links: {s}", .{@errorName(e)});
    defer engine.external.link.deinitMany(existing, ctx.allocator);
    if (existing.len > 0) {
        exit.die(ctx, error.AlreadyExists, "plan {d} already has an external link on {s}", .{ plan.id, args.system });
    }

    const sync_result = engine.workbench.sync.push(d, ctx.allocator, plan.id, .failures, false) catch |e|
        exit.die(ctx, e, "workbench publish: rendering workbench: {s}", .{@errorName(e)});
    defer engine.workbench.sync.deinitResult(ctx.allocator, sync_result);
    if (sync_result.conflicts > 0) {
        exit.die(ctx, error.Conflict, "{d} workbench conflict(s) must be resolved before publication", .{sync_result.conflicts});
    }

    const root = common.resolveAndEnsureWorkbenchRoot(ctx.allocator, ctx.environ, ctx.io) catch |e|
        exit.die(ctx, e, "workbench publish: resolving workbench root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);
    const bundle = buildBundle(ctx, d, root, plan.id) catch |e|
        exit.die(ctx, e, "workbench publish: reading rendered files: {s}", .{@errorName(e)});
    defer ctx.allocator.free(bundle.body);

    var adapter = adapter_factory.build(ctx.allocator, ctx.io, &ctx.environ, system) catch |e|
        exit.die(ctx, e, "workbench publish: building adapter: {s}", .{@errorName(e)});
    defer {
        adapter.deinit();
        ctx.allocator.destroy(adapter);
    }
    const payload = switch (adapter.kind) {
        .jira => adapter.jira_adapter.?.render(ctx.allocator, .{
            .kind = "plan",
            .id = plan.id,
            .title = plan.title,
            .body = bundle.body,
            .status = @tagName(plan.status),
        }, .{ .project = system.default_project }),
        .github => adapter.github_adapter.?.render(ctx.allocator, .{
            .kind = "plan",
            .id = plan.id,
            .title = plan.title,
            .body = bundle.body,
            .status = @tagName(plan.status),
        }, .{ .project = system.default_project }),
    } catch |e| exit.die(ctx, e, "workbench publish: rendering external payload: {s}", .{@errorName(e)});
    defer ctx.allocator.free(payload);

    const created = remote.createRemote(adapter, ctx.allocator, system, payload) catch |e|
        exit.die(ctx, e, "workbench publish: creating external mirror: {s}", .{@errorName(e)});
    defer ctx.allocator.free(created.external_id);
    defer ctx.allocator.free(created.external_url);
    const link_id = engine.extsync.parent_issue.recordLink(
        d,
        "plan",
        plan.id,
        system.id,
        created.external_id,
        created.external_url,
        .@"two-way",
    ) catch |e| exit.die(ctx, e, "workbench publish: recording external link: {s}", .{@errorName(e)});

    if (args.json) {
        try std.json.Stringify.value(.{
            .ok = true,
            .plan_id = plan.id,
            .system = system.slug,
            .link_id = link_id,
            .external_id = created.external_id,
            .external_url = created.external_url,
            .files_published = bundle.files,
            .bytes_published = bundle.body.len,
        }, .{}, ctx.stdout);
        try ctx.stdout.print("\n", .{});
    } else {
        try ctx.stdout.print(
            "published {d} workbench file(s) for plan:{d} to {s}:{s}\n",
            .{ bundle.files, plan.id, system.slug, created.external_id },
        );
    }
}

const Bundle = struct {
    body: []u8,
    files: usize,
};

fn buildBundle(ctx: *const runtime.Ctx, d: anytype, root: []const u8, plan_id: i64) !Bundle {
    const rows = try engine.workbench.manifest.load(d, ctx.allocator, plan_id);
    defer engine.workbench.manifest.deinitRows(rows, ctx.allocator);

    var out: std.Io.Writer.Allocating = .init(ctx.allocator);
    defer out.deinit();
    var files: usize = 0;
    for (rows) |row| {
        const abs_path = try std.fs.path.join(ctx.allocator, &.{ root, row.file_path });
        defer ctx.allocator.free(abs_path);
        const content = try std.Io.Dir.cwd().readFileAlloc(
            ctx.io,
            abs_path,
            ctx.allocator,
            .limited(4 * 1024 * 1024),
        );
        defer ctx.allocator.free(content);
        if (files > 0) try out.writer.writeAll("\n\n---\n\n");
        try out.writer.print("<!-- planar-workbench: {s} -->\n\n{s}", .{ row.file_path, content });
        files += 1;
    }
    return .{ .body = try ctx.allocator.dupe(u8, out.written()), .files = files };
}
