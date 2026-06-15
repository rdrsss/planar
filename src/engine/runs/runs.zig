//! engine/runs/runs — the run-record lifecycle: the measurement-rig
//! substrate behind the (future) `planar run *` / `bench` verb group.
//!
//! Owns the SQLite writes for the three tables introduced by migration
//! 00025 (see docs/research/run-record-schema.md):
//!
//!   runs        — one row per (plan, arm, repetition); the experimental
//!                 unit. Comparison is paired per plan; two runs are
//!                 comparable iff their config_hash is identical except
//!                 for arm.
//!   run_events  — the run's append-only, seq-ordered journal. kind +
//!                 payload are opaque (JSON-validated at the CLI, plain
//!                 text at the schema).
//!   run_touches — the declared-vs-actual touch harvest. Each row is one
//!                 (task, path) touch tagged kind in {declared, actual}.
//!
//! Scope boundary: this module implements the lifecycle PRIMITIVES only —
//! `start`, `event`, `touch`, `finish`, `show`. The declared-touch
//! snapshot-on-start (reading task_touch_paths into kind='declared' at
//! run start) is a SEPARATE task (M1.5) that will extend `start`; it is
//! deliberately absent here.
//!
//! Like the rest of the engine this module holds no IO writers and no
//! global state; callers pass `*db.sqlite.Db` plus an allocator and do
//! their own output rendering.

const std = @import("std");
const db = @import("db");

// =========================================================================
// Types
// =========================================================================

/// The declared-vs-actual touch discriminator. `declared` is the
/// predicted closure snapshotted at run start; `actual` is ground truth
/// harvested from `git diff --name-only` at fan-in. The schema enforces
/// this closed two-value set with a CHECK; we mirror it here so the
/// engine refuses an out-of-set value before reaching SQLite.
pub const TouchKind = enum {
    declared,
    actual,

    pub fn fromText(s: []const u8) ?TouchKind {
        if (std.mem.eql(u8, s, "declared")) return .declared;
        if (std.mem.eql(u8, s, "actual")) return .actual;
        return null;
    }
};

/// A run record as read back by `show`. `arm` / `status` / `corpus_repo`
/// are plain text — their enum sets are enforced at the CLI parse layer,
/// not the schema, so pilot/probe runs need no migration. Owned strings
/// are freed by `deinit`.
pub const Run = struct {
    id: i64,
    run_uid: []const u8,
    plan_id: i64,
    arm: []const u8,
    base_sha: []const u8,
    config_hash: []const u8,
    config_json: ?[]const u8,
    corpus_repo: ?[]const u8,
    status: []const u8,
    started_at: []const u8,
    ended_at: ?[]const u8,
};

pub fn deinit(r: Run, allocator: std.mem.Allocator) void {
    allocator.free(r.run_uid);
    allocator.free(r.arm);
    allocator.free(r.base_sha);
    allocator.free(r.config_hash);
    if (r.config_json) |s| allocator.free(s);
    if (r.corpus_repo) |s| allocator.free(s);
    allocator.free(r.status);
    allocator.free(r.started_at);
    if (r.ended_at) |s| allocator.free(s);
}

/// A journal row as read back by `events`. Owned strings freed by
/// `Event.deinit`.
pub const Event = struct {
    id: i64,
    run_id: i64,
    seq: i64,
    kind: []const u8,
    payload: ?[]const u8,
    created_at: []const u8,

    pub fn deinit(self: Event, allocator: std.mem.Allocator) void {
        allocator.free(self.kind);
        if (self.payload) |s| allocator.free(s);
        allocator.free(self.created_at);
    }
};

/// A touch row as read back by `touches`. Owned strings freed by
/// `Touch.deinit`.
pub const Touch = struct {
    id: i64,
    run_id: i64,
    task_id: i64,
    path: []const u8,
    kind: TouchKind,
    created_at: []const u8,

    pub fn deinit(self: Touch, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.created_at);
    }
};

