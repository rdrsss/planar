//! engine/introspect — diagnostic bundle aggregation for `planar report`.
//!
//! Provides the `Bundle` struct and the aggregate-only query functions that
//! build it. All queries in this module are structurally redacted by
//! construction: they select ONLY counts, categories, verb paths, statuses,
//! and timestamps — never entity title/body/summary, scope slugs, or any
//! path-bearing column. This is the load-bearing privacy decision: no query
//! below can leak entity text into the bundle even if a caller forgets to
//! sanitize the output.
//!
//! Tables read:
//!   cli_invocations   — verb-path + outcome aggregates; failure tail rows.
//!   agent_actions     — agent-action outcome aggregates.
//!   sync_events       — sync-event outcome aggregates.
//!   task_reopens      — reopen count aggregate (Bundle.reopens field).
//!   agent_work_claims — stale-claim count.
//!   handoffs          — stale-handoff count and never-consumed-handoff count.
//!
//! Tables never read (redaction invariant):
//!   tasks, questions, scenarios, decisions, artifacts, plans,
//!   projects, project_associations — any column carrying entity text.

const std = @import("std");
const db = @import("db");

// =========================================================================
// Bundle types
// =========================================================================

/// One verb-path invocation aggregate row.
pub const VerbCount = struct {
    verb_path: []const u8,
    count: i64,
    success_count: i64,
    failure_count: i64,
};

/// One error-category failure aggregate row.
pub const FailureCategory = struct {
    category: []const u8,
    count: i64,
};

/// One agent-action outcome aggregate row.
pub const ActionOutcome = struct {
    action_kind: []const u8,
    outcome: []const u8,
    count: i64,
};

/// One sync-event outcome aggregate row.
pub const SyncOutcome = struct {
    outcome: []const u8,
    count: i64,
};

/// One failure-tail row (most-recent failed invocations).
/// Contains only: verb_path, error_category, exit_code, recorded_at.
/// Structurally redacted — no entity text or scope slugs.
pub const FailureTailRow = struct {
    verb_path: []const u8,
    error_category: []const u8,
    exit_code: i64,
    recorded_at: []const u8,
};

/// Claim aggregate counts.
pub const ClaimCounts = struct {
    /// Active claims older than the stale threshold (24 h).
    stale_claims: i64,
    /// Total claims that expired without being consumed.
    never_consumed: i64,
};

/// Handoff aggregate counts.
pub const HandoffCounts = struct {
    /// Pending/validated handoffs older than 24 h.
    stale_handoffs: i64,
    /// Handoffs that were never consumed (remained 'pending' > 24 h then expired, or still pending).
    never_consumed: i64,
};

/// The complete diagnostic bundle returned by `build`.
/// Field order is the `--json` wire format (spec: version, schema_version,
/// health, window, invocations, failures, actions, sync, claims, handoffs).
pub const Bundle = struct {
    /// Binary version string (embed from build options; "unknown" as default).
    version: []const u8,
    /// Max schema_migrations version currently applied.
    schema_version: i64,
    /// Health summary string: "ok" or "degraded".
    health: []const u8,
    /// Window in days that was queried.
    window_days: i64,
    /// Whether CLI logging is enabled (affects invocations/failures sections).
    logging_enabled: bool,
    /// Invocation counts by verb_path (empty when logging disabled).
    invocations: []VerbCount,
    /// Failure counts by error_category (empty when logging disabled).
    failures: []FailureCategory,
    /// Agent-action outcome aggregates (always-on).
    actions: []ActionOutcome,
    /// Sync-event outcome aggregates (always-on).
    sync: []SyncOutcome,
    /// Claim aggregate counts (always-on).
    claims: ClaimCounts,
    /// Handoff aggregate counts (always-on).
    handoffs: HandoffCounts,
    /// Count of task reopen events in the window (always-on).
    reopens: i64,
    /// Most-recent failed invocations tail (empty when logging disabled).
    failure_tail: []FailureTailRow,

    pub fn deinit(self: *Bundle, allocator: std.mem.Allocator) void {
        allocator.free(self.version);
        allocator.free(self.health);
        for (self.invocations) |*v| allocator.free(v.verb_path);
        allocator.free(self.invocations);
        for (self.failures) |*f| allocator.free(f.category);
        allocator.free(self.failures);
        for (self.actions) |*a| {
            allocator.free(a.action_kind);
            allocator.free(a.outcome);
        }
        allocator.free(self.actions);
        for (self.sync) |*s| allocator.free(s.outcome);
        allocator.free(self.sync);
        for (self.failure_tail) |*r| {
            allocator.free(r.verb_path);
            allocator.free(r.error_category);
            allocator.free(r.recorded_at);
        }
        allocator.free(self.failure_tail);
    }
};

// =========================================================================
// Main entry point
// =========================================================================

pub const Error = error{
    QueryFailed,
    OutOfMemory,
};

