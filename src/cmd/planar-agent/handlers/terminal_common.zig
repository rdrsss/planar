//! handlers/terminal_common — shared response renderer for the
//! complete/fail/release/block atomic ops. All four return the same
//! JSON shape: { ok, claim_token, claim, task }.

const std = @import("std");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const atomic = engine.runtime.agentactivity.atomic;
const sessioncommits = engine.runtime.sessioncommits;
const task_mod = engine.planning.task;

const json = @import("json.zig");
const exit = @import("../exit.zig");

/// Render a CompleteResult (also used as BlockResult). Owns nothing —
/// caller releases the result via its own `.deinit`.
pub fn emit(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    result: atomic.CompleteResult,
    use_json: bool,
) !void {
    const task = task_mod.show(d, ctx.allocator, result.task_id) catch |e|
        exit.die(ctx, e, "task show: {s}", .{@errorName(e)});
    defer task_mod.deinit(task, ctx.allocator);

    if (use_json) {
        const w = ctx.stdout;
        try w.print("{{\"ok\":true,\"claim_token\":", .{});
        try std.json.Stringify.encodeJsonString(result.claim.claim_token, .{}, w);
        try w.print(",\"claim\":", .{});
        try json.writeClaim(w, result.claim, null);
        try w.print(",\"task\":", .{});
        try json.writeTask(w, task);
        try w.print("}}\n", .{});
    } else {
        try ctx.stdout.print("ok task:{d} status:{s} claim_status:{s}\n", .{
            task.id,
            @tagName(task.status),
            result.claim.status.toText(),
        });
    }
}

/// Best-effort post-transaction commit attribution for terminal verbs.
pub fn collectCommits(
    ctx: *const runtime.Ctx,
    d: *db.sqlite.Db,
    result: atomic.CompleteResult,
    no_locality_probe: bool,
) void {
    sessioncommits.recordClaimWindowBestEffort(d, .{
        .allocator = ctx.allocator,
        .io = ctx.io,
        .no_locality_probe = no_locality_probe,
        .window = .{
            .claim_id = result.claim.id,
            .session_id = result.claim.session_id,
            .worktree_path = result.claim.worktree_path,
            .repo_root = result.claim.repo_root,
            .head_sha_at_claim = result.claim.head_sha_at_claim,
        },
    });
}