pub const StartArgs = struct {
    /// Stable external id (ULID/uuid) minted by the harness. Required —
    /// the engine does not synthesize one; the caller owns the identity
    /// scheme so archived transcripts can be keyed by it.
    run_uid: []const u8,
    plan_id: i64,
    arm: []const u8,
    base_sha: []const u8,
    config_hash: []const u8,
    config_json: ?[]const u8 = null,
    corpus_repo: ?[]const u8 = null,
    /// Initial status; defaults to the schema default 'running' when null.
    status: ?[]const u8 = null,
};

/// The result of `start`: the new autoincrement id plus the caller's
/// run_uid echoed back. The id is the FK target for events/touches; the
/// run_uid is the stable external handle.
pub const StartResult = struct {
    id: i64,
    run_uid: []const u8,

    pub fn deinit(self: StartResult, allocator: std.mem.Allocator) void {
        allocator.free(self.run_uid);
    }
};

pub const Error = error{
    NotFound,
    /// run_uid collided with an existing run (UNIQUE violation).
    DuplicateRunUid,
    /// (run_id, seq) collided — the seq is taken for this run.
    DuplicateSeq,
    QueryFailed,
} || std.mem.Allocator.Error;

// =========================================================================
// Lifecycle
// =========================================================================

/// Insert a `runs` row and return its new id + run_uid. Does NOT snapshot
/// declared touches — that is M1.5's extension point.
pub fn start(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: StartArgs) Error!StartResult {
    const id = d.execParams(
        \\insert into runs
        \\  (run_uid, plan_id, arm, base_sha, config_hash, config_json, corpus_repo, status)
        \\values
        \\  (?, ?, ?, ?, ?, ?, ?, coalesce(?, 'running'))
    , &.{
        .{ .text = args.run_uid },
        .{ .int = args.plan_id },
        .{ .text = args.arm },
        .{ .text = args.base_sha },
        .{ .text = args.config_hash },
        if (args.config_json) |s| .{ .text = s } else .{ .null = {} },
        if (args.corpus_repo) |s| .{ .text = s } else .{ .null = {} },
        if (args.status) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.DuplicateRunUid;
        std.log.err("runs.start exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    return .{ .id = id, .run_uid = try allocator.dupe(u8, args.run_uid) };
}

/// Append a `run_events` row. `seq` is caller-supplied and ordered per
/// run — the UNIQUE (run_id, seq) constraint rejects a re-used seq with
/// Error.DuplicateSeq. `kind` / `payload` are opaque text at this layer.
pub fn event(
    d: *db.sqlite.Db,
    run_id: i64,
    seq: i64,
    kind: []const u8,
    payload: ?[]const u8,
) Error!i64 {
    return d.execParams(
        \\insert into run_events (run_id, seq, kind, payload)
        \\values (?, ?, ?, ?)
    , &.{
        .{ .int = run_id },
        .{ .int = seq },
        .{ .text = kind },
        if (payload) |s| .{ .text = s } else .{ .null = {} },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.DuplicateSeq;
        std.log.err("runs.event exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
}

/// Insert a `run_touches` row. `task_id` is a plain integer (not an FK
/// cascade) so deleting a task later cannot erase the historical record
/// of what it once touched. Duplicate (run, task, path, kind) tuples are
/// rejected by the UNIQUE constraint; the engine surfaces that as a
/// no-op-friendly idempotent insert by returning the existing rowid is
/// NOT attempted here — callers that re-touch the same tuple receive
/// Error.QueryFailed via the UNIQUE violation. (Harvest dedupes before
/// inserting, so this path is for genuine programmer error.)
pub fn touch(
    d: *db.sqlite.Db,
    run_id: i64,
    task_id: i64,
    path: []const u8,
    kind: TouchKind,
) Error!i64 {
    return d.execParams(
        \\insert into run_touches (run_id, task_id, path, kind)
        \\values (?, ?, ?, ?)
    , &.{
        .{ .int = run_id },
        .{ .int = task_id },
        .{ .text = path },
        .{ .text = @tagName(kind) },
    }) catch |e| {
        std.log.err("runs.touch exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
}

/// Set the run's terminal status and stamp `ended_at` to now. Idempotent
/// at the SQL level (a second finish just rewrites status + ended_at).
/// Returns Error.NotFound if the run id does not exist.
pub fn finish(
    d: *db.sqlite.Db,
    run_id: i64,
    status: []const u8,
) Error!void {
    // Verify existence first so a no-op UPDATE on a missing id surfaces
    // as NotFound rather than a silent success.
    var stmt = d.prepare("select count(*) from runs where id = ?") catch return Error.QueryFailed;
    {
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = run_id }}) catch return Error.QueryFailed;
        const n = switch (stmt.step() catch return Error.QueryFailed) {
            .row => stmt.columnInt(0),
            .done => 0,
        };
        if (n == 0) return Error.NotFound;
    }

    _ = d.execParams(
        \\update runs
        \\set status = ?,
        \\    ended_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{ .{ .text = status }, .{ .int = run_id } }) catch |e| {
        std.log.err("runs.finish exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };
}

// =========================================================================
// Reads
// =========================================================================

const select_columns =
    "id, run_uid, plan_id, arm, base_sha, config_hash, config_json, " ++
    "corpus_repo, status, started_at, ended_at";

/// Read one run back by id. Sufficient for the future `bench show --json`.
pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Run {
    var stmt = d.prepare("select " ++ select_columns ++ " from runs where id = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRun(&stmt, allocator),
    };
}

/// Read one run back by its stable external run_uid.
pub fn showByUid(d: *db.sqlite.Db, allocator: std.mem.Allocator, run_uid: []const u8) Error!Run {
    var stmt = d.prepare("select " ++ select_columns ++ " from runs where run_uid = ?") catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = run_uid }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRun(&stmt, allocator),
    };
}

/// All journal rows for a run, ordered by seq. Caller frees via
/// `deinitEvents`.
pub fn events(d: *db.sqlite.Db, allocator: std.mem.Allocator, run_id: i64) Error![]Event {
    var stmt = d.prepare(
        "select id, run_id, seq, kind, payload, created_at from run_events where run_id = ? order by seq",
    ) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = run_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Event) = .empty;
    errdefer {
        for (out.items) |x| x.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, .{
                .id = stmt.columnInt(0),
                .run_id = stmt.columnInt(1),
                .seq = stmt.columnInt(2),
                .kind = try stmt.columnTextAlloc(3, allocator),
                .payload = try stmt.columnTextOpt(4, allocator),
                .created_at = try stmt.columnTextAlloc(5, allocator),
            }),
        }
    }
    return out.toOwnedSlice(allocator);
}

pub fn deinitEvents(items: []const Event, allocator: std.mem.Allocator) void {
    for (items) |x| x.deinit(allocator);
    allocator.free(items);
}

/// All touch rows for a run, optionally filtered by kind, ordered by id.
/// Caller frees via `deinitTouches`.
pub fn touches(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    run_id: i64,
    kind: ?TouchKind,
) Error![]Touch {
    var stmt = if (kind) |k| blk: {
        var s = d.prepare(
            "select id, run_id, task_id, path, kind, created_at from run_touches where run_id = ? and kind = ? order by id",
        ) catch return Error.QueryFailed;
        s.bind(&.{ .{ .int = run_id }, .{ .text = @tagName(k) } }) catch return Error.QueryFailed;
        break :blk s;
    } else blk: {
        var s = d.prepare(
            "select id, run_id, task_id, path, kind, created_at from run_touches where run_id = ? order by id",
        ) catch return Error.QueryFailed;
        s.bind(&.{.{ .int = run_id }}) catch return Error.QueryFailed;
        break :blk s;
    };
    defer stmt.finalize();

    var out: std.ArrayList(Touch) = .empty;
    errdefer {
        for (out.items) |x| x.deinit(allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => {
                const kind_text = try stmt.columnTextAlloc(4, allocator);
                defer allocator.free(kind_text);
                const tk = TouchKind.fromText(kind_text) orelse return Error.QueryFailed;
                try out.append(allocator, .{
                    .id = stmt.columnInt(0),
                    .run_id = stmt.columnInt(1),
                    .task_id = stmt.columnInt(2),
                    .path = try stmt.columnTextAlloc(3, allocator),
                    .kind = tk,
                    .created_at = try stmt.columnTextAlloc(5, allocator),
                });
            },
        }
    }
    return out.toOwnedSlice(allocator);
}

pub fn deinitTouches(items: []const Touch, allocator: std.mem.Allocator) void {
    for (items) |x| x.deinit(allocator);
    allocator.free(items);
}

// =========================================================================
// Internals
// =========================================================================

fn readRun(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Run {
    return .{
        .id = stmt.columnInt(0),
        .run_uid = try stmt.columnTextAlloc(1, allocator),
        .plan_id = stmt.columnInt(2),
        .arm = try stmt.columnTextAlloc(3, allocator),
        .base_sha = try stmt.columnTextAlloc(4, allocator),
        .config_hash = try stmt.columnTextAlloc(5, allocator),
        .config_json = try stmt.columnTextOpt(6, allocator),
        .corpus_repo = try stmt.columnTextOpt(7, allocator),
        .status = try stmt.columnTextAlloc(8, allocator),
        .started_at = try stmt.columnTextAlloc(9, allocator),
        .ended_at = try stmt.columnTextOpt(10, allocator),
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

/// Seed a plan row so the runs.plan_id FK is satisfiable. Returns its id.
fn seedPlan(d: *db.sqlite.Db, slug: []const u8) !i64 {
    return d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', ?, ?, 'draft')",
        &.{ .{ .text = slug }, .{ .text = slug } },
    );
}

test "start inserts a run and echoes id + run_uid (status defaults to running)" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "exp plan");

    const res = try start(&d, a, .{
        .run_uid = "01J0RUNUID0001",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "deadbeef",
        .config_hash = "cfg-hash-1",
        .config_json = "{\"model\":\"x\"}",
        .corpus_repo = "corpus/foo",
    });
    defer res.deinit(a);

    try std.testing.expect(res.id > 0);
    try std.testing.expectEqualStrings("01J0RUNUID0001", res.run_uid);

    const r = try show(&d, a, res.id);
    defer deinit(r, a);
    try std.testing.expectEqualStrings("strict", r.arm);
    try std.testing.expectEqualStrings("deadbeef", r.base_sha);
    try std.testing.expectEqualStrings("cfg-hash-1", r.config_hash);
    try std.testing.expectEqualStrings("{\"model\":\"x\"}", r.config_json.?);
    try std.testing.expectEqualStrings("corpus/foo", r.corpus_repo.?);
    try std.testing.expectEqualStrings("running", r.status);
    try std.testing.expect(r.ended_at == null);
    try std.testing.expectEqual(pid, r.plan_id);
}

test "start honours an explicit initial status" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-status",
        .plan_id = pid,
        .arm = "eligibility",
        .base_sha = "sha",
        .config_hash = "h",
        .status = "queued",
    });
    defer res.deinit(a);
    const r = try show(&d, a, res.id);
    defer deinit(r, a);
    try std.testing.expectEqualStrings("queued", r.status);
}