/// Build the diagnostic bundle for the given window and tail size.
///
/// `d`:              open database connection.
/// `allocator`:      owns all strings in the returned Bundle.
/// `window_days`:    how many days back to query (must be > 0; caller
///                   validates before calling).
/// `tail_n`:         how many failure-tail rows to return (must be > 0).
/// `logging_enabled`:whether `[introspection].cli_log` is on. When false
///                   the invocation/failure sections are left empty so the
///                   caller can render "logging disabled" instead of zeros.
/// `db_path`:        borrowed string for the health report.
///
/// Caller must call `bundle.deinit(allocator)` when done.
pub fn build(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
    tail_n: i64,
    logging_enabled: bool,
    db_path: []const u8,
) Error!Bundle {
    const version = allocator.dupe(u8, "planar") catch return error.OutOfMemory;
    errdefer allocator.free(version);

    const schema_version = d.intQuery(
        "select coalesce(max(version), 0) from schema_migrations",
    ) catch 0;

    // Health summary (reuse health module logic cheaply).
    const health_str = healthSummary(d, db_path);
    const health = allocator.dupe(u8, health_str) catch return error.OutOfMemory;
    errdefer allocator.free(health);

    // Invocations + failures: only when logging is enabled.
    var invocations: []VerbCount = &.{};
    var failures: []FailureCategory = &.{};
    var failure_tail: []FailureTailRow = &.{};

    if (logging_enabled) {
        invocations = queryInvocations(d, allocator, window_days) catch return error.QueryFailed;
        failures = queryFailureCategories(d, allocator, window_days) catch {
            for (invocations) |*v| allocator.free(v.verb_path);
            allocator.free(invocations);
            return error.QueryFailed;
        };
        failure_tail = queryFailureTail(d, allocator, window_days, tail_n) catch {
            for (invocations) |*v| allocator.free(v.verb_path);
            allocator.free(invocations);
            for (failures) |*f| allocator.free(f.category);
            allocator.free(failures);
            return error.QueryFailed;
        };
    }
    errdefer {
        for (invocations) |*v| allocator.free(v.verb_path);
        allocator.free(invocations);
        for (failures) |*f| allocator.free(f.category);
        allocator.free(failures);
        for (failure_tail) |*r| {
            allocator.free(r.verb_path);
            allocator.free(r.error_category);
            allocator.free(r.recorded_at);
        }
        allocator.free(failure_tail);
    }

    // Always-on sections.
    const actions = queryActionOutcomes(d, allocator, window_days) catch return error.QueryFailed;
    errdefer {
        for (actions) |*a| {
            allocator.free(a.action_kind);
            allocator.free(a.outcome);
        }
        allocator.free(actions);
    }

    const sync_outcomes = querySyncOutcomes(d, allocator, window_days) catch return error.QueryFailed;
    errdefer {
        for (sync_outcomes) |*s| allocator.free(s.outcome);
        allocator.free(sync_outcomes);
    }

    const claims = queryClaimCounts(d, window_days);
    const handoffs = queryHandoffCounts(d, window_days);
    const reopens = queryReopenCount(d, window_days);

    return .{
        .version = version,
        .schema_version = schema_version,
        .health = health,
        .window_days = window_days,
        .logging_enabled = logging_enabled,
        .invocations = invocations,
        .failures = failures,
        .actions = actions,
        .sync = sync_outcomes,
        .claims = claims,
        .handoffs = handoffs,
        .reopens = reopens,
        .failure_tail = failure_tail,
    };
}

// =========================================================================
// Health summary (lightweight — avoids importing the full health module)
// =========================================================================

fn healthSummary(d: *db.sqlite.Db, _: []const u8) []const u8 {
    // Degraded if not_resumable_tasks > 0 or stale_handoffs > 0.
    const inflight = d.intQuery(
        "select count(*) from tasks where status in ('doing','blocked')",
    ) catch return "degraded";

    const resumable = d.intQuery(
        "select count(*) from tasks t" ++
            " where t.status in ('doing','blocked')" ++
            "   and coalesce(t.next_action,'') != ''" ++
            "   and exists (select 1 from context_snapshots cs where cs.task_id = t.id)",
    ) catch 0;

    const not_resumable = inflight - resumable;

    const stale_handoffs = d.intQuery(
        "select count(*) from handoffs" ++
            " where status in ('pending','validated')" ++
            "   and (julianday('now') - julianday(created_at)) * 24 > 24",
    ) catch 0;

    if (not_resumable > 0 or stale_handoffs > 0) return "degraded";
    return "ok";
}

// =========================================================================
// Invocation aggregates
// =========================================================================

