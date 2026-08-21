//! handlers/handoff/create — `planar handoff create <snapshot-id> [--vendor]`
//!
//! Escape-hatch verb: create a handoff row from an existing snapshot.
//! Per plan 144 M4, handoffs are operator-state — no scope guard.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const render = @import("render.zig");
const cmd = @import("cmd.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "handoff", "create" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const snap_id = std.fmt.parseInt(i64, args.snapshot_id, 10) catch
        exit.die(ctx, error.InvalidInput, "snapshot id must be an integer, got '{s}'", .{args.snapshot_id});

    // Look up the snapshot to get from_vendor + task_id.
    const snap = engine.runtime.snapshot.show(d, ctx.allocator, snap_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "snapshot {d} not found", .{snap_id}),
        else => exit.die(ctx, e, "snapshot lookup failed: {s}", .{@errorName(e)}),
    };
    defer engine.runtime.snapshot.deinit(snap, ctx.allocator);

    // Capture worktree context from the active claim, if any. Plan 297
    // followup t#2947.
    var wt: cmd.WorktreeContext = .{};
    defer wt.deinit(ctx.allocator);
    if (snap.task_id) |tid| wt = cmd.resolveWorktreeForTask(d, ctx.allocator, tid);

    const h = engine.runtime.handoff.create(d, ctx.allocator, .{
        .from_snapshot_id = snap.id,
        .from_vendor = snap.vendor,
        .to_vendor = args.vendor,
        .worktree_path = wt.worktree_path,
        .repo_root = wt.repo_root,
        .branch = wt.branch,
    }) catch |e| exit.die(ctx, e, "handoff create: {s}", .{@errorName(e)});
    defer engine.runtime.handoff.deinit(h, ctx.allocator);

    try render.emitOne(ctx, h, args.json);
}
