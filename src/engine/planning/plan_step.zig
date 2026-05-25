//! engine/planning/plan_step — PlanStep entity: CRUD against the `plan_steps` table.
//!
//! Steps belong to a plan and represent ordered action items within that plan.
//! Each step has a body, a status, and an optional link to a task.
//!
//! Status set: {pending, in_progress, done, skipped}.
//! Terminal states: {done, skipped} — no further transitions permitted.
//!
//! Ordinal assignment: when no explicit ordinal is supplied, the engine
//! assigns max(ordinal)+1 for the plan_id. On an empty plan the first
//! step gets ordinal 1.
//!
//! The unique (plan_id, ordinal) constraint is enforced by the schema;
//! inserting a duplicate ordinal returns OrdinalConflict.

const std = @import("std");
const db = @import("db");
const policy = @import("../policy.zig");

// =========================================================================
// Types
// =========================================================================

pub const Status = enum {
    pending,
    in_progress,
    done,
    skipped,

    pub fn fromText(s: []const u8) ?Status {
        if (std.mem.eql(u8, s, "pending")) return .pending;
        if (std.mem.eql(u8, s, "in-progress")) return .in_progress;
        if (std.mem.eql(u8, s, "done")) return .done;
        if (std.mem.eql(u8, s, "skipped")) return .skipped;
        return null;
    }

    pub fn toText(self: Status) []const u8 {
        return switch (self) {
            .pending => "pending",
            .in_progress => "in-progress",
            .done => "done",
            .skipped => "skipped",
        };
    }

    pub fn isTerminal(self: Status) bool {
        return self == .done or self == .skipped;
    }
};

/// One row from the `plan_steps` table. String fields are owned by the
/// allocator passed to the function that produced this Step — the caller
/// frees them (or lets the process arena reclaim them).
pub const Step = struct {
    id: i64,
    plan_id: i64,
    ordinal: i64,
    body: []const u8,
    status: Status,
    task_id: ?i64,
    created_at: []const u8,
    updated_at: []const u8,
};

pub fn deinit(s: Step, allocator: std.mem.Allocator) void {
    allocator.free(s.body);
    allocator.free(s.created_at);
    allocator.free(s.updated_at);
}

pub fn deinitMany(steps: []const Step, allocator: std.mem.Allocator) void {
    for (steps) |s| deinit(s, allocator);
    allocator.free(steps);
}

pub const AddArgs = struct {
    plan_id: i64,
    body: []const u8,
    /// When null, ordinal is assigned as max(ordinal)+1 for the plan.
    ordinal: ?i64 = null,
};

pub const Error =
    error{
        NotFound,
        InvalidTransition,
        OrdinalConflict,
        QueryFailed,
    } ||
    std.mem.Allocator.Error ||
    policy.audit.Error;

// =========================================================================
// CRUD
// =========================================================================

/// Add a step to a plan. If `args.ordinal` is null, appends at ordinal
/// max+1 (or 1 for an empty plan). Returns OrdinalConflict on duplicate.
pub fn add(d: *db.sqlite.Db, allocator: std.mem.Allocator, args: AddArgs) Error!Step {
    // Verify plan exists.
    {
        var stmt = d.prepare("select count(*) from plans where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = args.plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => {
                if (stmt.columnInt(0) == 0) return Error.NotFound;
            },
        }
    }

    const ordinal: i64 = if (args.ordinal) |o|
        o
    else blk: {
        // Compute max(ordinal)+1 for the plan.
        var stmt = d.prepare(
            "select coalesce(max(ordinal), 0) from plan_steps where plan_id = ?",
        ) catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = args.plan_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break :blk @as(i64, 1),
            .row => break :blk stmt.columnInt(0) + 1,
        }
    };

    const id = d.execParams(
        \\insert into plan_steps (plan_id, ordinal, body, status)
        \\values (?, ?, ?, 'pending')
    , &.{
        .{ .int = args.plan_id },
        .{ .int = ordinal },
        .{ .text = args.body },
    }) catch |e| {
        if (d.lastWasUniqueViolation()) return Error.OrdinalConflict;
        std.log.err("plan_step.add exec failed: {s}", .{@errorName(e)});
        return Error.QueryFailed;
    };

    const summary = try std.fmt.allocPrint(allocator, "add step {d} to plan {d}", .{ ordinal, args.plan_id });
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .create,
        .entity = .{ .kind = "plan_step", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
}