/// Query cli_invocations: count per verb_path + success/failure breakdown.
/// Window: rows within the last `window_days` days.
/// Privacy: selects only verb_path and counts. No title/body/summary/scope.
fn queryInvocations(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
) ![]VerbCount {
    var buf: [512]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select verb_path," ++
            " count(*) as total," ++
            " sum(case when exit_code = 0 then 1 else 0 end) as successes," ++
            " sum(case when exit_code != 0 then 1 else 0 end) as failures" ++
            " from cli_invocations" ++
            " where recorded_at >= datetime('now', '-{d} days')" ++
            " group by verb_path" ++
            " order by total desc",
        .{window_days},
    ) catch return error.OutOfMemory;
    const sql = try allocator.dupeZ(u8, sql_str);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return error.PrepareFailed;
    defer stmt.finalize();

    var list: std.ArrayList(VerbCount) = .empty;
    errdefer {
        for (list.items) |*v| allocator.free(v.verb_path);
        list.deinit(allocator);
    }

    while (true) {
        const row = stmt.step() catch return error.StepFailed;
        if (row == .done) break;

        const verb_path = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(verb_path);

        const total = stmt.columnInt(1);
        const success_count = stmt.columnInt(2);
        const failure_count = stmt.columnInt(3);

        try list.append(allocator, .{
            .verb_path = verb_path,
            .count = total,
            .success_count = success_count,
            .failure_count = failure_count,
        });
    }

    return list.toOwnedSlice(allocator);
}

/// Query cli_invocations: count per error_category for failed rows.
/// Privacy: selects only error_category and count. No entity text.
fn queryFailureCategories(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
) ![]FailureCategory {
    var buf: [512]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select coalesce(error_category, 'unknown'), count(*)" ++
            " from cli_invocations" ++
            " where exit_code != 0" ++
            "   and recorded_at >= datetime('now', '-{d} days')" ++
            " group by error_category" ++
            " order by count(*) desc",
        .{window_days},
    ) catch return error.OutOfMemory;
    const sql = try allocator.dupeZ(u8, sql_str);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return error.PrepareFailed;
    defer stmt.finalize();

    var list: std.ArrayList(FailureCategory) = .empty;
    errdefer {
        for (list.items) |*f| allocator.free(f.category);
        list.deinit(allocator);
    }

    while (true) {
        const row = stmt.step() catch return error.StepFailed;
        if (row == .done) break;
        const category = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(category);
        const count = stmt.columnInt(1);
        try list.append(allocator, .{ .category = category, .count = count });
    }

    return list.toOwnedSlice(allocator);
}

// =========================================================================
// Failure tail
// =========================================================================

/// Query the last `tail_n` failed invocations within the window, newest first.
/// Returns: verb_path, error_category, exit_code, recorded_at ONLY.
/// Privacy: no entity text, no scope slug, no args_shape.
fn queryFailureTail(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
    tail_n: i64,
) ![]FailureTailRow {
    var buf: [768]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select verb_path, coalesce(error_category,'unknown'), exit_code, recorded_at" ++
            " from cli_invocations" ++
            " where exit_code != 0" ++
            "   and recorded_at >= datetime('now', '-{d} days')" ++
            " order by recorded_at desc" ++
            " limit {d}",
        .{ window_days, tail_n },
    ) catch return error.OutOfMemory;
    const sql = try allocator.dupeZ(u8, sql_str);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return error.PrepareFailed;
    defer stmt.finalize();

    var list: std.ArrayList(FailureTailRow) = .empty;
    errdefer {
        for (list.items) |*r| {
            allocator.free(r.verb_path);
            allocator.free(r.error_category);
            allocator.free(r.recorded_at);
        }
        list.deinit(allocator);
    }

    while (true) {
        const row = stmt.step() catch return error.StepFailed;
        if (row == .done) break;

        const verb_path = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(verb_path);
        const error_category = try stmt.columnTextAlloc(1, allocator);
        errdefer allocator.free(error_category);
        const exit_code = stmt.columnInt(2);
        const recorded_at = try stmt.columnTextAlloc(3, allocator);
        errdefer allocator.free(recorded_at);

        try list.append(allocator, .{
            .verb_path = verb_path,
            .error_category = error_category,
            .exit_code = exit_code,
            .recorded_at = recorded_at,
        });
    }

    return list.toOwnedSlice(allocator);
}

// =========================================================================
// Always-on aggregates
// =========================================================================

