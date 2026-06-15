//! handlers/bench/finish — `planar bench finish <run-uid>
//!   --status <completed|aborted|error>`
//!
//! Sets the terminal status on the named run and stamps `ended_at`.
//! `status` is enum-validated at this layer.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

/// Terminal status values accepted by `bench finish`.
const valid_statuses = [_][]const u8{ "completed", "aborted", "error" };

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "finish" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Enum-validate status at the parse layer.
    var status_ok = false;
    for (valid_statuses) |s| {
        if (std.mem.eql(u8, args.status, s)) {
            status_ok = true;
            break;
        }
    }
    if (!status_ok) {
        exit.die(
            ctx,
            error.InvalidInput,
            "bench finish: invalid --status '{s}'; expected completed, aborted, or error",
            .{args.status},
        );
    }

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench finish: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench finish: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    engine.runs.lifecycle.finish(d, run.id, args.status) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench finish: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench finish: {s}", .{@errorName(e)}),
    };

    try ctx.stdout.print("ok\n", .{});
}
