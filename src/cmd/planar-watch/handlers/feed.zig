//! handlers/feed — `planar-watch feed [--follow] [--vendor v] [--plan id] [--task id] [--since S] [--limit N] [--json] [--interval D]`
//!
//! Cross-cutting activity feed: one line per claim transition, action
//! transition, or task status change, ordered by occurrence time.
//! Default invocation of the binary; `planar-watch` with no args
//! shells out to `planar-watch feed`.
//!
//! JSON shape (tech-spec § "JSON shapes"): NDJSON, one object per line.
//!   { event: "claim_acquired" | "heartbeat" | "released" | "stale" |
//!            "completed" | "failed" | "blocked" |
//!            "action_started" | "action_ended" | "task_status_changed",
//!     at: ISO8601,
//!     claim?: ClaimRow,
//!     action?: ActionRow,
//!     task?: { id, status_before, status_after } }
//!
//! Watermark: max of agent_work_claims.claimed_at / last_heartbeat_at /
//! released_at and agent_actions.started_at / ended_at. Each follow
//! iteration selects rows strictly newer than the previous watermark.
//!
//! M8 ships the Tier-1 poll path; M9 swaps in kqueue/inotify on the
//! SQLite `-wal` file without changing the public contract.

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");
const follow = @import("follow.zig");
const planfilter = @import("planfilter.zig");
const ps = @import("ps.zig");

const agentactivity = engine.runtime.agentactivity;

