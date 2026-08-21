//! handlers/bench/show — `planar bench show <run-uid> [--json]`
//!
//! Shows a run's full state: header row (from `runs`), journal events
//! (from `run_events`, ordered by seq), and touch records (from
//! `run_touches`, ordered by id).
//!
//! JSON shape (--json):
//!
//!   {
//!     "id":          <i64>,
//!     "run_uid":     <string>,
//!     "plan_id":     <i64>,
//!     "arm":         <string>,
//!     "base_sha":    <string>,
//!     "config_hash": <string>,
//!     "config_json": <string|null>,
//!     "corpus_repo": <string|null>,
//!     "status":      <string>,
//!     "started_at":  <string>,
//!     "ended_at":    <string|null>,
//!     "events": [
//!       { "id": <i64>, "seq": <i64>, "kind": <string>,
//!         "payload": <string|null>, "created_at": <string> }
//!     ],
//!     "touches": [
//!       { "id": <i64>, "task_id": <i64>, "path": <string>,
//!         "kind": <string>, "created_at": <string> }
//!     ]
//!   }
//!
//! Text output (no --json): human-readable summary.

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");
const output = @import("../../output.zig");

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "bench", "show" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const run = engine.runs.lifecycle.showByUid(d, ctx.allocator, args.run_uid) catch |e| switch (e) {
        error.NotFound => exit.die(
            ctx,
            error.NotFound,
            "bench show: run '{s}' not found",
            .{args.run_uid},
        ),
        else => exit.die(ctx, e, "bench show: {s}", .{@errorName(e)}),
    };
    defer engine.runs.lifecycle.deinit(run, ctx.allocator);

    const evs = engine.runs.lifecycle.events(d, ctx.allocator, run.id) catch |e|
        exit.die(ctx, e, "bench show: fetching events: {s}", .{@errorName(e)});
    defer engine.runs.lifecycle.deinitEvents(evs, ctx.allocator);

    const touches = engine.runs.lifecycle.touches(d, ctx.allocator, run.id, null) catch |e|
        exit.die(ctx, e, "bench show: fetching touches: {s}", .{@errorName(e)});
    defer engine.runs.lifecycle.deinitTouches(touches, ctx.allocator);

    if (args.json) {
        try emitJSON(ctx, run, evs, touches);
    } else {
        try emitText(ctx, run, evs, touches);
    }
}

// -------------------------------------------------------------------------
// JSON emission — hand-rolled to guarantee a stable wire shape that
// mustRunJSON can parse even when std.json.Stringify changes field
// ordering or optional handling.
// -------------------------------------------------------------------------

fn emitJSON(
    ctx: *const runtime.Ctx,
    run: engine.runs.lifecycle.Run,
    evs: []const engine.runs.lifecycle.Event,
    touches: []const engine.runs.lifecycle.Touch,
) !void {
    const w = ctx.stdout;
    try w.print("{{", .{});
    try w.print("\"id\":{d}", .{run.id});
    try w.print(",\"run_uid\":", .{});
    try output.writeJsonString(w, run.run_uid);
    try w.print(",\"plan_id\":{d}", .{run.plan_id});
    try w.print(",\"arm\":", .{});
    try output.writeJsonString(w, run.arm);
    try w.print(",\"base_sha\":", .{});
    try output.writeJsonString(w, run.base_sha);
    try w.print(",\"config_hash\":", .{});
    try output.writeJsonString(w, run.config_hash);
    if (run.config_json) |s| {
        // config_json is itself a JSON blob — embed it verbatim.
        try w.print(",\"config_json\":{s}", .{s});
    } else {
        try w.print(",\"config_json\":null", .{});
    }
    if (run.corpus_repo) |s| {
        try w.print(",\"corpus_repo\":", .{});
        try output.writeJsonString(w, s);
    } else {
        try w.print(",\"corpus_repo\":null", .{});
    }
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
            try w.print(",\"payload\":", .{});
            // payload is a JSON blob stored verbatim; embed it directly.
            try w.print("{s}", .{p});
        } else {
            try w.print(",\"payload\":null", .{});
        }
        try w.print(",\"created_at\":", .{});
        try output.writeJsonString(w, ev.created_at);
        try w.print("}}", .{});
    }
    try w.print("]", .{});

    // Touches array.
    try w.print(",\"touches\":[", .{});
    for (touches, 0..) |t, i| {
        if (i > 0) try w.print(",", .{});
        try w.print("{{\"id\":{d},\"task_id\":{d},\"path\":", .{ t.id, t.task_id });
        try output.writeJsonString(w, t.path);
        try w.print(",\"kind\":", .{});
        try output.writeJsonString(w, @tagName(t.kind));
        try w.print(",\"created_at\":", .{});
        try output.writeJsonString(w, t.created_at);
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
    touches: []const engine.runs.lifecycle.Touch,
) !void {
    const w = ctx.stdout;
    try w.print("run:         {s}\n", .{run.run_uid});
    try w.print("plan_id:     {d}\n", .{run.plan_id});
    try w.print("arm:         {s}\n", .{run.arm});
    try w.print("status:      {s}\n", .{run.status});
    try w.print("base_sha:    {s}\n", .{run.base_sha});
    try w.print("config_hash: {s}\n", .{run.config_hash});
    if (run.corpus_repo) |s| try w.print("corpus_repo: {s}\n", .{s});
    try w.print("started_at:  {s}\n", .{run.started_at});
    if (run.ended_at) |s| try w.print("ended_at:    {s}\n", .{s});
    try w.print("\nevents ({d}):\n", .{evs.len});
    for (evs) |ev| {
        if (ev.payload) |p| {
            try w.print("  [{d}] {s}: {s}\n", .{ ev.seq, ev.kind, p });
        } else {
            try w.print("  [{d}] {s}\n", .{ ev.seq, ev.kind });
        }
    }
    try w.print("\ntouches ({d}):\n", .{touches.len});
    for (touches) |t| {
        try w.print("  task={d} path={s} kind={s}\n", .{ t.task_id, t.path, @tagName(t.kind) });
    }
}
