//! handlers/annotate/bulk — shared helpers for bulk-resolve / bulk-dismiss / bulk-archive.

const std = @import("std");
const db_mod = @import("db");
const engine = @import("engine");
const runtime = @import("../../runtime.zig");

pub const Action = enum { resolve, dismiss, archive };

/// Apply `action` to every annotation matching `filter`. Returns the
/// number of rows that transitioned. Filter rows already in the
/// target terminal state are skipped (the engine's per-verb status
/// guard would otherwise refuse).
pub fn apply(
    d: *db_mod.sqlite.Db,
    ctx: *const runtime.Ctx,
    filter: engine.planning.annotation.ListFilter,
    action: Action,
) !usize {
    const items = try engine.planning.annotation.list(d, ctx.allocator, filter);
    defer engine.planning.annotation.deinitMany(items, ctx.allocator);

    var count: usize = 0;
    for (items) |a| {
        // Skip rows already in the target terminal state — calling
        // resolve/dismiss/archive on them would return TerminalStatus.
        const already_done = switch (action) {
            .resolve => a.status == .resolved,
            .dismiss => a.status == .dismissed,
            .archive => a.status == .archived,
        };
        if (already_done) continue;

        // Skip already-terminal rows (different terminal than ours)
        // for resolve/dismiss — they can't transition from another
        // terminal state. For archive, allow archiving from any
        // status (operator intent: clear the desk).
        if (action != .archive and a.status.isTerminal()) continue;

        const updated = switch (action) {
            .resolve => engine.planning.annotation.resolve(d, ctx.allocator, a.id),
            .dismiss => engine.planning.annotation.dismiss(d, ctx.allocator, a.id),
            .archive => engine.planning.annotation.archive(d, ctx.allocator, a.id),
        } catch |e| switch (e) {
            error.TerminalStatus => continue,
            else => return e,
        };
        engine.planning.annotation.deinit(updated, ctx.allocator);
        count += 1;
    }
    return count;
}

pub fn emit(ctx: *const runtime.Ctx, verb_name: []const u8, count: usize, json: bool) !void {
    if (json) {
        try ctx.stdout.print("{{\"ok\":true,\"action\":\"{s}\",\"count\":{d}}}\n", .{ verb_name, count });
    } else {
        try ctx.stdout.print("{s}: {d} annotation(s)\n", .{ verb_name, count });
    }
}