/// Agent-action outcome aggregates from `agent_actions`.
/// Privacy: selects only action_kind, outcome, count. No entity text.
fn queryActionOutcomes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
) ![]ActionOutcome {
    // agent_actions may not exist in older schemas; fail open → empty slice.
    var buf: [512]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select action_kind, coalesce(outcome,'unknown'), count(*)" ++
            " from agent_actions" ++
            " where started_at >= datetime('now', '-{d} days')" ++
            " group by action_kind, outcome" ++
            " order by count(*) desc",
        .{window_days},
    ) catch return error.OutOfMemory;
    const sql = try allocator.dupeZ(u8, sql_str);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return &.{};
    defer stmt.finalize();

    var list: std.ArrayList(ActionOutcome) = .empty;
    errdefer {
        for (list.items) |*a| {
            allocator.free(a.action_kind);
            allocator.free(a.outcome);
        }
        list.deinit(allocator);
    }

    while (true) {
        const row = stmt.step() catch break;
        if (row == .done) break;
        const action_kind = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(action_kind);
        const outcome = try stmt.columnTextAlloc(1, allocator);
        errdefer allocator.free(outcome);
        const count = stmt.columnInt(2);
        try list.append(allocator, .{ .action_kind = action_kind, .outcome = outcome, .count = count });
    }

    return list.toOwnedSlice(allocator);
}

/// Sync-event outcome aggregates from `sync_events`.
/// Privacy: selects only outcome and count. No entity text.
fn querySyncOutcomes(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    window_days: i64,
) ![]SyncOutcome {
    var buf: [512]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select coalesce(outcome,'unknown'), count(*)" ++
            " from sync_events" ++
            " where at >= datetime('now', '-{d} days')" ++
            " group by outcome" ++
            " order by count(*) desc",
        .{window_days},
    ) catch return error.OutOfMemory;
    const sql = try allocator.dupeZ(u8, sql_str);
    defer allocator.free(sql);

    var stmt = d.prepare(sql) catch return &.{};
    defer stmt.finalize();

    var list: std.ArrayList(SyncOutcome) = .empty;
    errdefer {
        for (list.items) |*s| allocator.free(s.outcome);
        list.deinit(allocator);
    }

    while (true) {
        const row = stmt.step() catch break;
        if (row == .done) break;
        const outcome = try stmt.columnTextAlloc(0, allocator);
        errdefer allocator.free(outcome);
        const count = stmt.columnInt(1);
        try list.append(allocator, .{ .outcome = outcome, .count = count });
    }

    return list.toOwnedSlice(allocator);
}

/// Stale-claim and never-consumed counts from `agent_work_claims`.
/// Privacy: selects only counts and timestamps. No entity text, no slugs.
fn queryClaimCounts(d: *db.sqlite.Db, window_days: i64) ClaimCounts {
    // Build SQL on the stack, then use a sentinel-terminated copy for intQuery.
    var buf: [512]u8 = undefined;
    var buf2: [512]u8 = undefined;

    const stale_str = std.fmt.bufPrint(
        &buf,
        "select count(*) from agent_work_claims" ++
            " where status = 'active'" ++
            "   and (julianday('now') - julianday(claimed_at)) * 24 > 24" ++
            "   and claimed_at >= datetime('now', '-{d} days')",
        .{window_days},
    ) catch return .{ .stale_claims = 0, .never_consumed = 0 };

    // Null-terminate by writing into a slightly-larger buffer.
    var stale_z: [512 + 1]u8 = undefined;
    if (stale_str.len >= stale_z.len) return .{ .stale_claims = 0, .never_consumed = 0 };
    @memcpy(stale_z[0..stale_str.len], stale_str);
    stale_z[stale_str.len] = 0;
    const stale_sql: [:0]const u8 = stale_z[0..stale_str.len :0];

    const stale = d.intQuery(stale_sql) catch 0;

    const nc_str = std.fmt.bufPrint(
        &buf2,
        "select count(*) from agent_work_claims" ++
            " where status in ('expired','released')" ++
            "   and claimed_at >= datetime('now', '-{d} days')",
        .{window_days},
    ) catch return .{ .stale_claims = stale, .never_consumed = 0 };

    var nc_z: [512 + 1]u8 = undefined;
    if (nc_str.len >= nc_z.len) return .{ .stale_claims = stale, .never_consumed = 0 };
    @memcpy(nc_z[0..nc_str.len], nc_str);
    nc_z[nc_str.len] = 0;
    const nc_sql: [:0]const u8 = nc_z[0..nc_str.len :0];

    const never_consumed = d.intQuery(nc_sql) catch 0;

    return .{ .stale_claims = stale, .never_consumed = never_consumed };
}