test "start leaves config_json / corpus_repo null when omitted" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-nulls",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "sha",
        .config_hash = "h",
    });
    defer res.deinit(a);
    const r = try show(&d, a, res.id);
    defer deinit(r, a);
    try std.testing.expect(r.config_json == null);
    try std.testing.expect(r.corpus_repo == null);
}

test "start rejects a duplicate run_uid with DuplicateRunUid" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const first = try start(&d, a, .{
        .run_uid = "dup",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "sha",
        .config_hash = "h",
    });
    defer first.deinit(a);
    try std.testing.expectError(Error.DuplicateRunUid, start(&d, a, .{
        .run_uid = "dup",
        .plan_id = pid,
        .arm = "eligibility",
        .base_sha = "sha2",
        .config_hash = "h2",
    }));
}

test "event appends seq-ordered journal rows; reads back in order" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-ev",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);

    _ = try event(&d, res.id, 1, "token_sample", "{\"in\":10,\"out\":5}");
    _ = try event(&d, res.id, 2, "reviewer_decision", "{\"approve\":true}");
    _ = try event(&d, res.id, 3, "budget_mark", null);

    const evs = try events(&d, a, res.id);
    defer deinitEvents(evs, a);
    try std.testing.expectEqual(@as(usize, 3), evs.len);
    try std.testing.expectEqual(@as(i64, 1), evs[0].seq);
    try std.testing.expectEqualStrings("token_sample", evs[0].kind);
    try std.testing.expectEqualStrings("{\"in\":10,\"out\":5}", evs[0].payload.?);
    try std.testing.expectEqual(@as(i64, 3), evs[2].seq);
    try std.testing.expect(evs[2].payload == null);
}

