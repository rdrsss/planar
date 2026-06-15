//! handlers/plan/descendants — `planar plan descendants <plan-id> --json`
//!
//! Emit the anchor plan's full subtree (child plans + tasks + scenarios)
//! in dependency-topological order. READ-ONLY.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "descendants" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    // Verify the anchor plan exists.
    {
        var stmt = try d.prepare("select count(*) from plans where id = ?");
        defer stmt.finalize();
        try stmt.bind(&.{.{ .int = id }});
        const step = try stmt.step();
        if (step == .done or stmt.columnInt(0) == 0) {
            exit.die(ctx, error.NotFound, "no plan with id {d}", .{id});
        }
    }

    const tree = engine.extsync.propagate.walkTree(ctx.allocator, d, id) catch |e|
        exit.die(ctx, e, "plan descendants: walk tree: {s}", .{@errorName(e)});
    defer engine.extsync.propagate.freeTree(tree, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print("[", .{});
        for (tree, 0..) |entry, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            const kind_str: []const u8 = switch (entry.kind) {
                .plan_anchor => "plan",
                .plan_child => "plan",
                .task => "task",
            };
            const role_str: []const u8 = switch (entry.kind) {
                .plan_anchor => "anchor",
                .plan_child => "child",
                .task => "task",
            };
            try ctx.stdout.print("{{\"kind\":", .{});
            try output.writeJsonString(ctx.stdout, kind_str);
            try ctx.stdout.print(",\"role\":", .{});
            try output.writeJsonString(ctx.stdout, role_str);
            try ctx.stdout.print(",\"id\":{d},\"title\":", .{entry.id});
            try output.writeJsonString(ctx.stdout, entry.title);
            try ctx.stdout.print("}}", .{});
        }
        try ctx.stdout.print("]\n", .{});
    } else {
        for (tree) |entry| {
            const prefix: []const u8 = switch (entry.kind) {
                .plan_anchor => "plan (anchor)",
                .plan_child => "plan (child)",
                .task => "task",
            };
            try ctx.stdout.print("{s}:{d}  {s}\n", .{ prefix, entry.id, entry.title });
        }
    }
}
