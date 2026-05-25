//! handlers/capture/snapshot — `planar capture snapshot [<body>] [--task] [--note] [--next-action]`

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");
const util = @import("util.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "capture", "snapshot" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const sid = util.resolveSessionId(ctx, d, args.session);

    // Resolve task_id: --task flag wins; else session.task_id.
    var task_id: ?i64 = args.task;
    if (task_id == null) {
        const s = engine.runtime.session.getById(d, ctx.allocator, sid) catch |e|
            exit.die(ctx, e, "session lookup: {s}", .{@errorName(e)});
        defer engine.runtime.session.deinit(s, ctx.allocator);
        task_id = s.task_id;
    }

    // Resolve vendor (matches `capture session` semantics).
    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;

    // --note flag takes precedence over the positional body.
    const body: ?[]const u8 = args.note orelse args.body;

    // next_action: --next-action wins; else read from task.next_action.
    var next_action_owned: ?[]u8 = null;
    defer if (next_action_owned) |b| ctx.allocator.free(b);
    var next_action: ?[]const u8 = args.next_action;
    if (next_action == null and task_id != null) {
        const sql: [:0]const u8 = "select coalesce(next_action,'') from tasks where id = ?";
        var stmt = d.prepare(sql) catch |e| exit.die(ctx, e, "task lookup: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id.? }}) catch |e| exit.die(ctx, e, "task bind: {s}", .{@errorName(e)});
        switch (stmt.step() catch |e| exit.die(ctx, e, "task step: {s}", .{@errorName(e)})) {
            .row => {
                const owned = try stmt.columnTextAlloc(0, ctx.allocator);
                next_action_owned = @constCast(owned);
                if (owned.len > 0) next_action = owned;
            },
            .done => {},
        }
    }

    const snap = engine.runtime.capture.takeSnapshot(d, ctx.allocator, .{
        .session_id = sid,
        .task_id = task_id,
        .vendor = vendor,
        .vendor_session_id = vsid,
        .body = body,
        .next_action = next_action,
    }) catch |e| exit.die(ctx, e, "capture snapshot: {s}", .{@errorName(e)});
    defer engine.runtime.snapshot.deinit(snap, ctx.allocator);

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"id\":{d},\"session_id\":{d},\"vendor\":\"{s}\"",
            .{ snap.id, snap.session_id, snap.vendor },
        );
        if (snap.task_id) |t| try ctx.stdout.print(",\"task_id\":{d}", .{t});
        try ctx.stdout.print("}}\n", .{});
        return;
    }
    try ctx.stdout.print("snapshot {d} created (vendor: {s}", .{ @as(u64, @intCast(snap.id)), snap.vendor });
    if (snap.task_id) |t| try ctx.stdout.print(", task: {d}", .{t});
    try ctx.stdout.print(")\n", .{});
}
