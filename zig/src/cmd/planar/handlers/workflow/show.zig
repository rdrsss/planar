//! handlers/workflow/show.zig — `planar workflow show <name> [--json]`
//!
//! Resolves a workflow by name (shipped then sandbox), parses its @meta
//! block, and prints name / description / phases / seam / path.
//! Read-only; no SQLite handle.

const std = @import("std");
const cli = @import("cli");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const dirs = @import("dirs.zig");
const scan = @import("scan.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "workflow", "show" }, args_ptr);
    const ctx = runtime.current();
    const allocator = ctx.allocator;

    const name = args.name;

    const d = dirs.resolve(ctx) catch |e|
        exit.die(ctx, e, "resolving workflow dirs: {s}", .{@errorName(e)});
    defer d.deinit(allocator);

    // Search shipped first, then sandbox.
    const dirs_and_local = [_]struct { path: []const u8, local: bool }{
        .{ .path = d.shipped, .local = false },
        .{ .path = d.sandbox, .local = true },
    };

    for (dirs_and_local) |dl| {
        const entries = scan.scan(dl.path, dl.local, allocator, ctx.io) catch |e|
            exit.die(ctx, e, "scanning workflow dir: {s}", .{@errorName(e)});
        defer scan.deinitEntries(entries, allocator);

        for (entries) |e| {
            if (!std.mem.eql(u8, e.effectiveName(), name)) continue;

            // Found.
            if (args.json) {
                try emitJson(ctx, e);
            } else {
                try emitText(ctx, e);
            }
            return;
        }
    }

    // Not found.
    exit.die(ctx, error.NotFound, "workflow '{s}' not found", .{name});
}

fn emitText(ctx: *const runtime.Ctx, e: scan.WorkflowEntry) !void {
    const kind: []const u8 = if (e.is_local) "local" else "shipped";
    try ctx.stdout.print("name:        {s}\n", .{e.effectiveName()});
    try ctx.stdout.print("kind:        {s}\n", .{kind});
    try ctx.stdout.print("path:        {s}\n", .{e.path});
    try ctx.stdout.print("meta:        {s}\n", .{if (e.meta_found) "present" else "absent"});
    if (e.workflow_meta.description.len > 0) {
        try ctx.stdout.print("description: {s}\n", .{e.workflow_meta.description});
    }
    if (e.workflow_meta.phases.len > 0) {
        try ctx.stdout.print("phases:      {s}\n", .{e.workflow_meta.phases});
    }
    if (e.workflow_meta.seam.len > 0) {
        try ctx.stdout.print("seam:        {s}\n", .{e.workflow_meta.seam});
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
