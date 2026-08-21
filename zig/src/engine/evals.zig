//! engine/evals — routing evals aggregator (plan 898/904, tech-spec 520
//! Architecture item 5, D8).
//!
//! A read-only, deterministic aggregation over *completed* dispatch outcomes
//! that scores routing quality per (work-type, candidate model). It NEVER
//! writes anything — no routing-map mutation, no DB write (D8). Applying a
//! recommendation is a separate operator-gated action outside this module.
//!
//! Source of truth (D8, no new migration):
//!   - `session_entries` — the `dispatch_shape` / `model_choice` note
//!     convention (`agents/orchestrator.md` step 8a). `model_choice` carries
//!     a `{tier, candidate, work_type}` triple per dispatched task.
//!   - `agent_work_claims` — terminal claim status (`completed` / `aborted`
//!     / other) is the recoverable proxy for reviewer disposition. Note: the
//!     tech-spec's "agent_failure_categories (migration 00029)" is in fact
//!     `agent_work_claims.failure_category`, a column added by that
//!     migration — there is no separate `agent_failure_categories` table.
//!     This module does not currently consume `failure_category` directly;
//!     see "Signals actually recoverable" below.
//!   - `agent_actions` — `action_kind = 'test_coder'` rows scoped to the
//!     task (`entity_kind = 'task'`) give the test-coder expansion outcome.
//!
//! Signals actually recoverable (see D8's four named signals):
//!   - Reviewer disposition (approve / abort): recoverable, via the
//!     terminal `agent_work_claims.status` for the task's most recent claim
//!     (`completed` → approve, `aborted` → abort, anything else → other/
//!     inconclusive). `request-changes` is NOT a distinct terminal state
//!     anywhere in the schema — it never releases the claim (the same claim
//!     persists across loop-back iterations per
//!     `agents/methodology.md` step 6) — so it is only recoverable
//!     indirectly, as iteration count > 1.
//!   - Iteration count to approval: recoverable, as the count of dispatch
//!     notes (`session_entries` rows matching the `dispatch_shape:` /
//!     `model_choice:` convention) whose `model_choice` map names the task,
//!     since step 8a fires once per cycle and the claim token is stable
//!     across loop-back iterations.
//!   - Quality-gate pass/fail: NOT independently recorded as a discrete
//!     field anywhere in `session_entries`, `agent_actions`, or
//!     `agent_work_claims`. Gate output is pasted into the coder's report to
//!     the reviewer but never persisted as a stored pass/fail signal. This
//!     module does not score it and reports `quality_gate_pass_fail: false`
//!     in `signals_sourced` rather than fabricating a proxy.
//!   - Test-coder expansion outcome: recoverable, via the most recent
//!     `agent_actions` row with `action_kind = 'test_coder'` and
//!     `entity_kind = 'task'` scoped to the task; `outcome = 'ok'` counts as
//!     a clean expansion, anything else counts against it.
//!
//! insufficient-data: a (work-type, candidate) pair with zero dispatch
//! history never gets a fabricated score. To surface it at all (rather than
//! silently omitting it), the aggregator enumerates "sibling" candidates —
//! every candidate in the same `models.<vendor>.<tier>` effective candidate
//! list as an *observed* (work-type, candidate) pair, for every work type
//! actually observed at that (vendor, tier) — and reports any sibling with
//! no dispatch history as `insufficient_data: true`. Vendor is recovered by
//! matching the candidate id against the curated catalog (`models.zig`); a
//! custom candidate id absent from the catalog cannot be attributed to a
//! vendor and is excluded from sibling enumeration (still scored on its own
//! observed history, just not cross-referenced against config).
//!
//! Determinism: no `Date.now`, no randomness. All ordering is by
//! `session_entries.id` (insertion order) for the scan pass and by explicit
//! sort keys (work_type, approval_rate desc, avg_iterations asc, candidate)
//! for the output. Two runs against the same DB state always produce
//! byte-identical output.

const std = @import("std");
const db = @import("db");
const config = @import("config.zig");
const models = @import("models.zig");

pub const Error = error{QueryFailed} || std.mem.Allocator.Error;

/// Which of D8's four named signals this module actually sources from
/// recorded data, vs. reports as unavailable. See the module doc comment.
pub const SignalsSourced = struct {
    reviewer_disposition: bool = true,
    iteration_count: bool = true,
    quality_gate_pass_fail: bool = false,
    test_coder_expansion: bool = true,
};

