//! engine/external/agentingest/dispatch — translate a normalized
//! `iface.Event` into agentactivity store + session-lifecycle calls.
//!
//! This layer is vendor-agnostic: it consumes the normalized Event
//! shape from `interface.zig` and dispatches to
//! `engine.runtime.session` (for `.session_start` / `.session_end`)
//! and `engine.runtime.agentactivity.store` (for action variants).
//!
//! Counter contract — every successful dispatch increments exactly one
//! of `sessions_created`, `actions_created`, or neither (when the
//! session lifecycle no-ops because the row already exists / is
//! already ended). `events_processed` ticks on EVERY successful
//! dispatch regardless. The CLI handler reads these counters into the
//! `--json` envelope.

const std = @import("std");
const db = @import("db");
const iface = @import("interface.zig");
const session_mod = @import("../../runtime/session.zig");
const store = @import("../../runtime/agentactivity/store.zig");
const types = @import("../../runtime/agentactivity/types.zig");

pub const Counts = struct {
    sessions_created: i64 = 0,
    claims_created: i64 = 0,
    actions_created: i64 = 0,
    events_processed: i64 = 0,
};

pub const Error = error{
    SessionResolveFailed,
    SessionEndFailed,
    ActionWriteFailed,
} || std.mem.Allocator.Error;

/// Apply a parsed event against the DB. Increments `counts` in place.
/// Returns an error if any underlying store call fails; the CLI handler
/// is responsible for rolling back the surrounding transaction.
pub fn apply(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    vendor: []const u8,
    event: iface.Event,
    counts: *Counts,
) Error!void {
    switch (event) {
        .session_start => |e| {
            // sessions_created counts NEW rows. startSession is
            // idempotent on (vendor, vendor_session_id); we detect
            // "was it new?" by checking activeForVendor BEFORE.
            const pre = session_mod.activeForVendor(
                d,
                allocator,
                vendor,
                e.envelope.vendor_session_id,
            ) catch return Error.SessionResolveFailed;
            const was_new = (pre == null);
            if (pre) |p| session_mod.deinit(p, allocator);

            const s = session_mod.startSession(d, allocator, .{
                .vendor = vendor,
                .vendor_session_id = e.envelope.vendor_session_id,
                .model = e.envelope.model,
            }) catch return Error.SessionResolveFailed;
            defer session_mod.deinit(s, allocator);

            if (was_new) counts.sessions_created += 1;
            if (e.summary) |sum| {
                session_mod.appendEntry(d, s.id, "note", sum) catch return Error.ActionWriteFailed;
            }
        },
        .session_end => |e| {
            const existing = session_mod.activeForVendor(
                d,
                allocator,
                vendor,
                e.envelope.vendor_session_id,
            ) catch return Error.SessionResolveFailed;
            if (existing) |s| {
                defer session_mod.deinit(s, allocator);
                session_mod.endSession(d, allocator, s.id, e.summary) catch |err| switch (err) {
                    // AlreadyEnded is a no-op success — the hook fired
                    // twice or arrived after the operator manually ended
                    // the session. Don't fail the ingest.
                    error.AlreadyEnded => {},
                    else => return Error.SessionEndFailed,
                };
            }
            // No `existing` means session_end arrived for an unknown
            // session — silently no-op (the alternative would be to
            // auto-open a session just to immediately close it, which
            // would dirty the sessions table for no benefit).
        },
        .action_start => |e| {
            const sid = try resolveSession(d, allocator, vendor, e.envelope, counts);
            _ = store.startAction(d, allocator, .{
                .session_id = sid,
                .action_kind = e.action_kind,
                .vendor = vendor,
                .vendor_role = e.envelope.vendor_role,
                .model = e.envelope.model,
            }) catch return Error.ActionWriteFailed;
            counts.actions_created += 1;
        },
        .action_end => |e| {
            // action_end with no in-flight matching action no-ops; we
            // don't surface an error because the hook stream may arrive
            // out of order (action_end before action_start). The shape
            // contract still counts this as an event_processed.
            const sid = try resolveSession(d, allocator, vendor, e.envelope, counts);
            const open_id = findOpenAction(d, sid, e.action_kind) catch return Error.ActionWriteFailed;
            if (open_id) |id| {
                store.endAction(d, allocator, id, e.outcome, e.summary) catch return Error.ActionWriteFailed;
            }
        },
        .action_atomic => |e| {
            const sid = try resolveSession(d, allocator, vendor, e.envelope, counts);
            const id = store.startAction(d, allocator, .{
                .session_id = sid,
                .action_kind = e.action_kind,
                .vendor = vendor,
                .vendor_role = e.envelope.vendor_role,
                .model = e.envelope.model,
            }) catch return Error.ActionWriteFailed;
            store.endAction(d, allocator, id, e.outcome, e.summary) catch return Error.ActionWriteFailed;
            counts.actions_created += 1;
        },
    }
    counts.events_processed += 1;
}

