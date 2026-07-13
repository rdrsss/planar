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
    const args = cli.castArgs(main.root, &.{ "feedback", "triage", "show" }, p);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const ref = engine.planning.feedback_triage.parseRef(args.finding) catch exit.die(ctx, error.InvalidInput, "finding must be task:<id> or question:<id>", .{});
    const row = engine.planning.feedback_triage.show(d, ctx.allocator, ref) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no triage row for {s}", .{args.finding}),
        else => exit.die(ctx, e, "feedback triage show: {s}", .{@errorName(e)}),
    };
    defer engine.planning.feedback_triage.deinit(row, ctx.allocator);
    try output.emit(ctx, engine.planning.feedback_triage, row, .{ .json = args.json });
}