/// One per-(work-type, candidate) scorecard row.
pub const ScoreRow = struct {
    work_type: []const u8,
    candidate: []const u8,
    /// Resolved from the curated model catalog by candidate id; null when
    /// the candidate id is not in the catalog (custom/operator-added id).
    vendor: ?[]const u8 = null,
    tier: []const u8,
    dispatch_count: usize = 0,
    approved_count: usize = 0,
    aborted_count: usize = 0,
    other_count: usize = 0,
    approval_rate: f64 = 0,
    avg_iterations: f64 = 0,
    test_coder_ok_count: usize = 0,
    test_coder_other_count: usize = 0,
    insufficient_data: bool,
    /// 1-based rank within this work type among scored (non-insufficient)
    /// rows; null for insufficient-data rows.
    rank: ?usize = null,
};

/// A recommended routing-map change (D8: preview only, writes nothing).
pub const Recommendation = struct {
    work_type: []const u8,
    vendor: ?[]const u8 = null,
    tier: []const u8,
    candidate: []const u8,
    rationale: []const u8,
};

pub const Result = struct {
    scorecard: []ScoreRow = &.{},
    recommendations: []Recommendation = &.{},
    signals_sourced: SignalsSourced = .{},
    /// Distinct tasks whose dispatch notes never carried a complete
    /// {tier, candidate, work_type} triple (pre-convention notes, or
    /// malformed `model_choice` JSON) — excluded from all scoring rather
    /// than attributed to a guessed work type.
    legacy_dispatch_notes_skipped: usize = 0,

    pub fn deinit(self: *Result, allocator: std.mem.Allocator) void {
        for (self.scorecard) |row| {
            allocator.free(row.work_type);
            allocator.free(row.candidate);
            if (row.vendor) |v| allocator.free(v);
            allocator.free(row.tier);
        }
        allocator.free(self.scorecard);
        for (self.recommendations) |rec| {
            allocator.free(rec.work_type);
            if (rec.vendor) |v| allocator.free(v);
            allocator.free(rec.tier);
            allocator.free(rec.candidate);
            allocator.free(rec.rationale);
        }
        allocator.free(self.recommendations);
        self.* = .{};
    }
};

// =========================================================================
// Internal scratch types (arena-scoped)
// =========================================================================

const TaskInfo = struct {
    work_type: []const u8 = "",
    candidate: []const u8 = "",
    tier: []const u8 = "",
    iterations: usize = 0,
    has_info: bool = false,
};

const ClaimOutcome = enum { approved, aborted, other, none };
const TestCoderOutcome = enum { ok, other };

const GroupAccum = struct {
    work_type: []const u8,
    candidate: []const u8,
    tier: []const u8,
    dispatch_count: usize = 0,
    approved_count: usize = 0,
    aborted_count: usize = 0,
    other_count: usize = 0,
    total_iterations: usize = 0,
    test_coder_ok: usize = 0,
    test_coder_other: usize = 0,
};

/// The vendor a candidate was actually dispatched on, read from the claim that
/// recorded it. Planar does not infer this from a catalog (plan 950 removed
/// the catalog, and inferring a vendor is a support claim Planar does not
/// make): agents report `--vendor` alongside `--model` when they claim work,
/// so this reads back what was recorded. Null when no claim carries that
/// model string.
fn vendorForCandidate(d: *db.sqlite.Db, arena: std.mem.Allocator, id: []const u8) ?[]const u8 {
    const sql: [:0]const u8 =
        \\select vendor from agent_work_claims
        \\where model = ?
        \\order by claimed_at desc, id desc limit 1
    ;
    var stmt = d.prepare(sql) catch return null;
    defer stmt.finalize();
    stmt.bind(&.{.{ .text = id }}) catch return null;
    const step = stmt.step() catch return null;
    if (step == .done) return null;
    return stmt.columnTextAlloc(0, arena) catch null;
}

fn stringField(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const v = obj.get(key) orelse return null;
    return switch (v) {
        .string => |s| s,
        else => null,
    };
}

