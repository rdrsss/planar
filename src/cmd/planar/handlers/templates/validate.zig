//! handlers/templates/validate — `planar templates validate <set> <system> <kind>`.
//!
//! Load the resolved template and smoke-render every string field against a
//! stub context. Exit 0 on clean; exit 1 on any validation issue.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "validate" }, args_ptr);
    const ctx = runtime.current();

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const t = engine.templates.load(ctx.allocator, args.set, args.system, args.kind, root) catch |e| switch (e) {
        error.TemplateNotFound => exit.die(ctx, error.NotFound, "template {s}/{s}/{s} not found", .{ args.set, args.system, args.kind }),
        else => exit.die(ctx, e, "templates validate: load ({s})", .{@errorName(e)}),
    };
    defer engine.templates.deinitTemplate(t, ctx.allocator);

    const issues = engine.templates.validate(ctx.allocator, t) catch |e|
        exit.die(ctx, e, "templates validate: {s}", .{@errorName(e)});
    defer engine.templates.deinitIssues(issues, ctx.allocator);

    if (issues.len == 0) {
        try ctx.stdout.print("ok: {s}/{s}/{s}\n", .{ t.set_name, t.system, t.kind });
        return;
    }

    for (issues) |iss| {
        try ctx.stdout.print("ISSUE: {s} [{s}]: {s}\n", .{ t.path, iss.json_path, iss.message });
    }
    exit.die(ctx, error.InvalidInput, "{d} issue(s) in {s}/{s}/{s}", .{ issues.len, t.set_name, t.system, t.kind });
}