pub const verb: cli.Cmd = .{
    .name = "feed",
    .desc = "Cross-cutting activity feed across all vendors (default verb).",
    .long_desc = "One event per claim transition, action transition, or task status\n" ++
        "  change, in occurrence-time order. The default planar-watch\n" ++
        "  invocation routes here.\n\n" ++
        "  Without --follow: print the initial snapshot up to --limit\n" ++
        "  events (default 100), newest first.\n" ++
        "  With --follow: print the snapshot, then stream new events as\n" ++
        "  they appear. Tier-1 poll; --interval defaults to 1s.\n\n" ++
        "  Filters (--vendor / --plan / --task / --since) narrow both the\n" ++
        "  snapshot and the streaming view.\n\n" ++
        "  --json emits NDJSON — one JSON object per line, no surrounding\n" ++
        "  array, no trailing comma. Consumers can pipe through `jq -c`.",
    .flags = &.{
        .{ .long = "--follow", .kind = .bool, .default = .{ .bool = false }, .desc = "Stream new events until SIGINT" },
        .{ .long = "--vendor", .kind = .string, .desc = "Vendor filter" },
        .{ .long = "--plan", .kind = .int, .desc = "Plan id filter (matches plan-direct, task-on-plan, and plan_step-on-plan events)" },
        .{ .long = "--task", .kind = .int, .desc = "Task id filter" },
        .{ .long = "--since", .kind = .string, .desc = "Only events with at >= this ISO8601 timestamp" },
        .{ .long = "--limit", .kind = .int, .desc = "Snapshot row cap (default 100)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false }, .desc = "Emit NDJSON" },
        .{ .long = "--interval", .kind = .string, .desc = "Poll interval for --follow (default 1s)" },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"feed"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    const interval_ns = ps.parseIntervalOrDefault(args.interval);
    if (args.follow) follow.installSigintHandler();

    // Watermark starts at --since (if supplied) else at 1970-01-01
    // so the initial snapshot pulls everything up to --limit.
    var watermark_buf: [64]u8 = undefined;
    var watermark: []const u8 = blk: {
        if (args.since) |s| {
            const len = @min(s.len, watermark_buf.len);
            @memcpy(watermark_buf[0..len], s[0..len]);
            break :blk watermark_buf[0..len];
        }
        const empty = "1970-01-01T00:00:00.000Z";
        @memcpy(watermark_buf[0..empty.len], empty);
        break :blk watermark_buf[0..empty.len];
    };

    const initial_limit = if (args.limit) |n| n else 100;

    // Initial snapshot — most-recent N events. We render them in
    // chronological order so streaming continues naturally.
    {
        const snap = try collectSnapshot(d, ctx.allocator, args, initial_limit);
        defer freeEvents(ctx.allocator, snap);
        if (snap.len > 0) {
            for (snap) |ev| try writeEvent(ctx.stdout, args.json, ev);
            // Advance watermark to the LATEST event in the snapshot
            // so the streaming pass picks up only NEW rows.
            const last = snap[snap.len - 1].at;
            const len = @min(last.len, watermark_buf.len);
            @memcpy(watermark_buf[0..len], last[0..len]);
            watermark = watermark_buf[0..len];
        }
        try ctx.stdout.flush();
    }

    if (!args.follow) return;

    while (true) {
        if (follow.shouldStop()) return;
        follow.interruptibleSleep(interval_ns);
        if (follow.shouldStop()) return;

        const incremental = try collectSince(d, ctx.allocator, args, watermark);
        defer freeEvents(ctx.allocator, incremental);
        for (incremental) |ev| try writeEvent(ctx.stdout, args.json, ev);
        try ctx.stdout.flush();
        if (incremental.len > 0) {
            const last = incremental[incremental.len - 1].at;
            const len = @min(last.len, watermark_buf.len);
            @memcpy(watermark_buf[0..len], last[0..len]);
            watermark = watermark_buf[0..len];
        }
    }
}

// =====================================================================
// Event shape
// =====================================================================

const Event = struct {
    at: []const u8,
    kind: []const u8,
    // At most one of claim / action is non-null. Both null only for
    // task_status_changed events (M8 doesn't synthesize those yet —
    // they require a separate audit log channel not in scope here).
    claim: ?agentactivity.types.Claim,
    action: ?agentactivity.types.Action,

    fn deinit(self: Event, allocator: std.mem.Allocator) void {
        allocator.free(self.at);
        if (self.claim) |c| c.deinit(allocator);
        if (self.action) |a| a.deinit(allocator);
    }
};

fn freeEvents(allocator: std.mem.Allocator, events: []const Event) void {
    for (events) |e| e.deinit(allocator);
    allocator.free(events);
}

fn writeEvent(w: *std.Io.Writer, json: bool, ev: Event) !void {
    if (json) {
        try w.print("{{\"event\":", .{});
        try std.json.Stringify.encodeJsonString(ev.kind, .{}, w);
        try w.print(",\"at\":", .{});
        try std.json.Stringify.encodeJsonString(ev.at, .{}, w);
        if (ev.claim) |c| {
            try w.print(",\"claim\":", .{});
            try agentactivity.json.writeClaim(w, c);
        }
        if (ev.action) |a| {
            try w.print(",\"action\":", .{});
            try agentactivity.json.writeAction(w, a);
        }
        try w.print("}}\n", .{});
    } else {
        // Compact human-readable line.
        if (ev.claim) |c| {
            try w.print(
                "  {s}  {s}  vendor:{s}  {s}:{d}  token:{s}\n",
                .{ ev.at, ev.kind, c.vendor, c.entity_kind.toText(), c.entity_id, c.claim_token },
            );
        } else if (ev.action) |a| {
            const ek: []const u8 = if (a.entity_kind) |k| k.toText() else "-";
            const eid: i64 = a.entity_id orelse 0;
            try w.print(
                "  {s}  {s}  vendor:{s}  kind:{s}  entity:{s}:{d}\n",
                .{ ev.at, ev.kind, a.vendor, a.action_kind.toText(), ek, eid },
            );
        } else {
            try w.print("  {s}  {s}\n", .{ ev.at, ev.kind });
        }
    }
}

// =====================================================================
// Event sources
// =====================================================================

fn collectSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
    limit: i64,
) ![]Event {
    return try collectBetween(d, allocator, args, "1970-01-01T00:00:00.000Z", limit, true);
}

fn collectSince(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
    after: []const u8,
) ![]Event {
    // No cap on incremental — we want every new event since the
    // watermark. Use a generous limit to bound a runaway burst.
    return try collectBetween(d, allocator, args, after, 1024, false);
}

fn collectBetween(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
    after: []const u8,
    limit: i64,
    is_snapshot: bool,
) ![]Event {
    var out: std.ArrayList(Event) = .empty;
    errdefer {
        for (out.items) |e| e.deinit(allocator);
        out.deinit(allocator);
    }

    // Wrap both queries in a single read transaction so they share a
    // consistent SQLite snapshot. Without the wrap, each statement
    // gets its own implicit read txn and a writer can commit between
    // them — that race surfaces as "action row visible but matching
    // claim row not visible" output, breaking the cross-process
    // invariant the Tier-2 wake test pins. The COMMIT here is just
    // a release of the read mark; readers don't write anything.
    //
    // Best-effort: a BEGIN failure leaves us in the legacy per-statement
    // mode rather than refusing service.
    const have_tx = blk: {
        d.exec("BEGIN DEFERRED;") catch break :blk false;
        break :blk true;
    };
    defer if (have_tx) {
        d.exec("COMMIT;") catch {};
    };

    // Claim events: each row contributes one acquire event and (if
    // not active) one terminal event. We emit both within the SQL
    // pass to keep the event count bounded. The args struct carries
    // the in-flight --vendor / --plan / --task / --since filters;
    // event collectors apply them in-loop.
    try collectClaimEvents(d, allocator, &out, after, limit, args);
    try collectActionEvents(d, allocator, &out, after, limit, args);

    // Sort by .at ascending (oldest first for streaming).
    std.mem.sort(Event, out.items, {}, Event_lessThan);

    // For the snapshot path, keep only the LAST `limit` events
    // (most recent) so the user sees the freshest window.
    if (is_snapshot and @as(i64, @intCast(out.items.len)) > limit) {
        const trim_n = @as(usize, @intCast(@as(i64, @intCast(out.items.len)) - limit));
        for (out.items[0..trim_n]) |e| e.deinit(allocator);
        std.mem.copyForwards(Event, out.items[0..@as(usize, @intCast(limit))], out.items[trim_n..]);
        out.shrinkRetainingCapacity(@as(usize, @intCast(limit)));
    }

    return try out.toOwnedSlice(allocator);
}

fn Event_lessThan(_: void, a: Event, b: Event) bool {
    return std.mem.lessThan(u8, a.at, b.at);
}

fn collectClaimEvents(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    out: *std.ArrayList(Event),
    after: []const u8,
    limit: i64,
    args: anytype,
) !void {
    var sql_buf: [1024]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where claimed_at > ? or coalesce(released_at, '') > ? or last_heartbeat_at > ?
        \\order by claimed_at desc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = after }, .{ .text = after }, .{ .text = after },
    }) catch return error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const c = try readClaimRow(&stmt, allocator);
                if (!claimMatches(d, c, args)) {
                    c.deinit(allocator);
                    continue;
                }
                // Emit acquire event if claimed_at > after.
                if (std.mem.lessThan(u8, after, c.claimed_at)) {
                    var c_copy = try cloneClaim(c, allocator);
                    try out.append(allocator, .{
                        .at = try allocator.dupe(u8, c.claimed_at),
                        .kind = "claim_acquired",
                        .claim = c_copy,
                        .action = null,
                    });
                    _ = &c_copy;
                }
                // Emit terminal event if released_at > after AND status != active.
                if (c.status != .active and c.released_at != null) {
                    const r = c.released_at.?;
                    if (std.mem.lessThan(u8, after, r)) {
                        const kind: []const u8 = switch (c.status) {
                            .released => "released",
                            .completed => "completed",
                            .aborted => "failed",
                            .stale => "stale",
                            .active => unreachable,
                        };
                        const c_copy = try cloneClaim(c, allocator);
                        try out.append(allocator, .{
                            .at = try allocator.dupe(u8, r),
                            .kind = kind,
                            .claim = c_copy,
                            .action = null,
                        });
                    }
                }
                c.deinit(allocator);
            },
        }
    }
}