test "event rejects a re-used seq with DuplicateSeq" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-seq",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);
    _ = try event(&d, res.id, 1, "k", null);
    try std.testing.expectError(Error.DuplicateSeq, event(&d, res.id, 1, "k2", null));
}

test "touch inserts declared + actual rows; filtered read by kind" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-touch",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);

    _ = try touch(&d, res.id, 100, "src/a.zig", .declared);
    _ = try touch(&d, res.id, 100, "src/b.zig", .declared);
    _ = try touch(&d, res.id, 100, "src/a.zig", .actual);

    const all = try touches(&d, a, res.id, null);
    defer deinitTouches(all, a);
    try std.testing.expectEqual(@as(usize, 3), all.len);

    const declared = try touches(&d, a, res.id, .declared);
    defer deinitTouches(declared, a);
    try std.testing.expectEqual(@as(usize, 2), declared.len);
    try std.testing.expectEqual(@as(i64, 100), declared[0].task_id);
    try std.testing.expectEqual(TouchKind.declared, declared[0].kind);

    const actual = try touches(&d, a, res.id, .actual);
    defer deinitTouches(actual, a);
    try std.testing.expectEqual(@as(usize, 1), actual.len);
    try std.testing.expectEqualStrings("src/a.zig", actual[0].path);
}

