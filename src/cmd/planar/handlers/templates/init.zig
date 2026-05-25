//! handlers/templates/init — `planar templates init`.
//!
//! Extract the embedded baseline templates to
//! `<templates_root>/default/<system>/<kind>.json`. Idempotent — existing
//! files are never overwritten.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "init" }, args_ptr);
    const ctx = runtime.current();
    _ = args.force;

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const created = engine.templates.initOnDisk(ctx.allocator, root) catch |e|
        exit.die(ctx, e, "templates init: {s}", .{@errorName(e)});
    defer engine.templates.init_mod.freeCreatedList(created, ctx.allocator);

    if (args.json) {
        for (created) |p| {
            try ctx.stdout.print("{{\"path\":", .{});
            try output.writeJsonString(ctx.stdout, p);
            try ctx.stdout.print("}}\n", .{});
        }
        return;
    }

    if (created.len == 0) {
        try ctx.stdout.print("templates init: nothing to do (all templates already present)\n", .{});
        return;
    }
    try ctx.stdout.print("templates init: wrote {d} file(s)\n", .{created.len});
    for (created) |p| try ctx.stdout.print("  {s}\n", .{p});
}