/// Parse one dispatch note's `model_choice:` line (if present) and fold the
/// per-task info into `task_map`. Any parse failure is a silent skip — a
/// malformed dispatch note is not a hard error (per test-spec: "malformed
/// dispatch notes are skipped, not surfaced as a hard error in v1").
fn processDispatchBody(
    arena: std.mem.Allocator,
    body: []const u8,
    task_map: *std.AutoHashMapUnmanaged(i64, TaskInfo),
) !void {
    var lines = std.mem.splitScalar(u8, body, '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, trimmed, "model_choice:")) continue;
        const json_part = std.mem.trim(u8, trimmed["model_choice:".len..], " \t");
        if (json_part.len == 0) return;

        var parsed = std.json.parseFromSlice(std.json.Value, arena, json_part, .{
            .allocate = .alloc_always,
        }) catch return;
        defer parsed.deinit();
        if (parsed.value != .object) return;

        var it = parsed.value.object.iterator();
        while (it.next()) |entry| {
            const task_id = std.fmt.parseInt(i64, entry.key_ptr.*, 10) catch continue;
            const gop = try task_map.getOrPut(arena, task_id);
            if (!gop.found_existing) gop.value_ptr.* = .{};
            gop.value_ptr.iterations += 1;

            if (entry.value_ptr.* != .object) continue;
            const eo = entry.value_ptr.*.object;
            const work_type = stringField(eo, "work_type") orelse continue;
            const candidate = stringField(eo, "candidate") orelse continue;
            const tier = stringField(eo, "tier") orelse continue;
            if (work_type.len == 0 or candidate.len == 0 or tier.len == 0) continue;

            // Last-note-wins for the {tier, candidate, work_type} triple:
            // arena-owned strings from this parse outlive the function via
            // the shared arena, so referencing them directly is safe.
            gop.value_ptr.work_type = try arena.dupe(u8, work_type);
            gop.value_ptr.candidate = try arena.dupe(u8, candidate);
            gop.value_ptr.tier = try arena.dupe(u8, tier);
            gop.value_ptr.has_info = true;
        }
        return; // only one model_choice line per note body
    }
}

fn scanDispatchNotes(
    d: *db.sqlite.Db,
    arena: std.mem.Allocator,
    task_map: *std.AutoHashMapUnmanaged(i64, TaskInfo),
) Error!void {
    const sql: [:0]const u8 =
        \\select body from session_entries
        \\where prefix = 'note' and body like '%dispatch_shape:%'
        \\order by id
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    while (true) {
        const step = stmt.step() catch return Error.QueryFailed;
        if (step == .done) break;
        const body = stmt.columnTextAlloc(0, arena) catch return Error.QueryFailed;
        processDispatchBody(arena, body, task_map) catch continue;
    }
}

fn resolveClaimOutcome(d: *db.sqlite.Db, arena: std.mem.Allocator, task_id: i64) Error!ClaimOutcome {
    const sql: [:0]const u8 =
        \\select status from agent_work_claims
        \\where entity_kind = 'task' and entity_id = ?
        \\order by claimed_at desc, id desc limit 1
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return .none;
    const status = stmt.columnTextAlloc(0, arena) catch return Error.QueryFailed;
    if (std.mem.eql(u8, status, "completed")) return .approved;
    if (std.mem.eql(u8, status, "aborted")) return .aborted;
    return .other;
}

fn resolveTestCoderOutcome(d: *db.sqlite.Db, arena: std.mem.Allocator, task_id: i64) Error!?TestCoderOutcome {
    const sql: [:0]const u8 =
        \\select outcome from agent_actions
        \\where action_kind = 'test_coder' and entity_kind = 'task' and entity_id = ?
        \\order by started_at desc, id desc limit 1
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{.{ .int = task_id }}) catch return Error.QueryFailed;
    const step = stmt.step() catch return Error.QueryFailed;
    if (step == .done) return null;
    const outcome = stmt.columnTextOpt(0, arena) catch return Error.QueryFailed;
    const o = outcome orelse return null;
    return if (std.mem.eql(u8, o, "ok")) .ok else .other;
}

fn groupKey(arena: std.mem.Allocator, work_type: []const u8, candidate: []const u8) ![]const u8 {
    return std.fmt.allocPrint(arena, "{s}\x00{s}", .{ work_type, candidate });
}

