//! handlers/audit/handoff-readiness — `planar audit handoff-readiness [--threshold]`
//!
//! Runs `resume.validate` against every in-flight task (status in
//! {todo, doing, blocked}) and reports the pass rate. Exits 1 when the
//! pass rate is below `--threshold` (default 90).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("../../runtime.zig");
const exit = @import("../../exit.zig");

const TaskResult = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
    resumable: bool,
};

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "audit", "handoff-readiness" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const threshold: i64 = args.threshold;

    var results: std.ArrayList(TaskResult) = .empty;
    defer {
        for (results.items) |r| {
            ctx.allocator.free(r.title);
            ctx.allocator.free(r.status);
        }
        results.deinit(ctx.allocator);
    }

    // Collect in-flight tasks.
    const sql: [:0]const u8 =
        "select id, coalesce(title,''), coalesce(status,'') from tasks " ++
        "where status in ('todo','doing','blocked') order by id";
    var stmt = d.prepare(sql) catch |e| exit.die(ctx, e, "task scan: {s}", .{@errorName(e)});
    defer stmt.finalize();
    var passing: i64 = 0;
    while (true) {
        switch (stmt.step() catch |e| exit.die(ctx, e, "task step: {s}", .{@errorName(e)})) {
            .done => break,
            .row => {
                const tid = stmt.columnInt(0);
                const title = try stmt.columnTextAlloc(1, ctx.allocator);
                const status = try stmt.columnTextAlloc(2, ctx.allocator);
                const r = engine.runtime.@"resume".validate(d, ctx.allocator, tid) catch {
                    try results.append(ctx.allocator, .{
                        .id = tid,
                        .title = title,
                        .status = status,
                        .resumable = false,
                    });
                    continue;
                };
                defer engine.runtime.@"resume".deinitResult(r, ctx.allocator);
                if (r.resumable) passing += 1;
                try results.append(ctx.allocator, .{
                    .id = tid,
                    .title = title,
                    .status = status,
                    .resumable = r.resumable,
                });
            },
        }
    }

    const total: i64 = @intCast(results.items.len);
    const failing = total - passing;
    const pct: f64 = if (total > 0) @as(f64, @floatFromInt(passing)) / @as(f64, @floatFromInt(total)) * 100.0 else 0.0;
    const ok = total == 0 or @as(i64, @intFromFloat(pct)) >= threshold;

    if (args.json) {
        try ctx.stdout.print(
            "{{\"total\":{d},\"passing\":{d},\"failing\":{d},\"percentage\":{d:.2},\"threshold\":{d},\"ok\":{s}}}\n",
            .{ total, passing, failing, pct, threshold, if (ok) "true" else "false" },
        );
        if (!ok) exit.die(ctx, error.NotFound, "handoff readiness below threshold", .{});
        return;
    }

    try ctx.stdout.print("handoff-readiness: {d}/{d} tasks pass ({d:.0}%, threshold {d}%)\n", .{
        passing, total, pct, threshold,
    });
    for (results.items) |r| {
        if (!r.resumable) {
            try ctx.stdout.print("  FAIL task:{d} \"{s}\" [{s}]\n", .{
                @as(u64, @intCast(r.id)), r.title, r.status,
            });
        }
    }
    if (ok) {
        try ctx.stdout.print("OK: threshold met\n", .{});
    } else {
        try ctx.stdout.print("FAIL: threshold not met ({d:.0}% < {d}%)\n", .{ pct, threshold });
        exit.die(ctx, error.NotFound, "handoff readiness below threshold", .{});
    }
}