/// Stale-handoff and never-consumed counts from `handoffs`.
/// Privacy: selects only counts and timestamps. No entity text.
fn queryHandoffCounts(d: *db.sqlite.Db, window_days: i64) HandoffCounts {
    var buf: [512]u8 = undefined;
    const stale_str = std.fmt.bufPrint(
        &buf,
        "select count(*) from handoffs" ++
            " where status in ('pending','validated')" ++
            "   and (julianday('now') - julianday(created_at)) * 24 > 24" ++
            "   and created_at >= datetime('now', '-{d} days')",
        .{window_days},
    ) catch return .{ .stale_handoffs = 0, .never_consumed = 0 };

    var stale_z: [512 + 1]u8 = undefined;
    if (stale_str.len >= stale_z.len) return .{ .stale_handoffs = 0, .never_consumed = 0 };
    @memcpy(stale_z[0..stale_str.len], stale_str);
    stale_z[stale_str.len] = 0;
    const stale_sql: [:0]const u8 = stale_z[0..stale_str.len :0];

    const stale = d.intQuery(stale_sql) catch 0;

    // Never consumed: handoffs that were created but never consumed,
    // regardless of staleness (distinct from stale_handoffs which requires
    // age > 24 h). "Never consumed" = status is not 'consumed' — includes
    // pending, validated, and abandoned handoffs in the window.
    var nc_buf: [512]u8 = undefined;
    const nc_str = std.fmt.bufPrint(
        &nc_buf,
        "select count(*) from handoffs" ++
            " where status != 'consumed'" ++
            "   and created_at >= datetime('now', '-{d} days')",
        .{window_days},
    ) catch return .{ .stale_handoffs = stale, .never_consumed = 0 };

    var nc_z: [512 + 1]u8 = undefined;
    if (nc_str.len >= nc_z.len) return .{ .stale_handoffs = stale, .never_consumed = 0 };
    @memcpy(nc_z[0..nc_str.len], nc_str);
    nc_z[nc_str.len] = 0;
    const nc_sql: [:0]const u8 = nc_z[0..nc_str.len :0];

    const never_consumed = d.intQuery(nc_sql) catch 0;

    return .{ .stale_handoffs = stale, .never_consumed = never_consumed };
}

/// Count task reopen events within the window from `task_reopens`.
/// Privacy: selects only a count. No entity text.
fn queryReopenCount(d: *db.sqlite.Db, window_days: i64) i64 {
    var buf: [256]u8 = undefined;
    const sql_str = std.fmt.bufPrint(
        &buf,
        "select count(*) from task_reopens" ++
            " where created_at >= datetime('now', '-{d} days')",
        .{window_days},
    ) catch return 0;

    var z: [256 + 1]u8 = undefined;
    if (sql_str.len >= z.len) return 0;
    @memcpy(z[0..sql_str.len], sql_str);
    z[sql_str.len] = 0;
    const sql: [:0]const u8 = z[0..sql_str.len :0];

    return d.intQuery(sql) catch 0;
}

// =========================================================================
// Text rendering
// =========================================================================

/// Render the diagnostic bundle as human-readable text.
/// Sections whose data is absent due to disabled logging are marked
/// "logging disabled" — never rendered as zero.
pub fn renderText(bundle: Bundle, writer: *std.Io.Writer) !void {
    try writer.print("=== planar diagnostic report ===\n", .{});
    try writer.print("version:        {s}\n", .{bundle.version});
    try writer.print("schema_version: {d}\n", .{bundle.schema_version});
    try writer.print("health:         {s}\n", .{bundle.health});
    try writer.print("window:         {d} days\n\n", .{bundle.window_days});

    // Invocations section.
    if (!bundle.logging_enabled) {
        try writer.print("[invocations]   logging disabled\n", .{});
    } else if (bundle.invocations.len == 0) {
        try writer.print("[invocations]   none in window\n", .{});
    } else {
        try writer.print("[invocations]\n", .{});
        for (bundle.invocations) |v| {
            try writer.print("  {s}: total={d} ok={d} fail={d}\n", .{
                v.verb_path, v.count, v.success_count, v.failure_count,
            });
        }
    }
    try writer.print("\n", .{});

    // Failures section.
    if (!bundle.logging_enabled) {
        try writer.print("[failures]      logging disabled\n", .{});
    } else if (bundle.failures.len == 0) {
        try writer.print("[failures]      none in window\n", .{});
    } else {
        try writer.print("[failures]\n", .{});
        for (bundle.failures) |f| {
            try writer.print("  {s}: {d}\n", .{ f.category, f.count });
        }
    }
    try writer.print("\n", .{});

    // Actions section (always-on).
    if (bundle.actions.len == 0) {
        try writer.print("[actions]       none in window\n", .{});
    } else {
        try writer.print("[actions]\n", .{});
        for (bundle.actions) |a| {
            try writer.print("  {s}/{s}: {d}\n", .{ a.action_kind, a.outcome, a.count });
        }
    }
    try writer.print("\n", .{});

    // Sync section (always-on).
    if (bundle.sync.len == 0) {
        try writer.print("[sync]          none in window\n", .{});
    } else {
        try writer.print("[sync]\n", .{});
        for (bundle.sync) |s| {
            try writer.print("  {s}: {d}\n", .{ s.outcome, s.count });
        }
    }
    try writer.print("\n", .{});

    // Claims section (always-on).
    try writer.print("[claims]        stale={d} never_consumed={d}\n", .{
        bundle.claims.stale_claims,
        bundle.claims.never_consumed,
    });

    // Handoffs section (always-on).
    try writer.print("[handoffs]      stale={d} never_consumed={d}\n", .{
        bundle.handoffs.stale_handoffs,
        bundle.handoffs.never_consumed,
    });

    // Reopens section (always-on).
    try writer.print("[reopens]       {d}\n\n", .{bundle.reopens});

    // Failure tail.
    if (!bundle.logging_enabled) {
        try writer.print("[failure tail]  logging disabled\n", .{});
    } else if (bundle.failure_tail.len == 0) {
        try writer.print("[failure tail]  empty\n", .{});
    } else {
        try writer.print("[failure tail]\n", .{});
        for (bundle.failure_tail) |r| {
            try writer.print("  {s}  cat={s}  exit={d}  at={s}\n", .{
                r.verb_path, r.error_category, r.exit_code, r.recorded_at,
            });
        }
    }
}

