//! handlers/run/finish — `planar run finish <run_uid>
//!   --status <completed|aborted|error> --json`
//!
//! Sets the terminal status on the named run and stamps `ended_at`.
//! `status` is enum-validated at this layer (same closed set as bench finish).
//!
//! JSON output shape (--json):
//!
//!   {"run_uid": "<uid>", "status": "<status>"}

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

/// Terminal status values accepted by `run finish`.
const valid_statuses = [_][]const u8{ "completed", "aborted", "error" };

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "finish" }, args_ptr);
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
            "run finish: invalid --status '{s}'; expected completed, aborted, or error",
            .{args.status},
        );
    }

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "run finish: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "run finish: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    engine.runs.lifecycle.finish(d, run.id, args.status) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "run finish: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "run finish: {s}", .{@errorName(e)}),
    };

    const w = ctx.stdout;
    try w.print("{{", .{});
    try w.print("\"run_uid\":", .{});
    try output.writeJsonString(w, args.run_uid);
    try w.print(",\"status\":", .{});
    try output.writeJsonString(w, args.status);
    try w.print("}}\n", .{});
}
