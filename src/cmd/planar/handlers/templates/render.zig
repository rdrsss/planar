//! handlers/templates/render — `planar templates render <set> <system> <kind> <entity-ref>`.
//!
//! Build a render context from the named entity (kind:id), render the
//! template, and print the resulting JSON payload to stdout. Pure dry run —
//! no DB writes or external calls.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "render" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const t = engine.templates.load(ctx.allocator, args.set, args.system, args.kind, root) catch |e| switch (e) {
        error.TemplateNotFound => exit.die(ctx, error.NotFound, "template {s}/{s}/{s} not found", .{ args.set, args.system, args.kind }),
        else => exit.die(ctx, e, "templates render: load: {s}", .{@errorName(e)}),
    };
    defer engine.templates.deinitTemplate(t, ctx.allocator);

    // Parse kind:id.
    const ref = args.entity_ref;
    const colon = std.mem.indexOfScalar(u8, ref, ':') orelse
        exit.die(ctx, error.InvalidInput, "--entity must be kind:id (got '{s}')", .{ref});
    const kind = ref[0..colon];
    const id = std.fmt.parseInt(i64, ref[colon + 1 ..], 10) catch
        exit.die(ctx, error.InvalidInput, "--entity id must be an integer (got '{s}')", .{ref[colon + 1 ..]});

    var owned = blk: {
        if (std.mem.eql(u8, kind, "task")) {
            break :blk engine.templates.builder.buildTaskContext(ctx.allocator, d, id) catch |e|
                exit.die(ctx, e, "building task context: {s}", .{@errorName(e)});
        } else if (std.mem.eql(u8, kind, "plan")) {
            break :blk engine.templates.builder.buildPlanContext(ctx.allocator, d, id) catch |e|
                exit.die(ctx, e, "building plan context: {s}", .{@errorName(e)});
        } else if (std.mem.eql(u8, kind, "scenario") or std.mem.eql(u8, kind, "test_scenario")) {
            break :blk engine.templates.builder.buildScenarioContext(ctx.allocator, d, id) catch |e|
                exit.die(ctx, e, "building scenario context: {s}", .{@errorName(e)});
        } else {
            exit.die(ctx, error.InvalidInput, "unsupported entity kind '{s}' (use task, plan, scenario)", .{kind});
        }
    };
    defer owned.deinit();

    var rendered = engine.templates.renderTemplate(ctx.allocator, t.fields, owned.ctx) catch |e|
        exit.die(ctx, e, "rendering template: {s}", .{@errorName(e)});
    defer rendered.deinit();

    const out = rendered.toJson(ctx.allocator) catch |e|
        exit.die(ctx, e, "encoding render output: {s}", .{@errorName(e)});
    defer ctx.allocator.free(out);
    try ctx.stdout.print("{s}\n", .{out});
}
