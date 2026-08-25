//! handlers/annotate/sweep — sweep stale annotations.
//!
//! "Stale" = status in {resolved, dismissed} AND updated_at older
//! than the `--since-days` cutoff (default 30). Swept rows transition
//! to `archived`.
//!
//! `--scope` restricts the sweep to one scope-ref, using the same
//! predicate `annotate list` applies, and refuses an unresolvable slug
//! with `SlugNotFound` (exit 1) like every sibling leaf. Before plan
//! 1001 / task 6150 the flag was declared but ignored: a scoped sweep
//! archived every eligible row in the database, exited 0, and reported
//! an accurate count that gave no hint the blast radius had exceeded
//! the named scope.
//!
//! Uses SQLite's `julianday(...)` for the cutoff comparison so we
//! don't need a Zig-side wall-clock primitive (std.time in 0.16
//! only exposes the per-unit constants).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "annotate", "sweep" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const since_days: i64 = args.since_days;
    if (since_days < 0) exit.die(ctx, error.InvalidInput, "--since-days must be ≥ 0", .{});

    const count = engine.planning.annotation.sweep(d, ctx.allocator, .{
        .since_days = since_days,
        .scope = args.scope,
    }) catch |e| exit.die(ctx, e, "annotate sweep: {s}", .{@errorName(e)});

    if (args.json) {
        try ctx.stdout.print(
            "{{\"ok\":true,\"action\":\"sweep\",\"since_days\":{d},\"swept\":{d}}}\n",
            .{ since_days, count },
        );
    } else {
        try ctx.stdout.print("sweep: archived {d} annotation(s) older than {d} day(s)\n", .{ count, since_days });
    }
}
