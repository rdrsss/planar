//! handlers/bench/event — `planar bench event <run-uid> --kind <k>
//!   --seq <n> [--payload <json>]`
//!
//! Appends a journal event to the named run. `seq` is a monotonic
//! per-run ordinal the caller assigns; `kind` and `payload` are opaque
//! text at the schema layer (JSON-validated here when payload is given).

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "event" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Validate --payload is well-formed JSON when supplied.
    if (args.payload) |blob| {
        var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, blob, .{}) catch
            exit.die(ctx, error.InvalidInput, "bench event: --payload is not valid JSON: {s}", .{blob});
        parsed.deinit();
    }

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench event: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench event: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    _ = engine.runs.lifecycle.event(d, run.id, args.seq, args.kind, args.payload) catch |e| switch (e) {
        error.DuplicateSeq => exit.die(
            ctx,
            error.AlreadyExists,
            "bench event: seq {d} already used for run '{s}'",
            .{ args.seq, args.run_uid },
        ),
        else => exit.die(ctx, e, "bench event: {s}", .{@errorName(e)}),
    };

    try ctx.stdout.print("ok\n", .{});
}
