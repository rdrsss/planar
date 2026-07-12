const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const output = @import("../../../output.zig");
const exit = @import("../../../exit.zig");
pub fn handle(p: *const anyopaque) anyerror!void {
    comptime {
        @setEvalBranchQuota(4_000);
    }
    const args = cli.castArgs(main.root, &.{ "feedback", "triage", "list" }, p);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    var f: engine.planning.feedback_triage.Filter = .{ .plan_id = args.plan };
    if (args.severity) |s| f.severity = engine.planning.feedback_triage.parseSeverity(s) orelse exit.die(ctx, error.InvalidInput, "unknown severity '{s}'", .{s});
    if (args.disposition) |s| f.disposition = engine.planning.feedback_triage.parseDisposition(s) orelse exit.die(ctx, error.InvalidInput, "unknown disposition '{s}'", .{s});
    const rows = engine.planning.feedback_triage.list(d, ctx.allocator, f) catch |e| exit.die(ctx, e, "feedback triage list: {s}", .{@errorName(e)});
    defer engine.planning.feedback_triage.deinitMany(rows, ctx.allocator);
    try output.emitList(ctx, engine.planning.feedback_triage, rows, .{ .json = args.json });
}