fn claimMatches(d: *db.sqlite.Db, c: agentactivity.types.Claim, args: anytype) bool {
    if (args.vendor) |v| {
        if (!std.mem.eql(u8, c.vendor, v)) return false;
    }
    if (args.task) |tid| {
        if (c.entity_kind != .task or c.entity_id != tid) return false;
    }
    if (args.plan) |pid| {
        // Widened --plan matches plan-direct AND task-on-plan AND
        // plan_step-on-plan rows.
        if (!planfilter.claimBelongsToPlan(d, c.entity_kind, c.entity_id, pid)) return false;
    }
    return true;
}

fn actionMatches(d: *db.sqlite.Db, a: agentactivity.types.Action, args: anytype) bool {
    if (args.vendor) |v| {
        if (!std.mem.eql(u8, a.vendor, v)) return false;
    }
    if (args.task) |tid| {
        const k = a.entity_kind orelse return false;
        if (k != .task) return false;
        const id = a.entity_id orelse return false;
        if (id != tid) return false;
    }
    if (args.plan) |pid| {
        const k = a.entity_kind orelse return false;
        const id = a.entity_id orelse return false;
        if (!planfilter.actionBelongsToPlan(d, k, id, pid)) return false;
    }
    return true;
}

fn collectActionEvents(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    out: *std.ArrayList(Event),
    after: []const u8,
    limit: i64,
    args: anytype,
) !void {
    var sql_buf: [1024]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, session_id, session_entry_id, parent_action_id, claim_id,
        \\       action_kind, entity_kind, entity_id,
        \\       vendor, vendor_role, model,
        \\       started_at, ended_at, outcome, summary,
        \\       head_sha, dirty
        \\from agent_actions
        \\where started_at > ? or coalesce(ended_at, '') > ?
        \\order by started_at desc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .text = after }, .{ .text = after },
    }) catch return error.QueryFailed;
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => {
                const a = try readActionRow(&stmt, allocator);
                if (!actionMatches(d, a, args)) {
                    a.deinit(allocator);
                    continue;
                }
                // started_at always advances.
                if (std.mem.lessThan(u8, after, a.started_at)) {
                    const a_copy = try cloneAction(a, allocator);
                    try out.append(allocator, .{
                        .at = try allocator.dupe(u8, a.started_at),
                        .kind = "action_started",
                        .claim = null,
                        .action = a_copy,
                    });
                }
                if (a.ended_at) |ea| {
                    if (std.mem.lessThan(u8, after, ea)) {
                        const a_copy = try cloneAction(a, allocator);
                        try out.append(allocator, .{
                            .at = try allocator.dupe(u8, ea),
                            .kind = "action_ended",
                            .claim = null,
                            .action = a_copy,
                        });
                    }
                }
                a.deinit(allocator);
            },
        }
    }
}

