//! handlers/plan/next — `planar plan next <plan-id>
//!     [--include-claimed] [--include-stale] [--json]`
//!
//! Bucketed "what's next on this plan" view. Same underlying query as
//! `planar-agent peek` (claim-aware selector) but returns the FULL
//! bucket breakdown — `available`, `claimed`, `stale`, `blocked` —
//! instead of just picking one row. This is the operator's read
//! surface; `planar-agent peek` / `pull` are the agent's.
//!
//! Flags:
//!   --include-claimed  Include the `claimed` bucket items in the text
//!                      rendering (the JSON shape always carries them
//!                      under `claimed`). Without the flag the text
//!                      output lists only `available` so the operator
//!                      sees their next-pickable items by default.
//!   --include-stale    Same idea for `stale`. Defaults to off so a
//!                      noisy reconcile backlog doesn't drown the
//!                      available bucket in text mode.
//!   --json             Emit JSON instead of text.
//!
//! JSON shape (tech spec § "JSON shapes"):
//!   { plan_id: int,
//!     available: [Task],
//!     claimed:   [{ task: Task, claim: ClaimRow }],
//!     stale:     [{ task: Task, claim: ClaimRow }],
//!     blocked:   [Task],
//!     summary: { available: int, claimed: int, stale: int,
//!                blocked: int, done: int } }

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const main = @import("../../main.zig");
const runtime = @import("runtime");
const exit = @import("../../exit.zig");

const agentactivity = engine.runtime.agentactivity;
const task_mod = engine.planning.task;

pub fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{ "plan", "next" }, args_ptr);
    const ctx = runtime.current();
    const d = try runtime.ensureDb();

    const plan_id = std.fmt.parseInt(i64, args.plan_id, 10) catch
        exit.die(ctx, error.InvalidInput, "plan id must be an integer, got '{s}'", .{args.plan_id});

    // Confirm the plan exists; otherwise NotFound (exit 1).
    {
        var stmt = d.prepare("select count(*) from plans where id = ?") catch |e|
            exit.die(ctx, e, "plan lookup prep: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch |e|
            exit.die(ctx, e, "plan lookup bind: {s}", .{@errorName(e)});
        const n = switch (stmt.step() catch |e| exit.die(ctx, e, "plan lookup step: {s}", .{@errorName(e)})) {
            .done => 0,
            .row => stmt.columnInt(0),
        };
        if (n == 0) exit.die(ctx, error.NotFound, "plan {d} not found", .{plan_id});
    }

    const nw = agentactivity.store.nextWork(d, ctx.allocator, plan_id) catch |e|
        exit.die(ctx, e, "plan next selector: {s}", .{@errorName(e)});
    defer nw.deinit(ctx.allocator);

    // Tally the recursively selected tasks.
    var n_avail: usize = 0;
    var n_claimed: usize = 0;
    var n_stale: usize = 0;
    var n_blocked: usize = 0;
    for (nw.rows) |r| {
        switch (r.bucket) {
            .available => n_avail += 1,
            .claimed => n_claimed += 1,
            .stale => n_stale += 1,
            .blocked => n_blocked += 1,
        }
    }

    // Done count: separate single-int query — `nextWork` filters done
    // tasks out, so we can't read it from the bucket totals.
    var done_count: i64 = 0;
    {
        var stmt = d.prepare(
            \\with recursive plan_tree(id) as (
            \\  select id from plans where id = ?
            \\  union all
            \\  select p.id from plans p join plan_tree pt on p.parent_plan_id = pt.id
            \\)
            \\select count(*) from tasks
            \\where plan_id in (select id from plan_tree) and status = 'done'
        ) catch |e| exit.die(ctx, e, "plan next done count prep: {s}", .{@errorName(e)});
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = plan_id }}) catch |e|
            exit.die(ctx, e, "plan next done count bind: {s}", .{@errorName(e)});
        switch (stmt.step() catch |e| exit.die(ctx, e, "plan next done count step: {s}", .{@errorName(e)})) {
            .done => {},
            .row => done_count = stmt.columnInt(0),
        }
    }

    if (args.json) {
        try writeJson(ctx.stdout, d, ctx.allocator, plan_id, nw.rows, .{
            .avail = n_avail,
            .claimed = n_claimed,
            .stale = n_stale,
            .blocked = n_blocked,
            .done = done_count,
        });
    } else {
        try writeText(ctx.stdout, plan_id, nw.rows, .{
            .include_claimed = args.include_claimed,
            .include_stale = args.include_stale,
        }, .{
            .avail = n_avail,
            .claimed = n_claimed,
            .stale = n_stale,
            .blocked = n_blocked,
            .done = done_count,
        });
    }
}

