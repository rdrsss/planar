//! handlers/handoff/cmd.zig — `planar handoff [<task-id>]` + subverbs.
//!
//! Top-level run is the combined ritual: capture snapshot → create
//! handoff → validate → append note. Subverbs are escape hatches.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

const create = @import("create.zig");
const validate = @import("validate.zig");
const consume = @import("consume.zig");
const abandon = @import("abandon.zig");
const list = @import("list.zig");
const show = @import("show.zig");
const render = @import("render.zig");

pub const verb: cli.Cmd = .{
    .name = "handoff",
    .desc = "Capture a context snapshot and create a validated handoff record.",
    .flags = &.{
        .{ .long = "--vendor", .kind = .string },
        .{ .long = "--note", .kind = .string },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .positionals = &.{
        .{ .name = "task-id", .kind = .string, .required = false },
    },
    .run = cli.handler(handle),
    .cmds = &.{
        // All subverbs inherit --json from parent `handoff` — don't redeclare.
        .{
            .name = "create",
            .desc = "Create a handoff from an existing snapshot.",
            // --vendor is inherited from parent.
            .positionals = &.{.{ .name = "snapshot-id", .kind = .string, .required = true }},
            .run = cli.handler(create.handle),
        },
        .{
            .name = "validate",
            .desc = "Validate a pending handoff.",
            .positionals = &.{.{ .name = "handoff-id", .kind = .string, .required = true }},
            .run = cli.handler(validate.handle),
        },
        .{
            .name = "consume",
            .desc = "Mark a handoff as consumed.",
            .flags = &.{.{ .long = "--session", .kind = .int }},
            .positionals = &.{.{ .name = "handoff-id", .kind = .string, .required = true }},
            .run = cli.handler(consume.handle),
        },
        .{
            .name = "abandon",
            .desc = "Abandon a non-terminal handoff.",
            .flags = &.{.{ .long = "--reason", .kind = .string }},
            .positionals = &.{.{ .name = "handoff-id", .kind = .string, .required = true }},
            .run = cli.handler(abandon.handle),
        },
        .{
            .name = "list",
            .desc = "List handoffs.",
            .flags = &.{.{ .long = "--status", .kind = .string }},
            .run = cli.handler(list.handle),
        },
        .{
            .name = "show",
            .desc = "Show a handoff's details.",
            .positionals = &.{.{ .name = "handoff-id", .kind = .string, .required = true }},
            .run = cli.handler(show.handle),
        },
    },
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"handoff"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Resolve vendor + vendor_session_id from environment (Go parity).
    const vendor: []const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR")) |v|
        (if (v.len > 0) @as([]const u8, v) else "cli")
    else
        "cli";
    const vsid_opt: ?[]const u8 = if (ctx.environ.getPosix("PLANAR_VENDOR_SESSION_ID")) |v|
        (if (v.len > 0) @as([]const u8, v) else null)
    else
        null;

    // Step 0: require an active session for this vendor tuple.
    // Mirrors Go's runHandoff: an explicit `planar capture session` is the
    // documented prerequisite. We previously auto-started a session here
    // for ergonomics, which masked the missing-session error and produced
    // a different audit shape than Go. Plan 314 task 2296 locks parity.
    const sess_opt = engine.runtime.session.activeForVendor(d, ctx.allocator, vendor, vsid_opt) catch |e|
        exit.die(ctx, e, "looking up active session: {s}", .{@errorName(e)});
    const sess: engine.runtime.session.Session = sess_opt orelse
        exit.die(ctx, error.InvalidInput, "no active session (run `planar capture session` first)", .{});
    defer engine.runtime.session.deinit(sess, ctx.allocator);

    // Resolve task id: explicit positional, or session.task_id, or none.
    var task_id_opt: ?i64 = null;
    if (args.task_id) |raw| {
        task_id_opt = std.fmt.parseInt(i64, raw, 10) catch
            exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{raw});
    } else if (sess.task_id) |tid| {
        task_id_opt = tid;
    }

    // Snapshot next_action from the task, when known.
    var next_action_owned: ?[]const u8 = null;
    defer if (next_action_owned) |buf| ctx.allocator.free(buf);
    if (task_id_opt) |tid| {
        const sql: [:0]const u8 = "select coalesce(next_action, '') from tasks where id = ?";
        var stmt = d.prepare(sql) catch |e| exit.die(ctx, e, "task lookup prep: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = tid }}) catch |e| exit.die(ctx, e, "task lookup bind: {s}", .{@errorName(e)});
        switch (stmt.step() catch |e| exit.die(ctx, e, "task lookup step: {s}", .{@errorName(e)})) {
            .done => exit.die(ctx, error.NotFound, "task {d} not found", .{tid}),
            .row => next_action_owned = try stmt.columnTextAlloc(0, ctx.allocator),
        }
    }

    // Step 1: snapshot.
    const snap = engine.runtime.snapshot.create(d, ctx.allocator, .{
        .session_id = sess.id,
        .task_id = task_id_opt,
        .vendor = vendor,
        .vendor_session_id = vsid_opt,
        .body = args.note,
        .next_action = next_action_owned,
    }) catch |e| exit.die(ctx, e, "snapshot create: {s}", .{@errorName(e)});
    defer engine.runtime.snapshot.deinit(snap, ctx.allocator);

    // Step 2: create handoff (pending).
    const h_pending = engine.runtime.handoff.create(d, ctx.allocator, .{
        .from_snapshot_id = snap.id,
        .from_vendor = vendor,
        .to_vendor = args.vendor,
    }) catch |e| exit.die(ctx, e, "handoff create: {s}", .{@errorName(e)});
    engine.runtime.handoff.deinit(h_pending, ctx.allocator);

    // Step 3: validate.
    const h = engine.runtime.handoff.validate(d, ctx.allocator, h_pending.id) catch |e|
        exit.die(ctx, e, "handoff validate: {s}", .{@errorName(e)});
    defer engine.runtime.handoff.deinit(h, ctx.allocator);

    // Step 4: append session_entries note.
    const entry_body = std.fmt.allocPrint(
        ctx.allocator,
        "handoff captured: snapshot={d} handoff={d}",
        .{ snap.id, h.id },
    ) catch |e| exit.die(ctx, e, "format entry: {s}", .{@errorName(e)});
    defer ctx.allocator.free(entry_body);
    engine.runtime.session.appendEntry(d, sess.id, "note", entry_body) catch {};

    // Resumability check.
    var resumable = false;
    if (task_id_opt) |tid| {
        const r = engine.runtime.@"resume".validate(d, ctx.allocator, tid) catch null;
        if (r) |vr| {
            defer engine.runtime.@"resume".deinitResult(vr, ctx.allocator);
            resumable = vr.resumable;
        }
    }

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"snapshot_id\":{d},\"handoff_id\":{d},\"status\":\"{s}\",\"resumable\":{s}}}\n",
            .{
                snap.id,                            h.id, @tagName(h.status),
                if (resumable) "true" else "false",
            },
        );
        return;
    }

    if (task_id_opt) |tid| {
        try ctx.stdout.print("handoff captured for task:{d}\n", .{@as(u64, @intCast(tid))});
    } else {
        try ctx.stdout.print("handoff captured for current task\n", .{});
    }
    try ctx.stdout.print("  snapshot: {d}  vendor: {s}  next_action: {s}\n", .{
        @as(u64, @intCast(snap.id)), snap.vendor, snap.next_action,
    });
    try ctx.stdout.print("  handoff:  {d}  status: {s}\n", .{ @as(u64, @intCast(h.id)), @tagName(h.status) });
    if (task_id_opt != null) {
        if (resumable) {
            try ctx.stdout.print("  validate: PASS — task is resume-ready\n", .{});
        } else {
            try ctx.stdout.print("  validate: FAIL — task is not resume-ready\n", .{});
        }
    }
    _ = render.emitOne; // keep import live in case the file is re-used.
}
