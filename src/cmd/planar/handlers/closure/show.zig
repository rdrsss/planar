//! handlers/closure/show — `planar closure show <task-id> [--json]`
//!
//! Reads back a task's persisted derived closure (the `closures` rows
//! written by `closure compute`), ordered modify → reference → transitive,
//! then path, then symbol. Read-only.
//!
//! JSON shape (--json):
//!   {
//!     "task_id": <n>,
//!     "rows": [
//!       { "id": <n>, "repo_id": <n>, "path": "...", "symbol": "...",
//!         "role": "modify|reference|transitive", "token_weight": <n>,
//!         "extractor_version": "...", "created_at": "..." }
//!     ]
//!   }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "closure", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const task_id = std.fmt.parseInt(i64, args.task_id, 10) catch
        exit.die(ctx, error.InvalidInput, "task id must be an integer, got '{s}'", .{args.task_id});

    const rows = engine.closure.store.show(d, ctx.allocator, task_id) catch |e|
        exit.die(ctx, e, "closure show: {s}", .{@errorName(e)});
    defer engine.closure.store.deinitRows(rows, ctx.allocator);

    if (args.json) {
        try emitJSON(ctx, task_id, rows);
    } else {
        try emitText(ctx, task_id, rows);
    }
}

fn emitJSON(ctx: *const runtime.Ctx, task_id: i64, rows: []const engine.closure.store.Row) !void {
    const w = ctx.stdout;
    try w.print("{{\"task_id\":{d},\"rows\":[", .{task_id});
    for (rows, 0..) |r, i| {
        if (i > 0) try w.print(",", .{});
        try w.print("{{\"id\":{d},\"repo_id\":{d},\"path\":", .{ r.id, r.repo_id });
        try output.writeJsonString(w, r.path);
        try w.print(",\"symbol\":", .{});
        try output.writeJsonString(w, r.symbol);
        try w.print(",\"role\":", .{});
        try output.writeJsonString(w, r.role);
        try w.print(",\"token_weight\":{d},\"extractor_version\":", .{r.token_weight});
        try output.writeJsonString(w, r.extractor_version);
        try w.print(",\"created_at\":", .{});
        try output.writeJsonString(w, r.created_at);
        try w.print("}}", .{});
    }
    try w.print("]}}\n", .{});
}

fn emitText(ctx: *const runtime.Ctx, task_id: i64, rows: []const engine.closure.store.Row) !void {
    const w = ctx.stdout;
    try w.print("closure for task {d} ({d} rows):\n", .{ task_id, rows.len });
    if (rows.len == 0) {
        try w.print("  (none — run `planar closure compute {d}` first)\n", .{task_id});
        return;
    }
    for (rows) |r| {
        try w.print("  [{s}] {s}::{s}  w={d}\n", .{ r.role, r.path, r.symbol, r.token_weight });
    }
}
