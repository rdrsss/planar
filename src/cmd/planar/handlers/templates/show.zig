//! handlers/templates/show — `planar templates show <set> <system> <kind>`.
//!
//! Print the resolved template's raw JSON to stdout. Honours the loader's
//! three-level fallback chain.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "show" }, args_ptr);
    const ctx = runtime.current();

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const t = engine.templates.load(ctx.allocator, args.set, args.system, args.kind, root) catch |e| switch (e) {
        error.TemplateNotFound => exit.die(ctx, error.NotFound, "template {s}/{s}/{s} not found", .{ args.set, args.system, args.kind }),
        else => exit.die(ctx, e, "templates show: {s}", .{@errorName(e)}),
    };
    defer engine.templates.deinitTemplate(t, ctx.allocator);

    try ctx.stdout.print("{s}", .{t.raw});
    if (t.raw.len > 0 and t.raw[t.raw.len - 1] != '\n') try ctx.stdout.print("\n", .{});
}