test "touch task_id is a plain integer — survives task deletion" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const tid = try d.execParams(
        "insert into tasks (scope_kind, title, status) values ('global','will-delete','todo')",
        &.{},
    );
    const res = try start(&d, a, .{
        .run_uid = "uid-orphan",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);
    _ = try touch(&d, res.id, tid, "src/x.zig", .actual);

    // Deleting the task must NOT cascade-delete the historical touch.
    _ = try d.execParams("delete from tasks where id = ?", &.{.{ .int = tid }});

    const remaining = try touches(&d, a, res.id, null);
    defer deinitTouches(remaining, a);
    try std.testing.expectEqual(@as(usize, 1), remaining.len);
    try std.testing.expectEqual(tid, remaining[0].task_id);
}

test "finish sets terminal status and stamps ended_at" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-finish",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);

    try finish(&d, res.id, "completed");
    const r = try show(&d, a, res.id);
    defer deinit(r, a);
    try std.testing.expectEqualStrings("completed", r.status);
    try std.testing.expect(r.ended_at != null);
}

test "finish on a missing run returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    try std.testing.expectError(Error.NotFound, finish(&d, 9999, "completed"));
}

test "show / showByUid round-trip; missing id is NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-show",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    defer res.deinit(a);

    const by_uid = try showByUid(&d, a, "uid-show");
    defer deinit(by_uid, a);
    try std.testing.expectEqual(res.id, by_uid.id);

    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
    try std.testing.expectError(Error.NotFound, showByUid(&d, a, "no-such-uid"));
}

test "deleting a run cascades its events and touches" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    const pid = try seedPlan(&d, "p");
    const res = try start(&d, a, .{
        .run_uid = "uid-cascade",
        .plan_id = pid,
        .arm = "strict",
        .base_sha = "s",
        .config_hash = "h",
    });
    const run_id = res.id;
    res.deinit(a);

    _ = try event(&d, run_id, 1, "k", null);
    _ = try touch(&d, run_id, 1, "p.zig", .actual);

    _ = try d.execParams("delete from runs where id = ?", &.{.{ .int = run_id }});

    try std.testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from run_events"));
    try std.testing.expectEqual(@as(i64, 0), try d.intQuery("select count(*) from run_touches"));
}
