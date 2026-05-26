//! handlers/health.zig — `planar health`
//!
//! Reference handler showing the full Tier-1-4 split:
//!   1. cli.castArgs   recovers the typed args
//!   2. runtime.current() yields the process Ctx (db, writers, …)
//!   3. engine.health.check runs the pure read-only domain logic
//!   4. output.emit emits the result (text or JSON per --json)
//!   5. exit.die normalizes error → user message + exit code

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../main.zig");
const runtime = @import("../runtime.zig");
const output = @import("../output.zig");
const exit = @import("../exit.zig");

pub const verb: cli.Cmd = .{
    .name = "health",
    .desc = "Report database and handoff-readiness health.",
    .long_desc = "Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, and pending handoff\n  staleness.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks not resumable, stale handoffs,\n       integrity errors, etc.)",
    .flags = &.{
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"health"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const report = engine.health.check(d) catch |e| exit.die(
        ctx,
        e,
        "health check failed: {s}",
        .{@errorName(e)},
    );
    try output.emit(ctx, engine.health, report, .{ .json = args.json });
}