fn lessThan(_: void, a: ScoreRow, b: ScoreRow) bool {
    const wt = std.mem.order(u8, a.work_type, b.work_type);
    if (wt != .eq) return wt == .lt;
    if (a.insufficient_data != b.insufficient_data) return !a.insufficient_data;
    if (!a.insufficient_data) {
        if (a.approval_rate != b.approval_rate) return a.approval_rate > b.approval_rate;
        if (a.avg_iterations != b.avg_iterations) return a.avg_iterations < b.avg_iterations;
    }
    return std.mem.order(u8, a.candidate, b.candidate) == .lt;
}

/// Build the routing-evals scorecard + recommendations. Read-only: opens no
/// transaction, writes nothing to the database or the filesystem (D8).
pub fn aggregate(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
) Error!Result {
    var arena_state = std.heap.ArenaAllocator.init(allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    var task_map: std.AutoHashMapUnmanaged(i64, TaskInfo) = .empty;
    try scanDispatchNotes(d, arena, &task_map);

    var groups: std.StringHashMapUnmanaged(GroupAccum) = .empty;
    var legacy_skipped: usize = 0;

    var task_it = task_map.iterator();
    while (task_it.next()) |task_entry| {
        const task_id = task_entry.key_ptr.*;
        const info = task_entry.value_ptr.*;
        if (!info.has_info) {
            legacy_skipped += 1;
            continue;
        }

        const outcome = try resolveClaimOutcome(d, arena, task_id);
        const tc_outcome = try resolveTestCoderOutcome(d, arena, task_id);

        const key = try groupKey(arena, info.work_type, info.candidate);
        const gop = try groups.getOrPut(arena, key);
        if (!gop.found_existing) {
            gop.value_ptr.* = .{ .work_type = info.work_type, .candidate = info.candidate, .tier = info.tier };
        }
        gop.value_ptr.dispatch_count += 1;
        gop.value_ptr.total_iterations += info.iterations;
        switch (outcome) {
            .approved => gop.value_ptr.approved_count += 1,
            .aborted => gop.value_ptr.aborted_count += 1,
            .other, .none => gop.value_ptr.other_count += 1,
        }
        if (tc_outcome) |tc| switch (tc) {
            .ok => gop.value_ptr.test_coder_ok += 1,
            .other => gop.value_ptr.test_coder_other += 1,
        };
    }

    // Scored rows, referencing arena-owned strings for now.
    var rows: std.ArrayList(ScoreRow) = .empty;
    var group_it = groups.valueIterator();
    while (group_it.next()) |g| {
        const dispatch_count_f: f64 = @floatFromInt(g.dispatch_count);
        try rows.append(arena, .{
            .work_type = g.work_type,
            .candidate = g.candidate,
            .vendor = vendorForCandidate(d, arena, g.candidate),
            .tier = g.tier,
            .dispatch_count = g.dispatch_count,
            .approved_count = g.approved_count,
            .aborted_count = g.aborted_count,
            .other_count = g.other_count,
            .approval_rate = @as(f64, @floatFromInt(g.approved_count)) / dispatch_count_f,
            .avg_iterations = @as(f64, @floatFromInt(g.total_iterations)) / dispatch_count_f,
            .test_coder_ok_count = g.test_coder_ok,
            .test_coder_other_count = g.test_coder_other,
            .insufficient_data = false,
        });
    }

    // Sibling enumeration was removed with Planar's model catalog (plan 950):
    // enumerating "candidates with no history" required a config-derived list
    // of what exists, which is exactly the support claim Planar no longer
    // makes. Rows now cover only candidates with observed dispatch history.

    std.mem.sort(ScoreRow, rows.items, {}, lessThan);

    // Assign per-work-type rank among scored rows (insufficient rows sort
    // after scored rows within the same work type, per lessThan).
    {
        var current_wt: ?[]const u8 = null;
        var rank_counter: usize = 0;
        for (rows.items) |*row| {
            if (current_wt == null or !std.mem.eql(u8, current_wt.?, row.work_type)) {
                current_wt = row.work_type;
                rank_counter = 0;
            }
            if (row.insufficient_data) {
                row.rank = null;
            } else {
                rank_counter += 1;
                row.rank = rank_counter;
            }
        }
    }

    // Recommendations: the rank-1 row for every work type that has at
    // least one scored row.
    var recs: std.ArrayList(Recommendation) = .empty;
    for (rows.items) |row| {
        if (row.rank != null and row.rank.? == 1) {
            const rationale = try std.fmt.allocPrint(
                arena,
                "{d}/{d} approved, avg {d:.2} iterations to approval (highest-ranked scored candidate for {s})",
                .{ row.approved_count, row.dispatch_count, row.avg_iterations, row.work_type },
            );
            try recs.append(arena, .{
                .work_type = row.work_type,
                .vendor = row.vendor,
                .tier = row.tier,
                .candidate = row.candidate,
                .rationale = rationale,
            });
        }
    }

    // Final pass: dupe every string into the caller's (non-arena) allocator
    // so the Result survives the arena's deinit.
    var out_rows = try allocator.alloc(ScoreRow, rows.items.len);
    var built: usize = 0;
    errdefer {
        for (out_rows[0..built]) |row| {
            allocator.free(row.work_type);
            allocator.free(row.candidate);
            if (row.vendor) |v| allocator.free(v);
            allocator.free(row.tier);
        }
        allocator.free(out_rows);
    }
    for (rows.items, 0..) |row, i| {
        out_rows[i] = .{
            .work_type = try allocator.dupe(u8, row.work_type),
            .candidate = try allocator.dupe(u8, row.candidate),
            .vendor = if (row.vendor) |v| try allocator.dupe(u8, v) else null,
            .tier = try allocator.dupe(u8, row.tier),
            .dispatch_count = row.dispatch_count,
            .approved_count = row.approved_count,
            .aborted_count = row.aborted_count,
            .other_count = row.other_count,
            .approval_rate = row.approval_rate,
            .avg_iterations = row.avg_iterations,
            .test_coder_ok_count = row.test_coder_ok_count,
            .test_coder_other_count = row.test_coder_other_count,
            .insufficient_data = row.insufficient_data,
            .rank = row.rank,
        };
        built += 1;
    }

    var out_recs = try allocator.alloc(Recommendation, recs.items.len);
    var recs_built: usize = 0;
    errdefer {
        for (out_recs[0..recs_built]) |rec| {
            allocator.free(rec.work_type);
            if (rec.vendor) |v| allocator.free(v);
            allocator.free(rec.tier);
            allocator.free(rec.candidate);
            allocator.free(rec.rationale);
        }
        allocator.free(out_recs);
    }
    for (recs.items, 0..) |rec, i| {
        out_recs[i] = .{
            .work_type = try allocator.dupe(u8, rec.work_type),
            .vendor = if (rec.vendor) |v| try allocator.dupe(u8, v) else null,
            .tier = try allocator.dupe(u8, rec.tier),
            .candidate = try allocator.dupe(u8, rec.candidate),
            .rationale = try allocator.dupe(u8, rec.rationale),
        };
        recs_built += 1;
    }

    return .{
        .scorecard = out_rows,
        .recommendations = out_recs,
        .signals_sourced = .{},
        .legacy_dispatch_notes_skipped = legacy_skipped,
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

fn emptyEffective() config.EffectiveMap {
    return .empty;
}

test "aggregate: no dispatch history at all yields empty scorecard, no recommendations" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var eff = emptyEffective();
    defer eff.deinit(a);

    var result = try aggregate(&d, a);
    defer result.deinit(a);

    try std.testing.expectEqual(@as(usize, 0), result.scorecard.len);
    try std.testing.expectEqual(@as(usize, 0), result.recommendations.len);
    try std.testing.expectEqual(@as(usize, 0), result.legacy_dispatch_notes_skipped);
    try std.testing.expect(!result.signals_sourced.quality_gate_pass_fail);
    try std.testing.expect(result.signals_sourced.reviewer_disposition);
    try std.testing.expect(result.signals_sourced.iteration_count);
    try std.testing.expect(result.signals_sourced.test_coder_expansion);
}

test "aggregate: a dispatch note missing work_type is skipped as legacy, not scored" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var eff = emptyEffective();
    defer eff.deinit(a);

    const sid = try d.execParams("insert into sessions (vendor) values ('claude')", &.{});
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"1\":{\"tier\":\"medium\",\"candidate\":\"claude-sonnet-5\"}}" } },
    );

    var result = try aggregate(&d, a);
    defer result.deinit(a);

    try std.testing.expectEqual(@as(usize, 0), result.scorecard.len);
    try std.testing.expectEqual(@as(usize, 1), result.legacy_dispatch_notes_skipped);
}

