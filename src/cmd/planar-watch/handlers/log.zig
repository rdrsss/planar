//! handlers/log — `planar-watch log (--task N | --plan N | --entity kind:N | --session N | --claim TOKEN) [--limit N] [--json]`
//!
//! JSON shape (tech-spec § "JSON shapes"):
//!   { entity: { kind, id }, entries: [LogEntry] }
//!
//! LogEntry is the union of action rows and claim transition rows,
//! discriminated by `.kind`:
//!   - .kind = "action"        — full ActionRow payload.
//!   - .kind = "claim_*"       — claim_acquired / claim_heartbeat /
//!                                claim_released / claim_stale, with the
//!                                ClaimRow payload.
//!
//! The five filters are mutually exclusive — exactly one must be set
//! (parser rejects zero or more than one).

const std = @import("std");
const cli = @import("cli");
const db = @import("db");
const engine = @import("engine");
const runtime = @import("runtime");

const main = @import("../main.zig");
const exit = @import("../exit.zig");

const agentactivity = engine.runtime.agentactivity;

pub const verb: cli.Cmd = .{
    .name = "log",
    .desc = "Per-entity / per-claim history (union of agent actions and claim transitions).",
    .long_desc = "Streams the agent_actions + agent_work_claims history scoped to\n" ++
        "  one entity or one claim_token. Exactly one of\n" ++
        "  --task / --plan / --entity / --session / --claim is required.\n\n" ++
        "  Entries are emitted in occurrence-time order (oldest first)\n" ++
        "  as a discriminated union: each entry carries a `.kind` field\n" ++
        "  that is either `action` (full ActionRow payload) or\n" ++
        "  `claim_acquired` / `claim_heartbeat` / `claim_released` /\n" ++
        "  `claim_stale` (with ClaimRow payload).",
    .flags = &.{
        .{ .long = "--task", .kind = .int, .desc = "Filter to one task id" },
        .{ .long = "--plan", .kind = .int, .desc = "Filter to one plan id (matches entity_kind=plan rows)" },
        .{ .long = "--entity", .kind = .string, .desc = "Filter to one entity, kind:id form" },
        .{ .long = "--session", .kind = .int, .desc = "Filter to one session_id" },
        .{ .long = "--claim", .kind = .string, .desc = "Filter to one claim_token" },
        .{ .long = "--limit", .kind = .int, .desc = "Row cap (default 100)" },
        .{ .long = "--json", .kind = .bool, .default = .{ .bool = false } },
    },
    .run = cli.handler(handle),
};

fn handle(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(main.root, &.{"log"}, args_ptr);
    const ctx = runtime.current();
    const d = runtime.ensureDbStrictReadOnly() catch |e| exit.die(ctx, e, "{s}", .{@errorName(e)});

    // Exactly-one validation.
    var set_count: u32 = 0;
    if (args.task != null) set_count += 1;
    if (args.plan != null) set_count += 1;
    if (args.entity != null) set_count += 1;
    if (args.session != null) set_count += 1;
    if (args.claim != null) set_count += 1;
    if (set_count != 1) {
        exit.die(ctx, error.InvalidInput, "log: exactly one of --task / --plan / --entity / --session / --claim required (got {d})", .{set_count});
    }

    // Resolve the filter into (entity_kind, entity_id) OR session id OR claim token.
    var entity_kind: ?[]const u8 = null;
    var entity_id: ?i64 = null;
    if (args.task) |t| {
        entity_kind = "task";
        entity_id = t;
    } else if (args.plan) |p| {
        entity_kind = "plan";
        entity_id = p;
    } else if (args.entity) |e| {
        const colon = std.mem.indexOf(u8, e, ":") orelse exit.die(ctx, error.InvalidInput, "log: --entity expects kind:id (got '{s}')", .{e});
        entity_kind = e[0..colon];
        entity_id = std.fmt.parseInt(i64, e[colon + 1 ..], 10) catch
            exit.die(ctx, error.InvalidInput, "log: --entity id is not an integer ('{s}')", .{e});
    }

    const limit = if (args.limit) |n| n else 100;

    emit(ctx.stdout, d, ctx.allocator, args, entity_kind, entity_id, limit) catch |e|
        exit.die(ctx, e, "log: {s}", .{@errorName(e)});
}