/// Emit the bundle as the stable `--json` wire format.
/// Field names ARE the contract consumed by M3/M4 agents and skills.
/// Empty windows emit empty arrays, never nulls or missing fields.
pub fn renderJson(bundle: Bundle, writer: *std.Io.Writer) !void {
    try writer.print("{{", .{});
    try writer.print("\"version\":", .{});
    try std.json.Stringify.encodeJsonString(bundle.version, .{}, writer);
    try writer.print(",\"schema_version\":{d}", .{bundle.schema_version});
    try writer.print(",\"health\":", .{});
    try std.json.Stringify.encodeJsonString(bundle.health, .{}, writer);
    try writer.print(",\"window\":{d}", .{bundle.window_days});

    // invocations array
    try writer.print(",\"invocations\":[", .{});
    if (bundle.logging_enabled) {
        for (bundle.invocations, 0..) |v, i| {
            if (i > 0) try writer.print(",", .{});
            try writer.print("{{\"verb_path\":", .{});
            try std.json.Stringify.encodeJsonString(v.verb_path, .{}, writer);
            try writer.print(",\"count\":{d},\"success_count\":{d},\"failure_count\":{d}}}", .{
                v.count, v.success_count, v.failure_count,
            });
        }
    }
    try writer.print("]", .{});

    // failures array
    try writer.print(",\"failures\":[", .{});
    if (bundle.logging_enabled) {
        for (bundle.failures, 0..) |f, i| {
            if (i > 0) try writer.print(",", .{});
            try writer.print("{{\"category\":", .{});
            try std.json.Stringify.encodeJsonString(f.category, .{}, writer);
            try writer.print(",\"count\":{d}}}", .{f.count});
        }
    }
    try writer.print("]", .{});

    // actions array (always-on)
    try writer.print(",\"actions\":[", .{});
    for (bundle.actions, 0..) |a, i| {
        if (i > 0) try writer.print(",", .{});
        try writer.print("{{\"action_kind\":", .{});
        try std.json.Stringify.encodeJsonString(a.action_kind, .{}, writer);
        try writer.print(",\"outcome\":", .{});
        try std.json.Stringify.encodeJsonString(a.outcome, .{}, writer);
        try writer.print(",\"count\":{d}}}", .{a.count});
    }
    try writer.print("]", .{});

    // sync array (always-on)
    try writer.print(",\"sync\":[", .{});
    for (bundle.sync, 0..) |s, i| {
        if (i > 0) try writer.print(",", .{});
        try writer.print("{{\"outcome\":", .{});
        try std.json.Stringify.encodeJsonString(s.outcome, .{}, writer);
        try writer.print(",\"count\":{d}}}", .{s.count});
    }
    try writer.print("]", .{});

    // claims object (always-on)
    try writer.print(",\"claims\":{{\"stale_claims\":{d},\"never_consumed\":{d}}}", .{
        bundle.claims.stale_claims,
        bundle.claims.never_consumed,
    });

    // handoffs object (always-on)
    try writer.print(",\"handoffs\":{{\"stale_handoffs\":{d},\"never_consumed\":{d}}}", .{
        bundle.handoffs.stale_handoffs,
        bundle.handoffs.never_consumed,
    });

    // reopens count (always-on)
    try writer.print(",\"reopens\":{d}", .{bundle.reopens});

    try writer.print("}}\n", .{});
}

// =========================================================================
// Unit tests
// =========================================================================

test "build: empty database — all aggregates are zero / empty" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 0), bundle.invocations.len);
    try std.testing.expectEqual(@as(usize, 0), bundle.failures.len);
    try std.testing.expectEqual(@as(usize, 0), bundle.actions.len);
    try std.testing.expectEqual(@as(usize, 0), bundle.sync.len);
    try std.testing.expectEqual(@as(i64, 0), bundle.claims.stale_claims);
    try std.testing.expectEqual(@as(i64, 0), bundle.handoffs.stale_handoffs);
    try std.testing.expectEqual(@as(usize, 0), bundle.failure_tail.len);
    try std.testing.expectEqual(@as(i64, 30), bundle.window_days);
    try std.testing.expectEqualStrings("ok", bundle.health);
}

