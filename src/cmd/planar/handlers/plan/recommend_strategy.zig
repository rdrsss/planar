//! handlers/plan/recommend-strategy — `planar plan recommend-strategy
//!     <plan-id> [--json]`
//!
//! READ-ONLY operator surface for the parallelizability-rules engine
//! (decision 370). Computes the parallel-eligible subset of a plan's
//! open (todo) tasks by applying the six parallel-eligibility rules and
//! emits the eligible subset + the serialized remainder with per-task
//! exclusion reasons. This is the single source of truth consumed by
//! centurion's fan-out gate (M5 task 3185) and the orchestrator
//! skill — the rules are NOT re-derived in centurion.
//!
//! JSON shape:
//!   { "plan_id": int,
//!     "parallel_eligible": [ { "id": int, "slug": str|null, "title": str } ],
//!     "serialized": [ { "id": int, "slug": str|null, "title": str,
//!                       "excluded_by": [ { "rule": int, "reason": str } ] } ],
//!     "summary": { "open_tasks": int, "eligible": int, "serialized": int,
//!                  "fan_out_available": bool },
//!     "recommended_note": str }

const std = @import("std");
const cli = @import("cli");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const strategy = engine.planning.strategy;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "recommend-strategy" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    const rec = strategy.recommend(d, ctx.allocator, plan_id) catch |e| switch (e) {
        error.NotFound => exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id}),
        else => exit.die(ctx, e, "recommend-strategy: {s}", .{@errorName(e)}),
    };
    defer rec.deinit(ctx.allocator);

    if (args.json) {
        try writeJson(ctx.stdout, rec);
    } else {
        try writeText(ctx.stdout, rec);
    }
}

fn writeJson(w: *std.Io.Writer, rec: strategy.Recommendation) !void {
    try w.print("{{\"plan_id\":{d},\"parallel_eligible\":[", .{rec.plan_id});
    for (rec.parallel_eligible, 0..) |t, i| {
        if (i != 0) try w.print(",", .{});
        try w.print("{{\"id\":{d},\"slug\":", .{t.id});
        try writeOptStr(w, t.slug);
        try w.print(",\"title\":", .{});
        try writeStr(w, t.title);
        try w.print("}}", .{});
    }
    try w.print("],\"serialized\":[", .{});
    for (rec.serialized, 0..) |t, i| {
        if (i != 0) try w.print(",", .{});
        try w.print("{{\"id\":{d},\"slug\":", .{t.id});
        try writeOptStr(w, t.slug);
        try w.print(",\"title\":", .{});
        try writeStr(w, t.title);
        try w.print(",\"excluded_by\":[", .{});
        for (t.excluded_by, 0..) |e, j| {
            if (j != 0) try w.print(",", .{});
            try w.print("{{\"rule\":{d},\"reason\":", .{e.rule});
            try writeStr(w, e.reason);
            try w.print("}}", .{});
        }
        try w.print("]}}", .{});
    }
    try w.print(
        "],\"summary\":{{\"open_tasks\":{d},\"eligible\":{d},\"serialized\":{d},\"fan_out_available\":{s}}},\"recommended_note\":",
        .{
            rec.open_tasks,
            rec.parallel_eligible.len,
            rec.serialized.len,
            if (rec.fan_out_available) "true" else "false",
        },
    );
    var note_buf: [128]u8 = undefined;
    const note = noteText(&note_buf, rec);
    try writeStr(w, note);
    try w.print("}}\n", .{});
}

fn writeText(w: *std.Io.Writer, rec: strategy.Recommendation) !void {
    var note_buf: [128]u8 = undefined;
    const note = noteText(&note_buf, rec);
    try w.print(
        "plan:{d}  open:{d}  eligible:{d}  serialized:{d}  fan_out_available:{s}\n",
        .{
            rec.plan_id,
            rec.open_tasks,
            rec.parallel_eligible.len,
            rec.serialized.len,
            if (rec.fan_out_available) "yes" else "no",
        },
    );
    try w.print("  {s}\n", .{note});

    try w.print("parallel-eligible:\n", .{});
    if (rec.parallel_eligible.len == 0) {
        try w.print("  (none)\n", .{});
    } else {
        for (rec.parallel_eligible) |t| {
            try w.print("  task:{d}  {s}\n", .{ t.id, t.title });
        }
    }

    try w.print("serialized:\n", .{});
    if (rec.serialized.len == 0) {
        try w.print("  (none)\n", .{});
    } else {
        for (rec.serialized) |t| {
            try w.print("  task:{d}  {s}\n", .{ t.id, t.title });
            for (t.excluded_by) |e| {
                try w.print("    - {s}\n", .{e.reason});
            }
        }
    }
}

/// One-line strategy hint matching the verb name. Written into a caller
/// buffer to avoid an allocation.
fn noteText(buf: []u8, rec: strategy.Recommendation) []const u8 {
    if (rec.fan_out_available) {
        return std.fmt.bufPrint(
            buf,
            "parallel-fanout available: {d} eligible tasks",
            .{rec.parallel_eligible.len},
        ) catch "parallel-fanout available";
    }
    if (rec.parallel_eligible.len == 1) {
        return "no fan-out: only 1 eligible task; run sequentially";
    }
    return "no fan-out: no eligible tasks; run sequentially";
}

fn writeStr(w: *std.Io.Writer, s: []const u8) !void {
    try w.print("\"", .{});
    for (s) |ch| {
        switch (ch) {
            '"' => try w.print("\\\"", .{}),
            '\\' => try w.print("\\\\", .{}),
            '\n' => try w.print("\\n", .{}),
            '\r' => try w.print("\\r", .{}),
            '\t' => try w.print("\\t", .{}),
            else => {
                if (ch < 0x20) {
                    try w.print("\\u{x:0>4}", .{ch});
                } else {
                    try w.print("{c}", .{ch});
                }
            },
        }
    }
    try w.print("\"", .{});
}

fn writeOptStr(w: *std.Io.Writer, s: ?[]const u8) !void {
    if (s) |v| {
        try writeStr(w, v);
    } else {
        try w.print("null", .{});
    }
}
