//! engine/runtime/capture — orchestrator that ties sessions, entries,
//! and snapshots into the operator-facing `capture` verbs.
//!
//! The underlying primitives live in `session.zig` and `snapshot.zig`.
//! This module groups them so handlers have a small, intention-named
//! API: `openSession`, `closeSession`, `appendNote`, `appendCommand`,
//! `appendFile`, `takeSnapshot`.

const std = @import("std");
const db = @import("db");
const session_mod = @import("session.zig");
const snapshot_mod = @import("snapshot.zig");

pub const Error = session_mod.Error || snapshot_mod.Error || error{NoActiveSession};

pub const Session = session_mod.Session;
pub const Snapshot = snapshot_mod.Snapshot;

pub const OpenArgs = struct {
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    task_id: ?i64 = null,
    model: ?[]const u8 = null,
};

/// Open or reuse a session and emit a session-start entry. Mirrors the
/// Go `runCaptureSession` glue (calls StartSession + AppendEntry).
pub fn openSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: OpenArgs,
) Error!Session {
    const s = try session_mod.startSession(d, allocator, .{
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session_id,
        .task_id = args.task_id,
        .model = args.model,
    });
    // Best-effort marker; ignore failure so session creation isn't lost
    // if the entry insert hiccups (matches Go behavior).
    session_mod.appendEntry(d, s.id, "action", "session opened") catch {};
    return s;
}

pub const CloseArgs = struct {
    /// Explicit session id; when null, the caller must resolve the
    /// active session externally and pass it in. Centralizing
    /// vendor-env lookup is the handler's job (it owns `process.Environ`).
    session_id: i64,
    summary: ?[]const u8 = null,
};

/// Close the named session. Appends a "session ended" note before flipping
/// ended_at so the timeline records the boundary.
pub fn closeSession(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: CloseArgs,
) Error!void {
    session_mod.appendEntry(d, args.session_id, "note", "session ended") catch {};
    try session_mod.endSession(d, allocator, args.session_id, args.summary);
}

pub fn appendNote(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "note", body);
}

pub fn appendCommand(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "command", body);
}

pub fn appendFile(
    d: *db.sqlite.Db,
    session_id: i64,
    body: []const u8,
) Error!void {
    try session_mod.appendEntry(d, session_id, "file", body);
}

pub const SnapshotArgs = struct {
    session_id: i64,
    task_id: ?i64 = null,
    vendor: []const u8,
    vendor_session_id: ?[]const u8 = null,
    body: ?[]const u8 = null,
    next_action: ?[]const u8 = null,
};

/// Create a context_snapshots row and emit a "snapshot created" note in
/// the same session timeline.
pub fn takeSnapshot(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    args: SnapshotArgs,
) Error!Snapshot {
    const snap = try snapshot_mod.create(d, allocator, .{
        .session_id = args.session_id,
        .task_id = args.task_id,
        .vendor = args.vendor,
        .vendor_session_id = args.vendor_session_id,
        .body = args.body,
        .next_action = args.next_action,
    });
    const note = std.fmt.allocPrint(
        allocator,
        "snapshot created: id={d}",
        .{snap.id},
    ) catch return snap;
    defer allocator.free(note);
    session_mod.appendEntry(d, args.session_id, "note", note) catch {};
    return snap;
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

test "openSession appends a session-opened action entry" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    try std.testing.expectEqual(@as(usize, 1), entries.len);
    try std.testing.expectEqualStrings("action", entries[0].prefix);
    try std.testing.expectEqualStrings("session opened", entries[0].body);
}

test "closeSession appends note + ends session" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    try closeSession(&d, a, .{ .session_id = s.id, .summary = "done" });
    const after = try session_mod.getById(&d, a, s.id);
    defer session_mod.deinit(after, a);
    try std.testing.expect(after.ended_at != null);
    try std.testing.expectEqualStrings("done", after.summary.?);
}

test "appendNote / appendCommand / appendFile use the right prefixes" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    try appendNote(&d, s.id, "n");
    try appendCommand(&d, s.id, "c");
    try appendFile(&d, s.id, "f");
    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    // 1: opened action, 2: note, 3: command, 4: file
    try std.testing.expectEqual(@as(usize, 4), entries.len);
    try std.testing.expectEqualStrings("note", entries[1].prefix);
    try std.testing.expectEqualStrings("command", entries[2].prefix);
    try std.testing.expectEqualStrings("file", entries[3].prefix);
}

test "takeSnapshot creates row and emits note entry" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const s = try openSession(&d, a, .{ .vendor = "v" });
    defer session_mod.deinit(s, a);
    const snap = try takeSnapshot(&d, a, .{
        .session_id = s.id,
        .vendor = "v",
        .body = "the body",
        .next_action = "do it",
    });
    defer snapshot_mod.deinit(snap, a);
    try std.testing.expectEqualStrings("the body", snap.body);

    const entries = try session_mod.listEntriesForSession(&d, a, s.id);
    defer session_mod.deinitEntries(entries, a);
    // 1: opened, 2: snapshot created note
    try std.testing.expectEqual(@as(usize, 2), entries.len);
    try std.testing.expectEqualStrings("note", entries[1].prefix);
    try std.testing.expect(std.mem.startsWith(u8, entries[1].body, "snapshot created"));
}