/// Resolve the session for an envelope, auto-opening if missing.
/// When auto-opening creates a new session row, bumps
/// `counts.sessions_created` so the action_start / action_atomic
/// paths report the auto-open in the same shape as an explicit
/// `.session_start` event would.
fn resolveSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    vendor: []const u8,
    envelope: iface.Envelope,
    counts: *Counts,
) Error!i64 {
    const pre = session_mod.activeForVendor(
        d,
        allocator,
        vendor,
        envelope.vendor_session_id,
    ) catch return Error.SessionResolveFailed;
    if (pre) |p| {
        defer session_mod.deinit(p, allocator);
        return p.id;
    }
    const created = session_mod.startSession(d, allocator, .{
        .vendor = vendor,
        .vendor_session_id = envelope.vendor_session_id,
        .model = envelope.model,
    }) catch return Error.SessionResolveFailed;
    defer session_mod.deinit(created, allocator);
    counts.sessions_created += 1;
    return created.id;
}

/// Find the most-recent agent_actions row for this session and
/// action_kind whose ended_at IS NULL. Returns null if no such row
/// exists. Used to pair `action_end` events with their `action_start`.
fn findOpenAction(
    d: *db.sqlite.Db,
    session_id: i64,
    kind: types.ActionKind,
) !?i64 {
    var stmt = try d.prepare(
        \\select id from agent_actions
        \\where session_id = ?
        \\  and action_kind = ?
        \\  and ended_at is null
        \\order by id desc
        \\limit 1
    );
    defer stmt.finalize();
    try stmt.bind(&.{
        .{ .int = session_id },
        .{ .text = kind.toText() },
    });
    return switch (try stmt.step()) {
        .done => null,
        .row => stmt.columnInt(0),
    };
}

// =========================================================================
// Tests
// =========================================================================

fn setupTestDb(allocator: std.mem.Allocator) !db.sqlite.Db {
    var d = try db.sqlite.Db.openMemory();
    errdefer d.close();
    try db.migrate.applyAll(&d, allocator);
    return d;
}

test "apply session_start: new session bumps sessions_created" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var counts: Counts = .{};
    const evt: iface.Event = .{ .session_start = .{
        .envelope = .{
            .vendor_session_id = try a.dupe(u8, "sid-1"),
            .model = try a.dupe(u8, "claude-opus-4-7"),
        },
    } };
    defer evt.deinit(a);

    try apply(&d, a, "claude", evt, &counts);
    try std.testing.expectEqual(@as(i64, 1), counts.sessions_created);
    try std.testing.expectEqual(@as(i64, 1), counts.events_processed);
}

test "apply session_start twice: second call REUSES (sessions_created==1)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var counts: Counts = .{};
    const evt1: iface.Event = .{ .session_start = .{
        .envelope = .{ .vendor_session_id = try a.dupe(u8, "sid-1") },
    } };
    defer evt1.deinit(a);
    try apply(&d, a, "claude", evt1, &counts);

    const evt2: iface.Event = .{ .session_start = .{
        .envelope = .{ .vendor_session_id = try a.dupe(u8, "sid-1") },
    } };
    defer evt2.deinit(a);
    try apply(&d, a, "claude", evt2, &counts);

    try std.testing.expectEqual(@as(i64, 1), counts.sessions_created);
    try std.testing.expectEqual(@as(i64, 2), counts.events_processed);
}

test "apply action_atomic: actions_created+=1 and action row is closed" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    var counts: Counts = .{};
    const evt: iface.Event = .{ .action_atomic = .{
        .envelope = .{ .vendor_session_id = try a.dupe(u8, "sid-1") },
        .action_kind = .tool_call,
        .outcome = .ok,
        .summary = try a.dupe(u8, "ran ls"),
    } };
    defer evt.deinit(a);

    try apply(&d, a, "claude", evt, &counts);
    try std.testing.expectEqual(@as(i64, 1), counts.actions_created);
    try std.testing.expectEqual(@as(i64, 1), counts.events_processed);

    // The auto-opened session also bumped sessions_created.
    try std.testing.expectEqual(@as(i64, 1), counts.sessions_created);

    // Verify the action is closed.
    const n_closed = try d.intQuery(
        "select count(*) from agent_actions where ended_at is not null",
    );
    try std.testing.expectEqual(@as(i64, 1), n_closed);
}

test "apply session_end on unknown session: no-op, no error" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var counts: Counts = .{};
    const evt: iface.Event = .{ .session_end = .{
        .envelope = .{ .vendor_session_id = try a.dupe(u8, "ghost") },
    } };
    defer evt.deinit(a);
    try apply(&d, a, "claude", evt, &counts);
    try std.testing.expectEqual(@as(i64, 1), counts.events_processed);
}

test "apply action_end with no matching open action: no-op success" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var counts: Counts = .{};
    const evt: iface.Event = .{ .action_end = .{
        .envelope = .{ .vendor_session_id = try a.dupe(u8, "sid-1") },
        .action_kind = .tool_call,
        .outcome = .ok,
    } };
    defer evt.deinit(a);
    try apply(&d, a, "claude", evt, &counts);
    try std.testing.expectEqual(@as(i64, 1), counts.events_processed);
}