test "aggregate: approved task scores approval_rate 1.0 with a recommendation" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var eff = emptyEffective();
    defer eff.deinit(a);

    const sid = try d.execParams("insert into sessions (vendor) values ('claude')", &.{});
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"1\":{\"tier\":\"medium\",\"candidate\":\"claude-sonnet-5\",\"work_type\":\"schema\"}}" } },
    );
    _ = try d.execParams(
        "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, model, lease_expires_at) " ++
            "values ('tok1', ?, 'task', 1, 'completed', 'claude', 'claude-sonnet-5', datetime('now','+1 hour'))",
        &.{.{ .int = sid }},
    );

    var result = try aggregate(&d, a);
    defer result.deinit(a);

    try std.testing.expectEqual(@as(usize, 1), result.scorecard.len);
    const row = result.scorecard[0];
    try std.testing.expectEqualStrings("schema", row.work_type);
    try std.testing.expectEqualStrings("claude-sonnet-5", row.candidate);
    try std.testing.expectEqualStrings("claude", row.vendor.?);
    try std.testing.expectEqual(@as(usize, 1), row.dispatch_count);
    try std.testing.expectEqual(@as(usize, 1), row.approved_count);
    try std.testing.expectEqual(@as(f64, 1.0), row.approval_rate);
    try std.testing.expect(!row.insufficient_data);
    try std.testing.expectEqual(@as(?usize, 1), row.rank);

    try std.testing.expectEqual(@as(usize, 1), result.recommendations.len);
    try std.testing.expectEqualStrings("claude-sonnet-5", result.recommendations[0].candidate);
}

