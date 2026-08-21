const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../../main.zig");
const runtime = @import("runtime");
const output = @import("../../../output.zig");
const exit = @import("../../../exit.zig");
const scope = @import("../../../scope.zig");
pub fn handle(p: *const anyopaque) anyerror!void {
    comptime {
        @setEvalBranchQuota(4_000);
    }
    const args = cli.castArgs(main.root, &.{ "feedback", "triage", "set" }, p);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();
    const ref = engine.planning.feedback_triage.parseRef(args.finding) catch exit.die(ctx, error.InvalidInput, "finding must be task:<id> or question:<id>", .{});
    const sev = engine.planning.feedback_triage.parseSeverity(args.severity) orelse exit.die(ctx, error.InvalidInput, "unknown severity '{s}'", .{args.severity});
    const disp = engine.planning.feedback_triage.parseDisposition(args.disposition) orelse exit.die(ctx, error.InvalidInput, "unknown disposition '{s}'", .{args.disposition});
    const repro = engine.planning.feedback_triage.parseReproduction(args.reproduction) orelse exit.die(ctx, error.InvalidInput, "unknown reproduction '{s}'", .{args.reproduction});
    const dup = if (args.duplicate_of) |s| engine.planning.feedback_triage.parseRef(s) catch exit.die(ctx, error.InvalidInput, "--duplicate-of must be task:<id> or question:<id>", .{}) else null;
    const write = try scope.resolveForWrite(ctx, args.scope);
    const owned = engine.planning.feedback_triage.entityScope(d, ctx.allocator, ref) catch |e| exit.die(ctx, e, "feedback finding: {s}", .{@errorName(e)});
    defer if (owned) |s| ctx.allocator.free(s);
    scope.guardWithMembership(d, owned, write.scope) catch exit.die(ctx, error.ScopeMismatch, "Refusing cross-scope write; pass --scope {s} or cd into the right repo.", .{owned orelse "global"});
    const row = engine.planning.feedback_triage.set(d, ctx.allocator, ref, .{ .severity = sev, .disposition = disp, .reproduction = repro, .duplicate_of = dup, .evidence = args.evidence }) catch |e| exit.die(ctx, e, "feedback triage set: {s}", .{@errorName(e)});
    defer engine.planning.feedback_triage.deinit(row, ctx.allocator);
    try output.emit(ctx, engine.planning.feedback_triage, row, .{ .json = args.json });
}