/// Fetch a single step by its primary key.
pub fn show(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Step {
    var stmt = d.prepare(select_one_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = id }}) catch return Error.QueryFailed;
    return switch (stmt.step() catch return Error.QueryFailed) {
        .done => Error.NotFound,
        .row => try readRow(&stmt, allocator),
    };
}

/// Return all steps for a plan, ordered by ordinal ascending.
pub fn list(d: *db.sqlite.Db, allocator: std.mem.Allocator, plan_id: i64) Error![]Step {
    var stmt = d.prepare(select_all_sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = plan_id }}) catch return Error.QueryFailed;

    var out: std.ArrayList(Step) = .empty;
    errdefer {
        for (out.items) |s| deinit(s, allocator);
        out.deinit(allocator);
    }
    while (true) {
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => break,
            .row => try out.append(allocator, try readRow(&stmt, allocator)),
        }
    }
    return try out.toOwnedSlice(allocator);
}

/// Transition a step to 'done'. Valid from: pending, in_progress.
/// Returns InvalidTransition when the step is already terminal.
pub fn markDone(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Step {
    return try stepTransition(d, allocator, id, .done);
}

/// Transition a step to 'skipped'. Valid from: pending only.
/// Returns InvalidTransition when the step is already terminal.
pub fn skip(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64) Error!Step {
    return try stepTransition(d, allocator, id, .skipped);
}

/// Link a step to a task. Sets plan_steps.task_id = task_id.
/// Returns NotFound when the step or task does not exist.
pub fn linkTask(d: *db.sqlite.Db, allocator: std.mem.Allocator, step_id: i64, task_id: i64) Error!Step {
    // Verify step exists.
    const current = try show(d, allocator, step_id);
    deinit(current, allocator);

    // Verify task exists.
    {
        var stmt = d.prepare("select count(*) from tasks where id = ?") catch return Error.QueryFailed;
        defer stmt.finalize();
        stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
        switch (stmt.step() catch return Error.QueryFailed) {
            .done => return Error.NotFound,
            .row => {
                if (stmt.columnInt(0) == 0) return Error.NotFound;
            },
        }
    }

    _ = d.execParams(
        \\update plan_steps
        \\set task_id = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{ .{ .int = task_id }, .{ .int = step_id } }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "link step {d} to task {d}",
        .{ step_id, task_id },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .link,
        .entity = .{ .kind = "plan_step", .id = step_id },
        .summary = summary,
    });

    return try show(d, allocator, step_id);
}

/// Unlink a step from its task (sets task_id = NULL).
pub fn unlinkTask(d: *db.sqlite.Db, allocator: std.mem.Allocator, step_id: i64) Error!Step {
    // Verify step exists.
    const current = try show(d, allocator, step_id);
    deinit(current, allocator);

    _ = d.execParams(
        \\update plan_steps
        \\set task_id = null, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{.{ .int = step_id }}) catch return Error.QueryFailed;

    try policy.audit.record(d, .{
        .verb = .unlink,
        .entity = .{ .kind = "plan_step", .id = step_id },
        .summary = null,
    });

    return try show(d, allocator, step_id);
}

// =========================================================================
// Internals
// =========================================================================

const select_columns = "id, plan_id, ordinal, body, status, task_id, created_at, updated_at";

const select_one_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from plan_steps where id = ?";

const select_all_sql: [:0]const u8 =
    "select " ++ select_columns ++ " from plan_steps where plan_id = ? order by ordinal";

fn readRow(stmt: *db.sqlite.Stmt, allocator: std.mem.Allocator) Error!Step {
    const status_text = try stmt.columnTextAlloc(4, allocator);
    defer allocator.free(status_text);
    const status = Status.fromText(status_text) orelse return Error.QueryFailed;

    return .{
        .id = stmt.columnInt(0),
        .plan_id = stmt.columnInt(1),
        .ordinal = stmt.columnInt(2),
        .body = try stmt.columnTextAlloc(3, allocator),
        .status = status,
        .task_id = stmt.columnIntOpt(5),
        .created_at = try stmt.columnTextAlloc(6, allocator),
        .updated_at = try stmt.columnTextAlloc(7, allocator),
    };
}