test "aggregate: two candidates for the same work type — higher approval/lower iterations ranks first" {
    const a = std.testing.allocator;
    var d = try setupTestDb(a);
    defer d.close();
    var eff = emptyEffective();
    defer eff.deinit(a);

    const sid = try d.execParams("insert into sessions (vendor) values ('claude')", &.{});

    // Task 1: candidate A, one cycle, approved.
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 1, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"1\":{\"tier\":\"large\",\"candidate\":\"claude-opus-4-8\",\"work_type\":\"schema\"}}" } },
    );
    _ = try d.execParams(
        "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, model, lease_expires_at) " ++
            "values ('tok1', ?, 'task', 1, 'completed', 'claude', 'claude-sonnet-5', datetime('now','+1 hour'))",
        &.{.{ .int = sid }},
    );

    // Task 2: candidate B, three cycles (request-changes twice), then aborted.
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 2, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"2\":{\"tier\":\"large\",\"candidate\":\"claude-haiku-4-5\",\"work_type\":\"schema\"}}" } },
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 3, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"2\":{\"tier\":\"large\",\"candidate\":\"claude-haiku-4-5\",\"work_type\":\"schema\"}}" } },
    );
    _ = try d.execParams(
        "insert into session_entries (session_id, ordinal, prefix, body) values (?, 4, 'note', ?)",
        &.{ .{ .int = sid }, .{ .text = "dispatch_shape: strict\nmodel_choice: {\"2\":{\"tier\":\"large\",\"candidate\":\"claude-haiku-4-5\",\"work_type\":\"schema\"}}" } },
    );
    _ = try d.execParams(
        "insert into agent_work_claims (claim_token, session_id, entity_kind, entity_id, status, vendor, model, lease_expires_at) " ++
            "values ('tok2', ?, 'task', 2, 'aborted', 'claude', 'claude-sonnet-5', datetime('now','+1 hour'))",
        &.{.{ .int = sid }},
    );

    var result = try aggregate(&d, a);
    defer result.deinit(a);

    try std.testing.expectEqual(@as(usize, 2), result.scorecard.len);
    try std.testing.expectEqualStrings("claude-opus-4-8", result.scorecard[0].candidate);
    try std.testing.expectEqual(@as(?usize, 1), result.scorecard[0].rank);
    try std.testing.expectEqualStrings("claude-haiku-4-5", result.scorecard[1].candidate);
    try std.testing.expectEqual(@as(?usize, 2), result.scorecard[1].rank);
    try std.testing.expectEqual(@as(usize, 1), result.scorecard[1].dispatch_count);
    try std.testing.expectEqual(@as(f64, 3.0), result.scorecard[1].avg_iterations);

    try std.testing.expectEqual(@as(usize, 1), result.recommendations.len);
    try std.testing.expectEqualStrings("claude-opus-4-8", result.recommendations[0].candidate);
}
