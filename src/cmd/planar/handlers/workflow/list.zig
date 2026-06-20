//! handlers/workflow/list.zig — `planar workflow list [--local] [--json]`
//!
//! Lists shipped and sandbox Lua workflows.  Read-only; no SQLite handle.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const dirs = @import("dirs.zig");
const scan = @import("scan.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workflow", "list" }, args_ptr);
    const ctx = runtime.current();
    const allocator = ctx.allocator;

    const d = dirs.resolve(ctx) catch |e|
        exit.die(ctx, e, "resolving workflow dirs: {s}", .{@errorName(e)});
    defer d.deinit(allocator);

    // Collect shipped (unless --local filters to sandbox only) + sandbox entries.
    var all: std.ArrayList(scan.WorkflowEntry) = .empty;
    // all_entries owns every WorkflowEntry item.  On any error or at exit,
    // deinit each item individually then free the list.
    defer {
        for (all.items) |e| scan.freeEntry(e, allocator);
        all.deinit(allocator);
    }

    if (!args.local) {
        const shipped = scan.scan(d.shipped, false, allocator, ctx.io) catch |e|
            exit.die(ctx, e, "scanning shipped workflows: {s}", .{@errorName(e)});
        // appendSlice moves the entries; then free just the slice header.
        try all.appendSlice(allocator, shipped);
        allocator.free(shipped);
    }

    {
        const sandbox = scan.scan(d.sandbox, true, allocator, ctx.io) catch |e|
            exit.die(ctx, e, "scanning sandbox workflows: {s}", .{@errorName(e)});
        try all.appendSlice(allocator, sandbox);
        allocator.free(sandbox);
    }

    if (args.json) {
        for (all.items) |e| {
            try emitJson(ctx, e);
        }
        return;
    }

    if (all.items.len == 0) {
        const label: []const u8 = if (args.local) "sandbox" else "shipped + sandbox";
        try ctx.stdout.print("no {s} workflows found\n", .{label});
        return;
    }

    // Text table: name, kind, phases, description.
    try ctx.stdout.print("{s:<24}  {s:<8}  {s:<20}  {s}\n", .{
        "name", "kind", "phases", "description",
    });
    for (all.items) |e| {
        const kind: []const u8 = if (e.is_local) "local" else "shipped";
        try ctx.stdout.print("{s:<24}  {s:<8}  {s:<20}  {s}\n", .{
            e.effectiveName(),
            kind,
            e.workflow_meta.phases,
            e.workflow_meta.description,
        });
    }
}

fn emitJson(ctx: *const runtime.Ctx, e: scan.WorkflowEntry) !void {
    const payload = .{
        .name = e.effectiveName(),
        .kind = if (e.is_local) @as([]const u8, "local") else "shipped",
        .path = e.path,
        .filename = e.filename,
        .meta_found = e.meta_found,
        .description = e.workflow_meta.description,
        .phases = e.workflow_meta.phases,
        .seam = e.workflow_meta.seam,
    };
    try std.json.Stringify.value(payload, .{ .emit_null_optional_fields = false }, ctx.stdout);
    try ctx.stdout.print("\n", .{});
}