test "build: logging disabled — invocations/failures are empty" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Seed an invocation row directly.
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('health', '', 0, datetime('now'))",
        &.{},
    );

    var bundle = try build(&d, std.testing.allocator, 30, 20, false, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    // logging_enabled = false → invocations/failures/failure_tail are empty.
    try std.testing.expect(!bundle.logging_enabled);
    try std.testing.expectEqual(@as(usize, 0), bundle.invocations.len);
    try std.testing.expectEqual(@as(usize, 0), bundle.failures.len);
    try std.testing.expectEqual(@as(usize, 0), bundle.failure_tail.len);
}

test "build: aggregates reflect seeded cli_invocations" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Seed 2 successful 'health' and 1 failed 'task add'.
    // Each INSERT is separate to avoid multi-row value issues with params.
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('health', '', 0, datetime('now'))",
        &.{},
    );
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('health', '', 0, datetime('now'))",
        &.{},
    );
    // Failed row: exit_code != 0 requires error_category (CHECK constraint).
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)" ++
            " values ('task add', '<pos:1>', 2, 'usage', datetime('now'))",
        &.{},
    );

    var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 2), bundle.invocations.len);
    // health has 2 total (most frequent first).
    try std.testing.expectEqualStrings("health", bundle.invocations[0].verb_path);
    try std.testing.expectEqual(@as(i64, 2), bundle.invocations[0].count);
    try std.testing.expectEqual(@as(i64, 2), bundle.invocations[0].success_count);
    try std.testing.expectEqual(@as(i64, 0), bundle.invocations[0].failure_count);

    try std.testing.expectEqual(@as(usize, 1), bundle.failures.len);
    try std.testing.expectEqualStrings("usage", bundle.failures[0].category);
    try std.testing.expectEqual(@as(i64, 1), bundle.failures[0].count);

    // Failure tail: 1 row (the task add).
    try std.testing.expectEqual(@as(usize, 1), bundle.failure_tail.len);
    try std.testing.expectEqualStrings("task add", bundle.failure_tail[0].verb_path);
    try std.testing.expectEqual(@as(i64, 2), bundle.failure_tail[0].exit_code);
}

test "build: window filter excludes old rows" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Insert one row 5 days old (inside 7-day window) and one 31 days old.
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('recent', '', 0, datetime('now', '-5 days'))," ++
            "        ('old', '', 0, datetime('now', '-31 days'))",
        &.{},
    );

    // 7-day window — only 'recent' should appear.
    var bundle = try build(&d, std.testing.allocator, 7, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 1), bundle.invocations.len);
    try std.testing.expectEqualStrings("recent", bundle.invocations[0].verb_path);
}

test "build: failure tail cap — only tail_n rows returned" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Seed 5 failed invocations.
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)" ++
            " values ('v1', '', 2, 'usage', datetime('now', '-4 minutes'))," ++
            "        ('v2', '', 2, 'usage', datetime('now', '-3 minutes'))," ++
            "        ('v3', '', 2, 'usage', datetime('now', '-2 minutes'))," ++
            "        ('v4', '', 2, 'usage', datetime('now', '-1 minutes'))," ++
            "        ('v5', '', 2, 'usage', datetime('now'))",
        &.{},
    );

    // tail_n = 2 — only 2 rows returned, newest first.
    var bundle = try build(&d, std.testing.allocator, 30, 2, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    try std.testing.expectEqual(@as(usize, 2), bundle.failure_tail.len);
    // Newest first: v5 then v4.
    try std.testing.expectEqualStrings("v5", bundle.failure_tail[0].verb_path);
    try std.testing.expectEqualStrings("v4", bundle.failure_tail[1].verb_path);
}

test "build: redaction — seeded sentinel titles never appear in text or JSON output" {
    const sentinel = "SENTINEL_MUST_NOT_APPEAR_IN_REPORT";
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Seed a task with the sentinel in its title (entity text, must not leak).
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', ?, 'todo', 100)",
        &.{.{ .text = sentinel }},
    );
    // Seed an invocation whose verb_path does NOT contain the sentinel,
    // but whose args_shape column would carry a value if the code leaked it.
    _ = try d.execParams(
        "insert into cli_invocations (verb_path, args_shape, exit_code, recorded_at)" ++
            " values ('health', '', 0, datetime('now'))",
        &.{},
    );

    var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    // Render both text and JSON and check no sentinel appears.
    // The bundle is small (one seeded invocation row) so 16 KB is ample.
    var text_buf: [16384]u8 = undefined;
    var text_w: std.Io.Writer = .fixed(&text_buf);
    try renderText(bundle, &text_w);
    try std.testing.expect(std.mem.indexOf(u8, text_w.buffered(), sentinel) == null);

    var json_buf: [16384]u8 = undefined;
    var json_w: std.Io.Writer = .fixed(&json_buf);
    try renderJson(bundle, &json_w);
    try std.testing.expect(std.mem.indexOf(u8, json_w.buffered(), sentinel) == null);
}