// =====================================================================
// Row reader + clone helpers (cloning lets us own one copy per event;
// the source row's deinit then releases the read buffer).
// =====================================================================

fn readClaimRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !agentactivity.types.Claim {
    const types = agentactivity.types;

    const kind_text = try stmt.columnTextAlloc(3, allocator);
    defer allocator.free(kind_text);
    const kind = types.EntityKind.fromText(kind_text) orelse return error.QueryFailed;

    const scope_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(scope_text);
    const scope = types.ClaimScope.fromText(scope_text) orelse return error.QueryFailed;

    const status_text = try stmt.columnTextAlloc(6, allocator);
    defer allocator.free(status_text);
    const status = types.ClaimStatus.fromText(status_text) orelse return error.QueryFailed;

    const dirty_opt = try stmt.columnTextOpt(16, allocator);
    var dirty: ?types.Dirty = null;
    if (dirty_opt) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .claim_token = try stmt.columnTextAlloc(1, allocator),
        .session_id = stmt.columnInt(2),
        .entity_kind = kind,
        .entity_id = stmt.columnInt(4),
        .claim_scope = scope,
        .status = status,
        .vendor = try stmt.columnTextAlloc(7, allocator),
        .vendor_session_id = try stmt.columnTextOpt(8, allocator),
        .role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .worktree_id = stmt.columnIntOpt(11),
        .worktree_path = try stmt.columnTextOpt(12, allocator),
        .repo_root = try stmt.columnTextOpt(13, allocator),
        .branch = try stmt.columnTextOpt(14, allocator),
        .head_sha_at_claim = try stmt.columnTextOpt(15, allocator),
        .dirty_at_claim = dirty,
        .purpose = try stmt.columnTextOpt(17, allocator),
        .base_ref = try stmt.columnTextOpt(18, allocator),
        .claimed_at = try stmt.columnTextAlloc(19, allocator),
        .last_heartbeat_at = try stmt.columnTextAlloc(20, allocator),
        .lease_expires_at = try stmt.columnTextAlloc(21, allocator),
        .released_at = try stmt.columnTextOpt(22, allocator),
        .release_reason = try stmt.columnTextOpt(23, allocator),
    };
}