const Summary = struct {
    avail: usize,
    claimed: usize,
    stale: usize,
    blocked: usize,
    done: i64,
};

const TextOpts = struct {
    include_claimed: bool,
    include_stale: bool,
};

fn writeText(
    w: *std.Io.Writer,
    plan_id: i64,
    rows: []const agentactivity.store.NextWorkRow,
    opts: TextOpts,
    s: Summary,
) !void {
    try w.print(
        "plan:{d}  available:{d}  claimed:{d}  stale:{d}  blocked:{d}  done:{d}\n",
        .{ plan_id, s.avail, s.claimed, s.stale, s.blocked, s.done },
    );
    for (rows) |r| {
        switch (r.bucket) {
            .available => try w.print(
                "  available  task:{d}  {s}  [pri:{d}]\n",
                .{ r.task_id, r.title, r.priority },
            ),
            .claimed => if (opts.include_claimed) {
                const tok = if (r.claim) |c| c.claim_token else "?";
                try w.print(
                    "  claimed    task:{d}  {s}  [pri:{d}, claim:{s}]\n",
                    .{ r.task_id, r.title, r.priority, tok },
                );
            },
            .stale => if (opts.include_stale) {
                const tok = if (r.claim) |c| c.claim_token else "?";
                try w.print(
                    "  stale      task:{d}  {s}  [pri:{d}, claim:{s}]\n",
                    .{ r.task_id, r.title, r.priority, tok },
                );
            },
            .blocked => try w.print(
                "  blocked    task:{d}  {s}  [pri:{d}]\n",
                .{ r.task_id, r.title, r.priority },
            ),
        }
    }
}

fn writeJson(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    plan_id: i64,
    rows: []const agentactivity.store.NextWorkRow,
    s: Summary,
) !void {
    try w.print("{{\"plan_id\":{d}", .{plan_id});

    // available: [Task]
    try w.print(",\"available\":[", .{});
    var first = true;
    for (rows) |r| {
        if (r.bucket != .available) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const task = task_mod.show(d, allocator, r.task_id) catch |e| return mapTaskErr(e);
        defer task_mod.deinit(task, allocator);
        try agentactivity.json.writeTask(w, task);
    }
    try w.print("]", .{});

    // claimed: [{ task: Task, claim: ClaimRow }]
    try w.print(",\"claimed\":[", .{});
    first = true;
    for (rows) |r| {
        if (r.bucket != .claimed) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const task = task_mod.show(d, allocator, r.task_id) catch |e| return mapTaskErr(e);
        defer task_mod.deinit(task, allocator);
        try w.print("{{\"task\":", .{});
        try agentactivity.json.writeTask(w, task);
        try w.print(",\"claim\":", .{});
        if (r.claim) |c| {
            const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
            defer scope.deinit(allocator);
            try agentactivity.json.writeClaim(w, c, scope);
        } else {
            try w.print("null", .{});
        }
        try w.print("}}", .{});
    }
    try w.print("]", .{});

    // stale: [{ task: Task, claim: ClaimRow }]
    try w.print(",\"stale\":[", .{});
    first = true;
    for (rows) |r| {
        if (r.bucket != .stale) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const task = task_mod.show(d, allocator, r.task_id) catch |e| return mapTaskErr(e);
        defer task_mod.deinit(task, allocator);
        try w.print("{{\"task\":", .{});
        try agentactivity.json.writeTask(w, task);
        try w.print(",\"claim\":", .{});
        if (r.claim) |c| {
            const scope = agentactivity.store.resolveClaimScope(d, allocator, c);
            defer scope.deinit(allocator);
            try agentactivity.json.writeClaim(w, c, scope);
        } else {
            try w.print("null", .{});
        }
        try w.print("}}", .{});
    }
    try w.print("]", .{});

    // blocked: [Task]
    try w.print(",\"blocked\":[", .{});
    first = true;
    for (rows) |r| {
        if (r.bucket != .blocked) continue;
        if (!first) try w.print(",", .{});
        first = false;
        const task = task_mod.show(d, allocator, r.task_id) catch |e| return mapTaskErr(e);
        defer task_mod.deinit(task, allocator);
        try agentactivity.json.writeTask(w, task);
    }
    try w.print("]", .{});

    try w.print(
        ",\"summary\":{{\"available\":{d},\"claimed\":{d},\"stale\":{d},\"blocked\":{d},\"done\":{d}",
        .{ s.avail, s.claimed, s.stale, s.blocked, s.done },
    );
    try w.print("}}}}\n", .{});
}

fn mapTaskErr(e: anyerror) anyerror {
    return e;
}