test "build: handoffs never_consumed is distinct from stale_handoffs" {
    // A consumed handoff created > 24 h ago: stale_handoffs = 0 (already consumed),
    // never_consumed = 0 (was consumed).
    // An unconsumed (pending) handoff created recently: stale_handoffs = 0 (not old),
    // never_consumed = 1.
    // This proves the two fields can differ when data is mixed.
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Need a session + context_snapshot to satisfy the FKs on handoffs.
    _ = try d.execParams(
        "insert into sessions (vendor, started_at) values ('test', datetime('now'))",
        &.{},
    );
    const session_id = d.intQuery("select last_insert_rowid()") catch 0;

    var id_buf: [256]u8 = undefined;
    _ = try d.execParams(
        try std.fmt.bufPrintZ(&id_buf, "insert into context_snapshots (session_id, vendor, created_at)" ++
            " values ({d}, 'test', datetime('now'))", .{session_id}),
        &.{},
    );
    const snap_id = d.intQuery("select last_insert_rowid()") catch 0;

    // Handoff 1: consumed, created 2 days ago (inside 30-day window, old enough to
    // be stale if not consumed — but it IS consumed, so stale_handoffs won't count it).
    _ = try d.execParams(
        try std.fmt.bufPrintZ(&id_buf, "insert into handoffs (from_snapshot_id, from_vendor, status, created_at, consumed_at)" ++
            " values ({d}, 'v1', 'consumed', datetime('now', '-2 days'), datetime('now', '-1 days'))", .{snap_id}),
        &.{},
    );

    // Handoff 2: pending, created just now (not stale, but never consumed).
    _ = try d.execParams(
        try std.fmt.bufPrintZ(&id_buf, "insert into handoffs (from_snapshot_id, from_vendor, status, created_at)" ++
            " values ({d}, 'v1', 'pending', datetime('now'))", .{snap_id}),
        &.{},
    );

    var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    // stale_handoffs: only pending/validated handoffs older than 24h — neither qualifies.
    try std.testing.expectEqual(@as(i64, 0), bundle.handoffs.stale_handoffs);
    // never_consumed: the pending handoff (handoff 2) is not consumed.
    try std.testing.expectEqual(@as(i64, 1), bundle.handoffs.never_consumed);
    // The two values differ — proving distinctness.
    try std.testing.expect(bundle.handoffs.stale_handoffs != bundle.handoffs.never_consumed);
}

test "build: reopens count reflects seeded task_reopens rows" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    // Zero reopens baseline.
    {
        var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
        defer bundle.deinit(std.testing.allocator);
        try std.testing.expectEqual(@as(i64, 0), bundle.reopens);
    }

    // Seed a task and two reopen rows directly (the unit-test layer
    // may use direct SQL; integration tests go through the CLI).
    _ = try d.execParams(
        "insert into tasks (scope_kind, title, status, priority) values ('global', 'T', 'done', 100)",
        &.{},
    );
    const task_id = d.intQuery("select last_insert_rowid()") catch 0;

    {
        const sql1 = try std.fmt.allocPrint(
            std.testing.allocator,
            "insert into task_reopens (task_id, from_status, to_status, source)" ++
                " values ({d}, 'done', 'todo', 'task-reopen')",
            .{task_id},
        );
        defer std.testing.allocator.free(sql1);
        try d.execSlice(std.testing.allocator, sql1);
    }
    {
        const sql2 = try std.fmt.allocPrint(
            std.testing.allocator,
            "insert into task_reopens (task_id, from_status, to_status, source)" ++
                " values ({d}, 'done', 'doing', 'task-update-force')",
            .{task_id},
        );
        defer std.testing.allocator.free(sql2);
        try d.execSlice(std.testing.allocator, sql2);
    }

    var bundle2 = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle2.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(i64, 2), bundle2.reopens);

    // A 1-day window should exclude rows older than 1 day; all rows are
    // datetime('now') so they should still be counted (within 1 day).
    var bundle3 = try build(&d, std.testing.allocator, 1, 20, true, "/tmp/test.db");
    defer bundle3.deinit(std.testing.allocator);
    try std.testing.expectEqual(@as(i64, 2), bundle3.reopens);
}

test "build: schema_version matches embedded_max after applyAll" {
    var d = try db.sqlite.Db.openMemory();
    defer d.close();
    try db.migrate.applyAll(&d, std.testing.allocator);

    var bundle = try build(&d, std.testing.allocator, 30, 20, true, "/tmp/test.db");
    defer bundle.deinit(std.testing.allocator);

    const expected: i64 = @intCast(db.migrate.embedded_max);
    try std.testing.expectEqual(expected, bundle.schema_version);
}