fn stepTransition(d: *db.sqlite.Db, allocator: std.mem.Allocator, id: i64, new_status: Status) Error!Step {
    const current = try show(d, allocator, id);
    defer deinit(current, allocator);

    if (current.status.isTerminal()) return Error.InvalidTransition;

    // For skip: only pending is valid source.
    // For done: pending or in_progress are valid.
    if (new_status == .skipped and current.status != .pending) return Error.InvalidTransition;

    _ = d.execParams(
        \\update plan_steps
        \\set status = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
        \\where id = ?
    , &.{ .{ .text = new_status.toText() }, .{ .int = id } }) catch return Error.QueryFailed;

    const summary = try std.fmt.allocPrint(
        allocator,
        "step {d}: {s} → {s}",
        .{ id, current.status.toText(), new_status.toText() },
    );
    defer allocator.free(summary);
    try policy.audit.record(d, .{
        .verb = .status_change,
        .entity = .{ .kind = "plan_step", .id = id },
        .summary = summary,
    });

    return try show(d, allocator, id);
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

fn insertPlan(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into plans (scope_kind, title, slug, status) values ('global', 'Test Plan', 'test-plan', 'active')",
        &.{},
    );
}

fn insertTask(d: *db.sqlite.Db) !i64 {
    return try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'Test Task', 'todo', 100)",
        &.{},
    );
}

test "Status.fromText / toText round-trip" {
    try std.testing.expectEqual(Status.pending, Status.fromText("pending").?);
    try std.testing.expectEqual(Status.in_progress, Status.fromText("in-progress").?);
    try std.testing.expectEqual(Status.done, Status.fromText("done").?);
    try std.testing.expectEqual(Status.skipped, Status.fromText("skipped").?);
    try std.testing.expect(Status.fromText("bogus") == null);

    try std.testing.expectEqualStrings("pending", Status.pending.toText());
    try std.testing.expectEqualStrings("in-progress", Status.in_progress.toText());
    try std.testing.expectEqualStrings("done", Status.done.toText());
    try std.testing.expectEqualStrings("skipped", Status.skipped.toText());
}

test "add: happy path; ordinal=1 on first add; audit row recorded" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "First step" });
    defer deinit(s, a);

    try std.testing.expectEqual(plan_id, s.plan_id);
    try std.testing.expectEqual(@as(i64, 1), s.ordinal);
    try std.testing.expectEqualStrings("First step", s.body);
    try std.testing.expectEqual(Status.pending, s.status);
    try std.testing.expect(s.task_id == null);

    // Audit row recorded.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where entity_kind = 'plan_step' and verb = 'create'"),
    );
}

test "add: ordinal=N+1 on subsequent adds" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s1 = try add(&d, a, .{ .plan_id = plan_id, .body = "Step one" });
    defer deinit(s1, a);
    const s2 = try add(&d, a, .{ .plan_id = plan_id, .body = "Step two" });
    defer deinit(s2, a);
    const s3 = try add(&d, a, .{ .plan_id = plan_id, .body = "Step three" });
    defer deinit(s3, a);

    try std.testing.expectEqual(@as(i64, 1), s1.ordinal);
    try std.testing.expectEqual(@as(i64, 2), s2.ordinal);
    try std.testing.expectEqual(@as(i64, 3), s3.ordinal);
}

test "add: explicit ordinal honored" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Explicit", .ordinal = 42 });
    defer deinit(s, a);
    try std.testing.expectEqual(@as(i64, 42), s.ordinal);
}

test "add: duplicate ordinal returns OrdinalConflict" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s1 = try add(&d, a, .{ .plan_id = plan_id, .body = "First", .ordinal = 5 });
    defer deinit(s1, a);

    try std.testing.expectError(
        Error.OrdinalConflict,
        add(&d, a, .{ .plan_id = plan_id, .body = "Dup", .ordinal = 5 }),
    );
}

test "add: missing plan returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    try std.testing.expectError(
        Error.NotFound,
        add(&d, a, .{ .plan_id = 9999, .body = "Orphan" }),
    );
}

test "list: returns steps ordered by ordinal" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    // Insert out-of-order with explicit ordinals.
    const s3 = try add(&d, a, .{ .plan_id = plan_id, .body = "Third", .ordinal = 3 });
    defer deinit(s3, a);
    const s1 = try add(&d, a, .{ .plan_id = plan_id, .body = "First", .ordinal = 1 });
    defer deinit(s1, a);
    const s2 = try add(&d, a, .{ .plan_id = plan_id, .body = "Second", .ordinal = 2 });
    defer deinit(s2, a);

    const steps = try list(&d, a, plan_id);
    defer deinitMany(steps, a);

    try std.testing.expectEqual(@as(usize, 3), steps.len);
    try std.testing.expectEqual(@as(i64, 1), steps[0].ordinal);
    try std.testing.expectEqual(@as(i64, 2), steps[1].ordinal);
    try std.testing.expectEqual(@as(i64, 3), steps[2].ordinal);
    try std.testing.expectEqualStrings("First", steps[0].body);
    try std.testing.expectEqualStrings("Second", steps[1].body);
    try std.testing.expectEqualStrings("Third", steps[2].body);
}

