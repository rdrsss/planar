//! handlers/audit/session — `planar audit session <session-id>`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "audit", "session" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const id = std.fmt.parseInt(i64, args.session_id, 10) catch
        exit.die(ctx, error.InvalidInput, "session id must be an integer, got '{s}'", .{args.session_id});

    const t = engine.runtime.audit_trail.sessionTimeline(d, ctx.allocator, id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "session {d} not found", .{id}),
        else => exit.die(ctx, e, "audit session: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.audit_trail.deinitTimeline(t, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print(
            "{{\"id\":{d},\"vendor\":\"{s}\",\"started_at\":\"{s}\"",
            .{ t.session_id, t.vendor, t.started_at },
        );
        if (t.task_id) |tid| try ctx.stdout.print(",\"task_id\":{d}", .{tid});
        if (t.ended_at) |e| try ctx.stdout.print(",\"ended_at\":\"{s}\"", .{e});
        try ctx.stdout.print(",\"entries\":[", .{});
        for (t.entries, 0..) |e, i| {
            if (i > 0) try ctx.stdout.print(",", .{});
            try ctx.stdout.print(
                "{{\"ordinal\":{d},\"prefix\":\"{s}\",\"body\":\"{s}\"}}",
                .{ e.ordinal, e.prefix, e.body },
            );
        }
        try ctx.stdout.print("]}}\n", .{});
        return;
    }

    const task_str: []const u8 = if (t.task_id) |_| "task:" else "(no task)";
    if (t.task_id) |tid| {
        try ctx.stdout.print("session {d}  vendor: {s}  task:{d}  {s}", .{
            @as(u64, @intCast(t.session_id)), t.vendor, @as(u64, @intCast(tid)), t.started_at,
        });
    } else {
        try ctx.stdout.print("session {d}  vendor: {s}  {s}  {s}", .{
            @as(u64, @intCast(t.session_id)), t.vendor, task_str, t.started_at,
        });
    }
    if (t.ended_at) |e| try ctx.stdout.print(" → {s}", .{e}) else try ctx.stdout.print(" → (active)", .{});
    try ctx.stdout.print("\n", .{});
    for (t.entries) |e| {
        try ctx.stdout.print("  {d:<4}  [{s:<12}]  {s}\n", .{ @as(u64, @intCast(e.ordinal)), e.prefix, e.body });
    }
}
