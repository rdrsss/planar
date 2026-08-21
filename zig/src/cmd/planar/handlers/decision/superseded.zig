//! handlers/decision/superseded — `planar decision supersede <decision-id> --by <new-id> [--scope]`
//!
//! The CLI verb is `supersede` (Go-side parity); the source file is
//! still named `superseded.zig` to avoid churning the existing handler
//! file inventory. The verb spelling in cmd.zig is what matters.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const output = @import("../../output.zig");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "decision", "supersede" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const old_id = std.fmt.parseInt(i64, args.decision_id, 10) catch
        exit.die(ctx, error.InvalidInput, "decision id must be an integer, got '{s}'", .{args.decision_id});

    // --by is `required = true` in the spec; the parser enforces presence
    // before we get here, so the field is a non-optional i64.
    const new_id: i64 = args.by;

    _ = args.scope;

    const dec = engine.planning.decision.supersede(d, ctx.allocator, old_id, new_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, e, "no decision with id {d} or {d}", .{ old_id, new_id }),
        error.TerminalStatus => exit.die(ctx, e, "decision {d} is terminal; cannot supersede", .{old_id}),
        error.LinkExists => exit.die(ctx, e, "supersedes link from decision {d} to {d} already exists", .{ new_id, old_id }),
        else => exit.die(ctx, e, "decision supersede: {s}", .{@errorName(e)}),
    };

    try output.emit(ctx, engine.planning.decision, dec, .{ .json = args.json });
}