test "show: happy path + NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Look up me" });
    defer deinit(s, a);

    const fetched = try show(&d, a, s.id);
    defer deinit(fetched, a);
    try std.testing.expectEqual(s.id, fetched.id);
    try std.testing.expectEqualStrings("Look up me", fetched.body);

    try std.testing.expectError(Error.NotFound, show(&d, a, 9999));
}

test "markDone: pending → done" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Do me" });
    defer deinit(s, a);
    try std.testing.expectEqual(Status.pending, s.status);

    const done = try markDone(&d, a, s.id);
    defer deinit(done, a);
    try std.testing.expectEqual(Status.done, done.status);

    // Audit row for status_change.
    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where entity_kind='plan_step' and verb='status_change'"),
    );
}

test "markDone: in-progress → done" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    // Manually set to in-progress.
    _ = try d.execParams(
        "insert into plan_steps (plan_id, ordinal, body, status) values (?, 1, 'active', 'in-progress')",
        &.{.{ .int = plan_id }},
    );
    const step_id = try d.intQuery("select max(id) from plan_steps");

    const done = try markDone(&d, a, step_id);
    defer deinit(done, a);
    try std.testing.expectEqual(Status.done, done.status);
}

test "markDone: already done returns InvalidTransition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Already done" });
    defer deinit(s, a);
    const d1 = try markDone(&d, a, s.id);
    defer deinit(d1, a);

    try std.testing.expectError(Error.InvalidTransition, markDone(&d, a, s.id));
}

test "markDone: skipped returns InvalidTransition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Skip me" });
    defer deinit(s, a);
    const sk = try skip(&d, a, s.id);
    defer deinit(sk, a);

    try std.testing.expectError(Error.InvalidTransition, markDone(&d, a, s.id));
}

test "skip: pending → skipped" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Skip me" });
    defer deinit(s, a);

    const skipped = try skip(&d, a, s.id);
    defer deinit(skipped, a);
    try std.testing.expectEqual(Status.skipped, skipped.status);
}

test "skip: terminal returns InvalidTransition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Already skipped" });
    defer deinit(s, a);
    const sk1 = try skip(&d, a, s.id);
    defer deinit(sk1, a);

    try std.testing.expectError(Error.InvalidTransition, skip(&d, a, s.id));
}

test "skip: in-progress returns InvalidTransition" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    _ = try d.execParams(
        "insert into plan_steps (plan_id, ordinal, body, status) values (?, 1, 'active', 'in-progress')",
        &.{.{ .int = plan_id }},
    );
    const step_id = try d.intQuery("select max(id) from plan_steps");

    try std.testing.expectError(Error.InvalidTransition, skip(&d, a, step_id));
}

test "linkTask: sets task_id; audit row" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const task_id = try insertTask(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Link me" });
    defer deinit(s, a);

    const linked = try linkTask(&d, a, s.id, task_id);
    defer deinit(linked, a);
    try std.testing.expectEqual(task_id, linked.task_id.?);

    try std.testing.expectEqual(
        @as(i64, 1),
        try d.intQuery("select count(*) from audit_log where entity_kind='plan_step' and verb='link'"),
    );
}

test "linkTask: missing task returns NotFound" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Link me" });
    defer deinit(s, a);

    try std.testing.expectError(Error.NotFound, linkTask(&d, a, s.id, 9999));
}

test "unlinkTask: clears task_id" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();

    const plan_id = try insertPlan(&d);
    const task_id = try insertTask(&d);
    const s = try add(&d, a, .{ .plan_id = plan_id, .body = "Link then unlink" });
    defer deinit(s, a);

    const linked = try linkTask(&d, a, s.id, task_id);
    defer deinit(linked, a);
    try std.testing.expectEqual(task_id, linked.task_id.?);

    const unlinked = try unlinkTask(&d, a, s.id);
    defer deinit(unlinked, a);
    try std.testing.expect(unlinked.task_id == null);
}
