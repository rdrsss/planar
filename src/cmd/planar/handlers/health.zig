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
const runtime = @import("runtime");
const output = @import("../output.zig");
const exit = @import("../exit.zig");
const skills_common = @import("skills/common.zig");

pub const verb: cli.Cmd = .{
    .name = "health",
    .desc = "Report database, handoff, and installed-projection health.",
    .long_desc = "Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, pending handoff staleness,\n  and manifest-owned installed projection freshness. This command is\n  read-only; recovery commands are reported but never run.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks not resumable, stale handoffs, stale\n       or missing managed projections, integrity errors, etc.)",
    .flags = &.{
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"health"}, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const base_report = engine.health.check(d, ctx.db_path) catch |e| exit.die(
        ctx,
        e,
        "health check failed: {s}",
        .{@errorName(e)},
    );
    const homes = skills_common.resolveHomes(ctx) catch |e| exit.die(ctx, e, "health check failed: resolving install homes: {s}", .{@errorName(e)});
    defer homes.deinit(ctx.allocator);
    var installed = engine.installedsurface.status(ctx.allocator, homes.options(null)) catch |e| exit.die(
        ctx,
        e,
        "health check failed: installed projection status: {s}",
        .{@errorName(e)},
    );
    defer installed.deinit();
    const report = engine.health.withProjectionFreshness(base_report, installed);
    try output.emit(ctx, engine.health, report, .{ .json = args.json });

    // Mirror Go's exit-1-on-DEGRADED contract (Cluster C-health-content-
    // loss, plan 351 Q235). Oncall scrapes treat a non-zero exit as
    // "needs attention"; collapsing it to 0 hides the degraded signal.
    if (std.mem.eql(u8, report.overall, "degraded")) {
        runtime.shutdown();
        std.process.exit(1);
    }
}