fn emit(
    w: *std.Io.Writer,
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: anytype,
    entity_kind: ?[]const u8,
    entity_id: ?i64,
    limit: i64,
) !void {
    // Two-source merge: we read each source's rows separately and
    // merge in occurrence-time order. Each source path applies its
    // own filter shape.
    var actions: []agentactivity.types.Action = &.{};
    var claims: []agentactivity.types.Claim = &.{};
    defer agentactivity.types.Action.deinitMany(actions, allocator);
    defer agentactivity.types.Claim.deinitMany(claims, allocator);

    if (args.claim) |tok| {
        // claim_token filter: ALL rows attached to one specific claim.
        claims = try listClaimsByToken(d, allocator, tok);
        actions = try listActionsByClaimToken(d, allocator, tok, limit);
    } else if (args.session) |sid| {
        claims = try listClaimsBySession(d, allocator, sid);
        actions = try listActionsBySession(d, allocator, sid, limit);
    } else {
        // entity / task / plan filter.
        claims = try listClaimsByEntity(d, allocator, entity_kind.?, entity_id.?);
        actions = try listActionsByEntity(d, allocator, entity_kind.?, entity_id.?, limit);
    }

    if (args.json) {
        try w.print("{{\"entity\":{{", .{});
        if (args.claim) |tok| {
            try w.print("\"kind\":\"claim_token\",\"id\":", .{});
            try std.json.Stringify.encodeJsonString(tok, .{}, w);
        } else if (args.session) |sid| {
            try w.print("\"kind\":\"session\",\"id\":{d}", .{sid});
        } else {
            try w.print("\"kind\":", .{});
            try std.json.Stringify.encodeJsonString(entity_kind.?, .{}, w);
            try w.print(",\"id\":{d}", .{entity_id.?});
        }
        try w.print("}},\"entries\":[", .{});
        try emitMergedJson(w, allocator, actions, claims);
        try w.print("]}}\n", .{});
    } else {
        try emitMergedText(w, actions, claims);
    }
}

// ---------------------------------------------------------------
// Merge + emit
// ---------------------------------------------------------------

fn emitMergedJson(
    w: *std.Io.Writer,
    allocator: std.mem.Allocator,
    actions: []const agentactivity.types.Action,
    claims: []const agentactivity.types.Claim,
) !void {
    // Materialize a flat event list, then sort. Each claim yields one
    // or two events: claim_acquired at claimed_at, then a terminal
    // event at released_at if the claim is not active. The terminal
    // event kind depends on .status:
    //   completed / aborted / released / stale → claim_<status>.
    var events: std.ArrayList(MergedEvent) = .empty;
    defer events.deinit(allocator);

    for (actions, 0..) |_, i| {
        try events.append(allocator, .{
            .at = actions[i].started_at,
            .kind = "action",
            .action_idx = i,
            .claim_idx = null,
        });
    }
    for (claims, 0..) |c, i| {
        try events.append(allocator, .{
            .at = c.claimed_at,
            .kind = "claim_acquired",
            .action_idx = null,
            .claim_idx = i,
        });
        if (c.status != .active) {
            const k: []const u8 = switch (c.status) {
                .released => "claim_released",
                .completed => "claim_completed",
                .aborted => "claim_aborted",
                .stale => "claim_stale",
                .active => unreachable,
            };
            try events.append(allocator, .{
                .at = c.released_at orelse c.last_heartbeat_at,
                .kind = k,
                .action_idx = null,
                .claim_idx = i,
            });
        }
    }

    // Sort by .at ascending (lexicographic on ISO8601 is correct ordering).
    std.mem.sort(MergedEvent, events.items, {}, MergedEvent.lessThan);

    var first = true;
    for (events.items) |ev| {
        if (!first) try w.print(",", .{});
        first = false;
        try w.print("{{\"kind\":", .{});
        try std.json.Stringify.encodeJsonString(ev.kind, .{}, w);
        try w.print(",\"at\":", .{});
        try std.json.Stringify.encodeJsonString(ev.at, .{}, w);
        if (ev.action_idx) |i| {
            try w.print(",\"action\":", .{});
            try agentactivity.json.writeAction(w, actions[i]);
        }
        if (ev.claim_idx) |i| {
            try w.print(",\"claim\":", .{});
            try agentactivity.json.writeClaim(w, claims[i]);
        }
        try w.print("}}", .{});
    }
}

fn emitMergedText(
    w: *std.Io.Writer,
    actions: []const agentactivity.types.Action,
    claims: []const agentactivity.types.Claim,
) !void {
    try w.print("entries: {d} actions + {d} claim rows\n", .{ actions.len, claims.len });
    for (claims) |c| {
        try w.print(
            "  claim:{s}  {s}  vendor:{s}  status:{s}\n",
            .{ c.claim_token, c.claimed_at, c.vendor, c.status.toText() },
        );
    }
    for (actions) |a| {
        try w.print(
            "  action:{d}  {s}  kind:{s}  vendor:{s}\n",
            .{ a.id, a.started_at, a.action_kind.toText(), a.vendor },
        );
    }
}

