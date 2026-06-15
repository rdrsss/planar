//! handlers/run/event — `planar run event <run_uid> --kind <k>
//!   [--payload <json>] --json`
//!
//! Appends a journal event to the named run. The seq number is
//! auto-incremented (max current seq + 1) so operational callers do not
//! need to track per-run ordinals.
//!
//! JSON validation: when --payload is supplied it must be well-formed JSON.
//!
//! JSON output shape (--json):
//!
//!   {"run_uid": "<uid>", "seq": <n>, "kind": "<kind>"}

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "event" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    // Validate --payload is well-formed JSON when supplied.
    if (args.payload) |blob| {
        var parsed = std.json.parseFromSlice(std.json.Value, ctx.allocator, blob, .{}) catch
            exit.die(ctx, error.InvalidInput, "run event: --payload is not valid JSON: {s}", .{blob});
        parsed.deinit();
    }

    // Look up the run by uid.
    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "run event: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "run event: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    // Auto-increment seq: max(seq)+1 for this run. When no events exist yet,
    // max returns NULL which SQLite delivers as 0; coalesce gives seq=1.
    var seq_stmt = d.prepare(
        "select coalesce(max(seq), 0) + 1 from run_events where run_id = ?",
    ) catch exit.die(ctx, error.Unexpected, "run event: seq query failed", .{});
    defer seq_stmt.finalize();
    seq_stmt.bind(&.{.{ .int = run.id }}) catch
        exit.die(ctx, error.Unexpected, "run event: seq bind failed", .{});
    const next_seq: i64 = switch (seq_stmt.step() catch exit.die(ctx, error.Unexpected, "run event: seq step failed", .{})) {
        .row => seq_stmt.columnInt(0),
        .done => 1,
    };

    _ = engine.runs.lifecycle.event(d, run.id, next_seq, args.kind, args.payload) catch |e| switch (e) {
        error.DuplicateSeq => exit.die(
            ctx,
            error.AlreadyExists,
            "run event: seq race on run '{s}' (retry)",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "run event: {s}", .{@errorName(e)}),
    };

    const w = ctx.stdout;
    try w.print("{{", .{});
    try w.print("\"run_uid\":", .{});
    try output.writeJsonString(w, args.run_uid);
    try w.print(",\"seq\":{d}", .{next_seq});
    try w.print(",\"kind\":", .{});
    try output.writeJsonString(w, args.kind);
    try w.print("}}\n", .{});
}
