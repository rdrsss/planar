//! handlers/annotate/sweep — sweep stale annotations.
//!
//! "Stale" = status in {resolved, dismissed} AND updated_at older
//! than the `--since-days` cutoff (default 30). Swept rows transition
//! to `archived`.
//!
//! Uses SQLite's `julianday(...)` for the cutoff comparison so we
//! don't need a Zig-side wall-clock primitive (std.time in 0.16
//! only exposes the per-unit constants).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const db_mod = @import("db");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "sweep" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const since_days: i64 = args.since_days;
    if (since_days < 0) exit.die(ctx, error.InvalidInput, "--since-days must be ≥ 0", .{});

    // Collect ids of rows that are (status in {resolved, dismissed})
    // AND updated_at is older than now minus `since_days` days. Then
    // archive each via the engine's transition helper so the audit
    // trail captures the transition correctly.
    var ids: std.ArrayList(i64) = .empty;
    defer ids.deinit(ctx.allocator);

    var sql_buf: [256]u8 = undefined;
    const sql_slice = std.fmt.bufPrintZ(
        &sql_buf,
        "select id from annotations" ++
            " where status in ('resolved','dismissed')" ++
            "   and (julianday('now') - julianday(updated_at)) > {d}",
        .{since_days},
    ) catch return exit.die(ctx, error.InvalidInput, "sweep: SQL too long", .{});

    {
        var stmt = d.prepare(sql_slice) catch |e|
            exit.die(ctx, e, "annotate sweep prep: {s}", .{@errorName(e)});
        defer stmt.finalize();
        while (true) {
            switch (stmt.step() catch |e| exit.die(ctx, e, "annotate sweep step: {s}", .{@errorName(e)})) {
                .done => break,
                .row => try ids.append(ctx.allocator, stmt.columnInt(0)),
            }
        }
    }

    var count: usize = 0;
    for (ids.items) |id| {
        // Under the retention-tier model (plan 692), resolved → archived
        // and dismissed → archived are legal, so archive() for rows
        // selected by the WHERE clause above will succeed.  The
        // TerminalStatus catch is kept as a defensive guard in case a
        // row was concurrently archived between the SELECT and this loop.
        const arc = engine.planning.annotation.archive(d, ctx.allocator, id) catch |e| switch (e) {
            error.TerminalStatus => continue,
            else => exit.die(ctx, e, "annotate sweep archive {d}: {s}", .{ id, @errorName(e) }),
        };
        engine.planning.annotation.deinit(arc, ctx.allocator);
        count += 1;
    }

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"action\":\"sweep\",\"since_days\":{d},\"swept\":{d}}}\n",
            .{ since_days, count },
        );
    } else {
        try ctx.stdout.print("sweep: archived {d} annotation(s) older than {d} day(s)\n", .{ count, since_days });
    }

    _ = db_mod; // imported for symmetry; the sweep query uses d.prepare directly.
}