fn readActionRow(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) !agentactivity.types.Action {
    const types = agentactivity.types;

    const kind_text = try stmt.columnTextAlloc(5, allocator);
    defer allocator.free(kind_text);
    const kind = types.ActionKind.fromText(kind_text) orelse return error.QueryFailed;

    var ent_kind: ?types.ActionEntityKind = null;
    if (try stmt.columnTextOpt(6, allocator)) |ek_text| {
        defer allocator.free(ek_text);
        ent_kind = types.ActionEntityKind.fromText(ek_text);
    }

    var outcome: ?types.Outcome = null;
    if (try stmt.columnTextOpt(13, allocator)) |o_text| {
        defer allocator.free(o_text);
        outcome = types.Outcome.fromText(o_text);
    }

    var dirty: ?types.Dirty = null;
    if (try stmt.columnTextOpt(16, allocator)) |d_text| {
        defer allocator.free(d_text);
        dirty = types.Dirty.fromText(d_text);
    }

    return .{
        .id = stmt.columnInt(0),
        .session_id = stmt.columnInt(1),
        .session_entry_id = stmt.columnIntOpt(2),
        .parent_action_id = stmt.columnIntOpt(3),
        .claim_id = stmt.columnIntOpt(4),
        .action_kind = kind,
        .entity_kind = ent_kind,
        .entity_id = stmt.columnIntOpt(7),
        .vendor = try stmt.columnTextAlloc(8, allocator),
        .vendor_role = try stmt.columnTextOpt(9, allocator),
        .model = try stmt.columnTextOpt(10, allocator),
        .started_at = try stmt.columnTextAlloc(11, allocator),
        .ended_at = try stmt.columnTextOpt(12, allocator),
        .outcome = outcome,
        .summary = try stmt.columnTextOpt(14, allocator),
        .head_sha = try stmt.columnTextOpt(15, allocator),
        .dirty = dirty,
    };
}

fn cloneClaim(c: agentactivity.types.Claim, a: std.mem.Allocator) !agentactivity.types.Claim {
    return .{
        .id = c.id,
        .claim_token = try a.dupe(u8, c.claim_token),
        .session_id = c.session_id,
        .entity_kind = c.entity_kind,
        .entity_id = c.entity_id,
        .claim_scope = c.claim_scope,
        .status = c.status,
        .vendor = try a.dupe(u8, c.vendor),
        .vendor_session_id = try dupeOpt(a, c.vendor_session_id),
        .role = try dupeOpt(a, c.role),
        .model = try dupeOpt(a, c.model),
        .worktree_id = c.worktree_id,
        .worktree_path = try dupeOpt(a, c.worktree_path),
        .repo_root = try dupeOpt(a, c.repo_root),
        .branch = try dupeOpt(a, c.branch),
        .head_sha_at_claim = try dupeOpt(a, c.head_sha_at_claim),
        .dirty_at_claim = c.dirty_at_claim,
        .purpose = try dupeOpt(a, c.purpose),
        .base_ref = try dupeOpt(a, c.base_ref),
        .claimed_at = try a.dupe(u8, c.claimed_at),
        .last_heartbeat_at = try a.dupe(u8, c.last_heartbeat_at),
        .lease_expires_at = try a.dupe(u8, c.lease_expires_at),
        .released_at = try dupeOpt(a, c.released_at),
        .release_reason = try dupeOpt(a, c.release_reason),
    };
}

fn cloneAction(x: agentactivity.types.Action, a: std.mem.Allocator) !agentactivity.types.Action {
    return .{
        .id = x.id,
        .session_id = x.session_id,
        .session_entry_id = x.session_entry_id,
        .parent_action_id = x.parent_action_id,
        .claim_id = x.claim_id,
        .action_kind = x.action_kind,
        .entity_kind = x.entity_kind,
        .entity_id = x.entity_id,
        .vendor = try a.dupe(u8, x.vendor),
        .vendor_role = try dupeOpt(a, x.vendor_role),
        .model = try dupeOpt(a, x.model),
        .started_at = try a.dupe(u8, x.started_at),
        .ended_at = try dupeOpt(a, x.ended_at),
        .outcome = x.outcome,
        .summary = try dupeOpt(a, x.summary),
        .head_sha = try dupeOpt(a, x.head_sha),
        .dirty = x.dirty,
    };
}

fn dupeOpt(allocator: std.mem.Allocator, s: ?[]const u8) !?[]const u8 {
    if (s) |v| return try allocator.dupe(u8, v);
    return null;
}