const MergedEvent = struct {
    at: []const u8,
    kind: []const u8,
    action_idx: ?usize,
    claim_idx: ?usize,

    fn lessThan(_: void, a: MergedEvent, b: MergedEvent) bool {
        return std.mem.lessThan(u8, a.at, b.at);
    }
};

// ---------------------------------------------------------------
// Source queries
// ---------------------------------------------------------------

fn listActionsByEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
    limit: i64,
) ![]agentactivity.types.Action {
    var sql_buf: [768]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, session_id, session_entry_id, parent_action_id, claim_id,
        \\       action_kind, entity_kind, entity_id,
        \\       vendor, vendor_role, model,
        \\       started_at, ended_at, outcome, summary,
        \\       head_sha, dirty
        \\from agent_actions
        \\where entity_kind = ? and entity_id = ?
        \\order by started_at asc, id asc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id } }) catch return error.QueryFailed;

    return try collectActions(&stmt, allocator);
}

fn listActionsBySession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
    limit: i64,
) ![]agentactivity.types.Action {
    var sql_buf: [768]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select id, session_id, session_entry_id, parent_action_id, claim_id,
        \\       action_kind, entity_kind, entity_id,
        \\       vendor, vendor_role, model,
        \\       started_at, ended_at, outcome, summary,
        \\       head_sha, dirty
        \\from agent_actions
        \\where session_id = ?
        \\order by started_at asc, id asc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;
    return try collectActions(&stmt, allocator);
}

fn listActionsByClaimToken(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    token: []const u8,
    limit: i64,
) ![]agentactivity.types.Action {
    var sql_buf: [768]u8 = undefined;
    const sql = std.fmt.bufPrintZ(&sql_buf,
        \\select a.id, a.session_id, a.session_entry_id, a.parent_action_id, a.claim_id,
        \\       a.action_kind, a.entity_kind, a.entity_id,
        \\       a.vendor, a.vendor_role, a.model,
        \\       a.started_at, a.ended_at, a.outcome, a.summary,
        \\       a.head_sha, a.dirty
        \\from agent_actions a
        \\where a.claim_id = (select id from agent_work_claims where claim_token = ?)
        \\order by a.started_at asc, a.id asc
        \\limit {d}
    , .{limit}) catch return error.QueryFailed;

    var stmt = d.prepare(sql) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = token }}) catch return error.QueryFailed;
    return try collectActions(&stmt, allocator);
}

fn collectActions(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) ![]agentactivity.types.Action {
    var out: std.ArrayList(agentactivity.types.Action) = .empty;
    errdefer {
        for (out.items) |a| a.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readActionRow(stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

fn listClaimsByEntity(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    entity_kind: []const u8,
    entity_id: i64,
) ![]agentactivity.types.Claim {
    var stmt = d.prepare(
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where entity_kind = ? and entity_id = ?
        \\order by claimed_at asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{ .{ .text = entity_kind }, .{ .int = entity_id } }) catch return error.QueryFailed;
    return try collectClaims(&stmt, allocator);
}

fn listClaimsBySession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    session_id: i64,
) ![]agentactivity.types.Claim {
    var stmt = d.prepare(
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where session_id = ?
        \\order by claimed_at asc
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = session_id }}) catch return error.QueryFailed;
    return try collectClaims(&stmt, allocator);
}

fn listClaimsByToken(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    token: []const u8,
) ![]agentactivity.types.Claim {
    var stmt = d.prepare(
        \\select id, claim_token, session_id, entity_kind, entity_id, claim_scope,
        \\       status, vendor, vendor_session_id, role, model,
        \\       worktree_id, worktree_path,
        \\       repo_root, branch, head_sha_at_claim, dirty_at_claim,
        \\       purpose, base_ref,
        \\       claimed_at, last_heartbeat_at, lease_expires_at,
        \\       released_at, release_reason
        \\from agent_work_claims
        \\where claim_token = ?
    ) catch return error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = token }}) catch return error.QueryFailed;
    return try collectClaims(&stmt, allocator);
}

fn collectClaims(
    stmt: *db.sqlite.Stmt,
    allocator: std.mem.Allocator,
) ![]agentactivity.types.Claim {
    var out: std.ArrayList(agentactivity.types.Claim) = .empty;
    errdefer {
        for (out.items) |c| c.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readClaimRow(stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
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
