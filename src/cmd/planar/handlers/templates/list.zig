//! handlers/templates/list — `planar templates list [--system …] [--set …] [--json]`.
//!
//! Walk the disk templates root and the embedded defaults, deduplicate, and
//! print every (set, system, kind, source) row. Disk entries supersede
//! embedded entries with the same (set, system, kind) triple.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");
const common = @import("common.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "templates", "list" }, args_ptr);
    const ctx = runtime.current();

    const root = common.resolveTemplatesRoot(ctx) catch |e|
        exit.die(ctx, e, "resolving templates root: {s}", .{@errorName(e)});
    defer ctx.allocator.free(root);

    const disk = engine.templates.listEntries(ctx.allocator, root) catch |e|
        exit.die(ctx, e, "listing templates on disk: {s}", .{@errorName(e)});
    defer engine.templates.deinitListEntries(disk, ctx.allocator);

    const embedded = engine.templates.listEmbeddedEntries(ctx.allocator) catch |e|
        exit.die(ctx, e, "listing embedded templates: {s}", .{@errorName(e)});
    defer engine.templates.deinitListEntries(embedded, ctx.allocator);

    // Merge: disk entries supersede embedded entries for the same triple.
    var merged: std.ArrayList(engine.templates.ListEntry) = .empty;
    defer merged.deinit(ctx.allocator);

    for (disk) |e| try merged.append(ctx.allocator, e);
    outer: for (embedded) |e| {
        for (disk) |d| {
            if (std.mem.eql(u8, d.set_name, e.set_name) and
                std.mem.eql(u8, d.system, e.system) and
                std.mem.eql(u8, d.kind, e.kind)) continue :outer;
        }
        try merged.append(ctx.allocator, e);
    }

    // Filter.
    var filtered: std.ArrayList(engine.templates.ListEntry) = .empty;
    defer filtered.deinit(ctx.allocator);
    for (merged.items) |e| {
        if (args.system) |sys| {
            if (sys.len > 0 and !std.mem.eql(u8, e.system, sys)) continue;
        }
        if (args.set) |set| {
            if (set.len > 0 and !std.mem.eql(u8, e.set_name, set)) continue;
        }
        try filtered.append(ctx.allocator, e);
    }

    // Sort by (set, system, kind).
    std.mem.sort(engine.templates.ListEntry, filtered.items, {}, struct {
        fn lt(_: void, a: engine.templates.ListEntry, b: engine.templates.ListEntry) bool {
            var ord = std.mem.order(u8, a.set_name, b.set_name);
            if (ord != .eq) return ord == .lt;
            ord = std.mem.order(u8, a.system, b.system);
            if (ord != .eq) return ord == .lt;
            return std.mem.order(u8, a.kind, b.kind) == .lt;
        }
    }.lt);

    if (args.json) {
        for (filtered.items) |e| {
            try ctx.stdout.print("{{\"set\":", .{});
            try output.writeJsonString(ctx.stdout, e.set_name);
            try ctx.stdout.print(",\"system\":", .{});
            try output.writeJsonString(ctx.stdout, e.system);
            try ctx.stdout.print(",\"kind\":", .{});
            try output.writeJsonString(ctx.stdout, e.kind);
            try ctx.stdout.print(",\"source\":", .{});
            try output.writeJsonString(ctx.stdout, e.source);
            try ctx.stdout.print(",\"path\":", .{});
            try output.writeJsonString(ctx.stdout, e.path);
            try ctx.stdout.print("}}\n", .{});
        }
        return;
    }

    if (filtered.items.len == 0) {
        try ctx.stdout.print("no templates found\n", .{});
        return;
    }

    try ctx.stdout.print("{s:<20} {s:<20} {s:<20} {s}\n", .{ "SET", "SYSTEM", "KIND", "SOURCE" });
    try ctx.stdout.print("----------------------------------------------------------------------\n", .{});
    for (filtered.items) |e| {
        try ctx.stdout.print("{s:<20} {s:<20} {s:<20} {s}\n", .{ e.set_name, e.system, e.kind, e.source });
    }
}
