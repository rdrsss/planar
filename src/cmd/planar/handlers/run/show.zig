//! handlers/run/show — `planar run show <run_uid> --json`
//!
//! Shows a run's full state: header row (from `runs`) and journal events
//! (from `run_events`, ordered by seq). Touches are omitted from the
//! operational surface — they are measurement-only and belong on `bench`.
//!
//! JSON shape (--json):
//!
//!   {
//!     "id":          <i64>,
//!     "run_uid":     <string>,
//!     "plan_id":     <i64>,
//!     "arm":         <string>,
//!     "status":      <string>,
//!     "started_at":  <string>,
//!     "ended_at":    <string|null>,
//!     "events": [
//!       { "id": <i64>, "seq": <i64>, "kind": <string>,
//!         "payload": <string|null>, "created_at": <string> }
//!     ]
//!   }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "run", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "run show: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "run show: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    const evs = engine.runs.lifecycle.events(d, ctx.allocator, run.id) catch |e|
        exit.die(ctx, e, "run show: fetching events: {s}", .{@errorName(e)});
    defer engine.runs.lifecycle.deinitEvents(evs, ctx.allocator);

    if (args.json) {
        try emitJSON(ctx, run, evs);
    } else {
        try emitText(ctx, run, evs);
    }
}

// -------------------------------------------------------------------------
// JSON emission — hand-rolled for stable wire shape.
// -------------------------------------------------------------------------

fn emitJSON(
    ctx: *const runtime.Ctx,
    run: engine.runs.lifecycle.Run,
    evs: []const engine.runs.lifecycle.Event,
) !void {
    const w = ctx.stdout;
    try w.print("{{", .{});
    try w.print("\"id\":{d}", .{run.id});
    try w.print(",\"run_uid\":", .{});
    try output.writeJsonString(w, run.run_uid);
    try w.print(",\"plan_id\":{d}", .{run.plan_id});
    try w.print(",\"arm\":", .{});
    try output.writeJsonString(w, run.arm);
    try w.print(",\"status\":", .{});
    try output.writeJsonString(w, run.status);
    try w.print(",\"started_at\":", .{});
    try output.writeJsonString(w, run.started_at);
    if (run.ended_at) |s| {
        try w.print(",\"ended_at\":", .{});
        try output.writeJsonString(w, s);
    } else {
        try w.print(",\"ended_at\":null", .{});
    }

    // Events array.
    try w.print(",\"events\":[", .{});
    for (evs, 0..) |ev, i| {
        if (i > 0) try w.print(",", .{});
        try w.print("{{\"id\":{d},\"seq\":{d},\"kind\":", .{ ev.id, ev.seq });
        try output.writeJsonString(w, ev.kind);
        if (ev.payload) |p| {
            // payload is a JSON blob stored verbatim; embed it directly.
            try w.print(",\"payload\":{s}", .{p});
        } else {
            try w.print(",\"payload\":null", .{});
        }
        try w.print(",\"created_at\":", .{});
        try output.writeJsonString(w, ev.created_at);
        try w.print("}}", .{});
    }
    try w.print("]", .{});

    try w.print("}}\n", .{});
}

// -------------------------------------------------------------------------
// Text emission — human-readable summary for interactive use.
// -------------------------------------------------------------------------

fn emitText(
    ctx: *const runtime.Ctx,
    run: engine.runs.lifecycle.Run,
    evs: []const engine.runs.lifecycle.Event,
) !void {
    const w = ctx.stdout;
    try w.print("run:        {s}\n", .{run.run_uid});
    try w.print("plan_id:    {d}\n", .{run.plan_id});
    try w.print("arm:        {s}\n", .{run.arm});
    try w.print("status:     {s}\n", .{run.status});
    try w.print("started_at: {s}\n", .{run.started_at});
    if (run.ended_at) |s| try w.print("ended_at:   {s}\n", .{s});
    try w.print("\nevents ({d}):\n", .{evs.len});
    for (evs) |ev| {
        if (ev.payload) |p| {
            try w.print("  [{d}] {s}: {s}\n", .{ ev.seq, ev.kind, p });
        } else {
            try w.print("  [{d}] {s}\n", .{ ev.seq, ev.kind });
        }
    }
}
