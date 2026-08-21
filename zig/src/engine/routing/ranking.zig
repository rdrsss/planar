//! engine/routing/ranking.zig — confidence-gated candidate ranking over
//! declared-experiment evidence (plan 950 task 5530).
//!
//! Replaces raw approval-rate suggestions. A raw rate is indistinguishable
//! between 1/1 and 40/40, so ranking on it promotes whichever candidate
//! happened to go first — the classic small-sample trap, and the reason a
//! single lucky dispatch could previously become a routing default.
//!
//! Ranking therefore uses the **Wilson score lower bound**: the lower end of a
//! 95% confidence interval on the success rate. It is deliberately pessimistic
//! about thin evidence — 1/1 scores well below 40/40 — so a candidate earns a
//! recommendation by being repeatedly good, not by being new.
//!
//! Two gates sit in front of ranking, and both refuse rather than guess:
//!
//!   * Under `minimum_samples`, a candidate is `insufficient_data`. It is
//!     shown (an operator still wants to see it exists) but never ranked and
//!     never recommended.
//!   * Below the quality floor, a candidate is excluded BEFORE any rework,
//!     gate-failure, latency, or cost comparison. Ordering a candidate that
//!     fails the floor by its latency would let "fast and wrong" outrank
//!     "slower and correct".
//!
//! Only cohort-eligible declared-experiment samples are counted; the evidence
//! boundary is enforced in `evidence.zig` and recorded on the row.

const std = @import("std");
const db = @import("db");
const store = @import("store.zig");

/// Bumped when aggregation or ranking semantics change, so results computed
/// under different rules are never presented as comparable.
pub const ranking_version = "routing-ranking-v1";

pub const Error = error{QueryFailed} || std.mem.Allocator.Error;

/// z for a two-sided 95% interval. Fixed rather than configurable: the
/// reported number is labelled "95% Wilson lower bound", so the constant and
/// the label have to move together.
pub const z_95: f64 = 1.959963984540054;

/// Lower bound of the Wilson score interval for `successes` of `n`.
///
/// Returns 0 for n = 0 — no evidence is not weak evidence of success, and a
/// zero keeps an unsampled candidate below every sampled one without needing
/// a special case at the ranking site.
pub fn wilsonLowerBound(successes: u64, n: u64, z: f64) f64 {
    if (n == 0) return 0;
    const nf: f64 = @floatFromInt(n);
    const p: f64 = @as(f64, @floatFromInt(successes)) / nf;
    const z2 = z * z;
    const denom = 1.0 + z2 / nf;
    const center = p + z2 / (2.0 * nf);
    const margin = z * @sqrt((p * (1.0 - p) + z2 / (4.0 * nf)) / nf);
    const lower = (center - margin) / denom;
    return if (lower < 0) 0 else lower;
}

/// The exact cohort a candidate is ranked within. Evidence is never pooled
/// across cohorts: a candidate good at `mechanical/bounded` work has told you
/// nothing about `architectural/high-risk` work.
pub const Cohort = struct {
    project_id: i64,
    validation_policy_version: []const u8,
    routing_policy_version: []const u8,
    vendor: []const u8,
    role: []const u8,
    tier: store.Tier,
    work_type: store.WorkType,
    complexity: store.Complexity,
};

/// Gate configuration. Both gates are refusals, not adjustments.
pub const Gates = struct {
    /// Below this sample count a candidate is `insufficient_data`.
    minimum_samples: u64 = 5,
    /// A candidate whose Wilson lower bound falls below this is excluded
    /// before any secondary ordering key is consulted.
    quality_floor: f64 = 0.5,
};

pub const Row = struct {
    candidate_id: i64,
    candidate: []const u8,
    vendor: []const u8,
    fallback_order: i64,

    samples: u64 = 0,
    successes: u64 = 0,
    /// Terminal state `quality_failed` — a run that finished but did not meet
    /// the validation policy. Tracked apart from the success rate because
    /// "failed review" and "crashed on spawn" are different problems.
    gate_failures: u64 = 0,
    /// Attempts beyond the first, summed across samples. Retries are the cost
    /// a candidate imposes even when it eventually succeeds.
    excess_attempts: u64 = 0,

    /// Optional comparable metrics. Null means NOT MEASURED, never zero: a
    /// zero would rank an unmeasured candidate as instant and free.
    mean_latency_ms: ?f64 = null,
    mean_cost_micros: ?f64 = null,
    measured_samples: u64 = 0,

    raw_rate: f64 = 0,
    wilson_lower: f64 = 0,
    gate_failure_rate: f64 = 0,
    expected_excess_iterations: f64 = 0,

    /// Under `minimum_samples`: shown, never ranked, never recommended.
    insufficient_data: bool = false,
    /// At or above minimum samples but below the quality floor.
    below_quality_floor: bool = false,
    /// 1-based rank among eligible rows; null for gated-out rows.
    rank: ?usize = null,

    pub fn deinit(self: Row, allocator: std.mem.Allocator) void {
        allocator.free(self.candidate);
        allocator.free(self.vendor);
    }
};

pub const Result = struct {
    rows: []Row = &.{},
    /// The recommendation, or null when no candidate cleared both gates.
    /// Null is a real answer: "keep the configured default" beats promoting
    /// the least-bad of several unproven candidates.
    recommended: ?usize = null,
    /// Why there is no recommendation, when there isn't one.
    no_recommendation_reason: ?[]const u8 = null,
    version: []const u8 = ranking_version,

    pub fn deinit(self: *Result, allocator: std.mem.Allocator) void {
        for (self.rows) |row| row.deinit(allocator);
        allocator.free(self.rows);
        self.* = .{};
    }
};

/// Order two ranked rows. Returns true when `a` should sort before `b`.
///
/// Lexicographic, in the order the acceptance criteria fix:
///   1. Wilson lower bound, descending — confidence-adjusted quality first.
///   2. Expected excess iterations, ascending — fewer retries.
///   3. Gate-failure rate, ascending.
///   4. Configured fallback order, ascending — a deterministic operator-chosen
///      tiebreak, so equal evidence never yields an arbitrary (or unstable)
///      ordering between runs.
///
/// Latency and cost are criteria-optional and only comparable when both rows
/// carry them; they are not yet recorded on a terminal sample, so they are
/// deliberately absent rather than defaulted to zero — a zero would silently
/// rank an unmeasured candidate as instant and free.
/// Compare an optional metric, lower-is-better, only when BOTH sides have one.
///
/// Returns null when either is unmeasured, so the caller falls through to the
/// next key rather than inventing an ordering. Treating a missing value as 0
/// would promote the candidate we know least about; treating it as infinity
/// would bury a candidate for not being instrumented. Neither is a judgement
/// the evidence supports, so the metric simply does not participate.
fn compareOptional(a: ?f64, b: ?f64) ?bool {
    const x = a orelse return null;
    const y = b orelse return null;
    if (x == y) return null;
    return x < y;
}

pub fn lessThan(_: void, a: Row, b: Row) bool {
    if (a.wilson_lower != b.wilson_lower) return a.wilson_lower > b.wilson_lower;
    if (a.expected_excess_iterations != b.expected_excess_iterations) {
        return a.expected_excess_iterations < b.expected_excess_iterations;
    }
    if (a.gate_failure_rate != b.gate_failure_rate) {
        return a.gate_failure_rate < b.gate_failure_rate;
    }
    if (compareOptional(a.mean_latency_ms, b.mean_latency_ms)) |lt| return lt;
    if (compareOptional(a.mean_cost_micros, b.mean_cost_micros)) |lt| return lt;
    return a.fallback_order < b.fallback_order;
}

/// Aggregate declared-experiment evidence for one exact cohort and rank it.
///
/// The query counts one row per terminal sample. Sample identity is enforced
/// by the unique constraint on `routing_terminal_samples`, so a retry that
/// produced a second terminal event cannot double-count here — the fold in
/// `evidence.zig` already reduced it to one sample.
pub fn rank(
    d: *db.sqlite.Db,
    allocator: std.mem.Allocator,
    cohort: Cohort,
    gates: Gates,
) Error!Result {
    const sql: [:0]const u8 =
        \\select
        \\  c.id,
        \\  c.candidate_id,
        \\  c.vendor,
        \\  c.fallback_order,
        \\  count(*) as samples,
        \\  sum(s.quality_success) as successes,
        \\  sum(case when s.terminal_state = 'quality_failed' then 1 else 0 end) as gate_failures,
        \\  coalesce(sum(
        \\    (select max(e.attempt_number) - 1
        \\     from routing_dispatch_events as e
        \\     where e.event_id = s.terminal_event_id
        \\        or e.dispatch_id = (
        \\          select d2.dispatch_id from routing_dispatch_events as d2
        \\          where d2.event_id = s.terminal_event_id
        \\        ))
        \\  ), 0) as excess_attempts,
        \\  avg(s.latency_ms) as mean_latency_ms,
        \\  avg(s.cost_micros) as mean_cost_micros,
        \\  sum(case when s.latency_ms is not null or s.cost_micros is not null
        \\      then 1 else 0 end) as measured_samples
        \\from routing_terminal_samples as s
        \\join routing_candidates as c on c.id = s.candidate_id
        \\where s.cohort_eligible = 1
        \\  and s.project_id = ?
        \\  and s.validation_policy_version = ?
        \\  and s.routing_policy_version = ?
        \\  and s.vendor = ?
        \\  and s.role = ?
        \\  and s.tier = ?
        \\  and s.work_type = ?
        \\  and s.complexity = ?
        \\group by c.id, c.candidate_id, c.vendor, c.fallback_order
    ;
    var stmt = d.prepare(sql) catch return Error.QueryFailed;
    defer stmt.finalize();
    stmt.bind(&.{
        .{ .int = cohort.project_id },
        .{ .text = cohort.validation_policy_version },
        .{ .text = cohort.routing_policy_version },
        .{ .text = cohort.vendor },
        .{ .text = cohort.role },
        .{ .text = @tagName(cohort.tier) },
        .{ .text = @tagName(cohort.work_type) },
        .{ .text = complexityText(cohort.complexity) },
    }) catch return Error.QueryFailed;

    var rows: std.ArrayList(Row) = .empty;
    errdefer {
        for (rows.items) |r| r.deinit(allocator);
        rows.deinit(allocator);
    }

    while ((stmt.step() catch return Error.QueryFailed) == .row) {
        const candidate = stmt.columnTextAlloc(1, allocator) catch return Error.QueryFailed;
        errdefer allocator.free(candidate);
        const vendor = stmt.columnTextAlloc(2, allocator) catch return Error.QueryFailed;
        errdefer allocator.free(vendor);

        const samples: u64 = @intCast(@max(0, stmt.columnInt(4)));
        const successes: u64 = @intCast(@max(0, stmt.columnInt(5)));
        const gate_failures: u64 = @intCast(@max(0, stmt.columnInt(6)));
        const excess: u64 = @intCast(@max(0, stmt.columnInt(7)));

        var agg: Row = .{
            .mean_latency_ms = if (stmt.columnIsNull(8)) null else stmt.columnDouble(8),
            .mean_cost_micros = if (stmt.columnIsNull(9)) null else stmt.columnDouble(9),
            .measured_samples = @intCast(@max(0, stmt.columnInt(10))),
            .candidate_id = stmt.columnInt(0),
            .candidate = candidate,
            .vendor = vendor,
            .fallback_order = stmt.columnInt(3),
            .samples = samples,
            .successes = successes,
            .gate_failures = gate_failures,
            .excess_attempts = excess,
        };
        finalize(&agg, gates);
        try rows.append(allocator, agg);
    }

    var result: Result = .{ .rows = try rows.toOwnedSlice(allocator) };

    // Rank only rows that cleared BOTH gates. Sorting the gated-out rows too
    // would put a number next to a candidate we just said we cannot judge.
    std.mem.sort(Row, result.rows, {}, sortEligibleFirst);
    var next_rank: usize = 1;
    for (result.rows) |*ranked| {
        if (ranked.insufficient_data or ranked.below_quality_floor) continue;
        ranked.rank = next_rank;
        next_rank += 1;
    }

    if (next_rank == 1) {
        result.recommended = null;
        result.no_recommendation_reason = if (result.rows.len == 0)
            "no cohort-eligible declared-experiment samples"
        else
            "no candidate cleared both the minimum-sample and quality-floor gates";
    } else {
        result.recommended = 0;
    }
    return result;
}

/// The wire spelling is hyphenated (`high-risk`); the enum is not.
fn complexityText(c: store.Complexity) []const u8 {
    return switch (c) {
        .bounded => "bounded",
        .standard => "standard",
        .high_risk => "high-risk",
    };
}

fn finalize(r: *Row, gates: Gates) void {
    const n: f64 = @floatFromInt(r.samples);
    if (r.samples > 0) {
        r.raw_rate = @as(f64, @floatFromInt(r.successes)) / n;
        r.gate_failure_rate = @as(f64, @floatFromInt(r.gate_failures)) / n;
        r.expected_excess_iterations = @as(f64, @floatFromInt(r.excess_attempts)) / n;
    }
    r.wilson_lower = wilsonLowerBound(r.successes, r.samples, z_95);

    if (r.samples < gates.minimum_samples) {
        r.insufficient_data = true;
        return;
    }
    if (r.wilson_lower < gates.quality_floor) r.below_quality_floor = true;
}

/// Eligible rows first (ranked among themselves), then gated-out rows in a
/// stable, explainable order rather than whatever the query returned.
fn sortEligibleFirst(_: void, a: Row, b: Row) bool {
    const a_gated = a.insufficient_data or a.below_quality_floor;
    const b_gated = b.insufficient_data or b.below_quality_floor;
    if (a_gated != b_gated) return !a_gated;
    if (a_gated) {
        // Among gated rows: more evidence first, then configured order.
        if (a.samples != b.samples) return a.samples > b.samples;
        return a.fallback_order < b.fallback_order;
    }
    return lessThan({}, a, b);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

const testing = std.testing;

test "wilson: no evidence scores zero, and never returns a negative bound" {
    try testing.expectEqual(@as(f64, 0), wilsonLowerBound(0, 0, z_95));
    try testing.expectEqual(@as(f64, 0), wilsonLowerBound(0, 10, z_95));
    try testing.expect(wilsonLowerBound(0, 1, z_95) >= 0);
}

test "wilson: thin evidence is penalized relative to identical rates with depth" {
    // The whole reason for using Wilson: 1/1 and 40/40 are both a 100% raw
    // rate, and ranking on the raw rate cannot tell them apart.
    const one = wilsonLowerBound(1, 1, z_95);
    const ten = wilsonLowerBound(10, 10, z_95);
    const forty = wilsonLowerBound(40, 40, z_95);
    try testing.expect(one < ten);
    try testing.expect(ten < forty);
    try testing.expect(forty < 1.0);

    // Known values, to catch an algebra slip that preserves ordering.
    try testing.expectApproxEqAbs(@as(f64, 0.2065493), one, 1e-6);
    try testing.expectApproxEqAbs(@as(f64, 0.7224672), ten, 1e-6);
}

test "wilson: a half-and-half record lands near, but below, one half" {
    const half = wilsonLowerBound(50, 100, z_95);
    try testing.expect(half < 0.5);
    try testing.expect(half > 0.39);
    try testing.expectApproxEqAbs(@as(f64, 0.4038315), half, 1e-6);
}

fn mkRow(candidate: []const u8, samples: u64, successes: u64, order: i64) Row {
    var r: Row = .{
        .candidate_id = order,
        .candidate = candidate,
        .vendor = "v",
        .fallback_order = order,
        .samples = samples,
        .successes = successes,
    };
    finalize(&r, .{});
    return r;
}

test "gates: under minimum samples is insufficient_data, not a low score" {
    // A perfect 2/2 must not be promoted; it must be declared unjudgeable.
    const thin = mkRow("a", 2, 2, 0);
    try testing.expect(thin.insufficient_data);
    try testing.expect(!thin.below_quality_floor);
    try testing.expectEqual(@as(f64, 1.0), thin.raw_rate);
}

test "gates: the quality floor is applied to the bound, not the raw rate" {
    // 5/5 is a 100% raw rate but only ~0.566 Wilson — above the 0.5 floor.
    const five = mkRow("a", 5, 5, 0);
    try testing.expect(!five.insufficient_data);
    try testing.expect(!five.below_quality_floor);

    // 6/10 is a 60% raw rate but ~0.313 Wilson — below the floor. Ranking on
    // the raw rate would have kept it.
    const six = mkRow("b", 10, 6, 0);
    try testing.expectEqual(@as(f64, 0.6), six.raw_rate);
    try testing.expect(six.below_quality_floor);
}

test "ranking: quality floor excludes before any secondary key is consulted" {
    // `fast` is below the floor but has zero retries and zero gate failures.
    // If the floor were applied after the iteration/gate keys it would win.
    var fast = mkRow("fast", 10, 6, 0);
    fast.expected_excess_iterations = 0;
    fast.gate_failure_rate = 0;
    var good = mkRow("good", 20, 19, 1);
    good.expected_excess_iterations = 2.0;
    good.gate_failure_rate = 0.1;

    var rows = [_]Row{ fast, good };
    std.mem.sort(Row, &rows, {}, sortEligibleFirst);
    try testing.expectEqualStrings("good", rows[0].candidate);
    try testing.expect(rows[1].below_quality_floor);
}

test "ranking: ties fall through to iterations, then gate failures, then order" {
    var a = mkRow("a", 20, 19, 5);
    var b = mkRow("b", 20, 19, 2);
    // Identical evidence: only the configured order separates them, and it
    // must, or the ordering would vary between runs.
    try testing.expectEqual(a.wilson_lower, b.wilson_lower);
    try testing.expect(lessThan({}, b, a));

    // Excess iterations outrank gate failures.
    a.expected_excess_iterations = 0.1;
    b.expected_excess_iterations = 0.9;
    a.gate_failure_rate = 0.9;
    b.gate_failure_rate = 0.0;
    try testing.expect(lessThan({}, a, b));

    // With iterations equal, the gate-failure rate decides.
    b.expected_excess_iterations = 0.1;
    try testing.expect(lessThan({}, b, a));
}

// --- DB-backed: the aggregation query and gates over a real evidence chain ---

/// Test-fixture shapes, shared with `views.zig` so the full evidence chain
/// (manifest -> dispatch -> event -> sample) is built one way only.
pub const Sample = struct { state: []const u8, success: bool, attempts: i64 };
pub const CandidateSpec = struct { name: []const u8, order: i64, samples: []const Sample };

/// Seed a full evidence chain: experiment manifest -> per-sample dispatch ->
/// outcome event -> terminal sample. The schema enforces every link (sample
/// identity, declared-experiment class, frozen-manifest membership), so a
/// shortcut fixture cannot be inserted at all — which is why this builds the
/// whole chain rather than writing samples directly.
fn seedCohort(
    conn: *db.sqlite.Db,
    a: std.mem.Allocator,
    specs: []const CandidateSpec,
) !void {
    return seedCohortWith(conn, a, specs, &.{});
}

/// `extra_population` names work items admitted by the frozen manifest but not
/// dispatched by `specs`. The manifest is immutable once written, so anything
/// a later insert needs must be declared here up front — which is the freeze
/// working as intended.
pub fn seedCohortWith(
    conn: *db.sqlite.Db,
    a: std.mem.Allocator,
    specs: []const CandidateSpec,
    extra_population: []const []const u8,
) !void {
    _ = try conn.execParams("insert into projects (slug, name, root_path) values ('p','p','/p')", &.{});

    var population: std.ArrayList(u8) = .empty;
    defer population.deinit(a);
    var candidate_set: std.ArrayList(u8) = .empty;
    defer candidate_set.deinit(a);
    try population.append(a, '[');
    try candidate_set.append(a, '[');

    var ids: std.ArrayList(i64) = .empty;
    defer ids.deinit(a);
    for (specs, 0..) |spec, si| {
        const id = try store.createCandidate(conn, .{
            .vendor = "vendor-x",
            .candidate_id = spec.name,
            .fallback_order = spec.order,
        });
        try ids.append(a, id);
        if (si != 0) try candidate_set.append(a, ',');
        {
            var buf: [24]u8 = undefined;
            try candidate_set.appendSlice(a, try std.fmt.bufPrint(&buf, "{d}", .{id}));
        }
        for (spec.samples, 0..) |_, i| {
            if (population.items.len > 1) try population.append(a, ',');
            {
                const item = try std.fmt.allocPrint(a, "\"lwi-{s}-{d}\"", .{ spec.name, i });
                defer a.free(item);
                try population.appendSlice(a, item);
            }
        }
    }
    for (extra_population) |extra| {
        if (population.items.len > 1) try population.append(a, ',');
        const item = try std.fmt.allocPrint(a, "\"{s}\"", .{extra});
        defer a.free(item);
        try population.appendSlice(a, item);
    }
    try population.append(a, ']');
    try candidate_set.append(a, ']');

    const exp = try conn.execParams(
        \\insert into routing_experiments (
        \\  experiment_key, project_id, validation_policy_version, vendor, role, tier,
        \\  work_type, complexity, routing_policy_version, eligible_population_json,
        \\  candidate_set_json, allocation_method, stopping_rule_json,
        \\  analysis_policy_json, manifest_digest, operator_approved_at
        \\) values (
        \\  'exp',1,'val-v1','vendor-x','coder','medium','feature','standard','route-v1',
        \\  ?,?,'balanced','{}','{}','d','2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .text = population.items }, .{ .text = candidate_set.items } });

    var seq: i64 = 0;
    for (specs, 0..) |spec, si| {
        for (spec.samples, 0..) |s, i| {
            const lwi = try std.fmt.allocPrint(a, "lwi-{s}-{d}", .{ spec.name, i });
            defer a.free(lwi);
            const key = try std.fmt.allocPrint(a, "dk-{s}-{d}", .{ spec.name, i });
            defer a.free(key);
            const ev = try std.fmt.allocPrint(a, "ev-{s}-{d}", .{ spec.name, i });
            defer a.free(ev);

            const dispatch = try conn.execParams(
                \\insert into routing_dispatch_snapshots (
                \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
                \\  vendor, role, tier, work_type, complexity, routing_policy_version,
                \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
                \\  requested_candidate_id, assignment_class, experiment_id, operator_decision,
                \\  reviewer_disposition, terminal_state, confirmed_at
                \\) values (
                \\  ?,?,1,'val-v1','vendor-x','coder','medium','feature','standard','route-v1',
                \\  'pr','pk','po','ca',?,'declared_experiment',?,'confirmed','approved',?,
                \\  '2026-01-01T00:00:00Z'
                \\)
            , &.{
                .{ .text = key }, .{ .text = lwi },     .{ .int = ids.items[si] },
                .{ .int = exp },  .{ .text = s.state },
            });

            _ = try conn.execParams(
                \\insert into routing_dispatch_events (
                \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
                \\  terminal_state, payload_json, occurred_at
                \\) values (?,?,?,'outcome',?,?,'{}','2026-01-01T00:00:00Z')
            , &.{
                .{ .int = dispatch },   .{ .text = ev },      .{ .int = seq },
                .{ .int = s.attempts }, .{ .text = s.state },
            });
            seq += 1;

            _ = try conn.execParams(
                \\insert into routing_terminal_samples (
                \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
                \\  candidate_id, project_id, validation_policy_version, routing_policy_version,
                \\  vendor, tier, work_type, complexity, terminal_event_id, terminal_state,
                \\  quality_success, cohort_eligible, finalized_at
                \\) values (
                \\  ?,?,'coder','pk',?,1,'val-v1','route-v1','vendor-x','medium','feature',
                \\  'standard',?,?,?,1,'2026-01-01T00:00:00Z'
                \\)
            , &.{
                .{ .int = exp }, .{ .text = lwi },     .{ .int = ids.items[si] },
                .{ .text = ev }, .{ .text = s.state }, .{ .int = if (s.success) 1 else 0 },
            });
        }
    }
}

fn repeat(a: std.mem.Allocator, n: usize, s: Sample) ![]Sample {
    const out = try a.alloc(Sample, n);
    for (out) |*slot| slot.* = s;
    return out;
}

test "rank: confidence gating withholds a perfect small sample and a mediocre large one" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    const ok: Sample = .{ .state = "completed", .success = true, .attempts = 1 };
    const gate_fail: Sample = .{ .state = "quality_failed", .success = false, .attempts = 1 };
    const aborted: Sample = .{ .state = "aborted", .success = false, .attempts = 1 };

    var a_rows = try repeat(a, 20, ok);
    defer a.free(a_rows);
    a_rows[19] = gate_fail; // 19/20

    var b_rows = try repeat(a, 10, ok);
    defer a.free(b_rows);
    for (b_rows[6..]) |*r| r.* = aborted; // 6/10

    const c_rows = try repeat(a, 2, ok); // 2/2
    defer a.free(c_rows);

    try seedCohort(&conn, a, &.{
        .{ .name = "cand-A", .order = 0, .samples = a_rows },
        .{ .name = "cand-B", .order = 1, .samples = b_rows },
        .{ .name = "cand-C", .order = 2, .samples = c_rows },
    });

    var result = try rank(&conn, a, .{
        .project_id = 1,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
    }, .{});
    defer result.deinit(a);

    try testing.expectEqual(@as(usize, 3), result.rows.len);

    // Only cand-A is judgeable. Ranking on the raw rate would have put
    // cand-C first (a perfect 1.000 from two runs) and cand-B second.
    try testing.expectEqualStrings("cand-A", result.rows[0].candidate);
    try testing.expectEqual(@as(?usize, 1), result.rows[0].rank);
    try testing.expectEqual(@as(u64, 20), result.rows[0].samples);
    try testing.expectEqual(@as(u64, 19), result.rows[0].successes);
    try testing.expectApproxEqAbs(@as(f64, 0.05), result.rows[0].gate_failure_rate, 1e-9);

    for (result.rows[1..]) |row| {
        try testing.expect(row.rank == null);
        if (std.mem.eql(u8, row.candidate, "cand-C")) {
            try testing.expect(row.insufficient_data);
            try testing.expectEqual(@as(f64, 1.0), row.raw_rate);
        } else {
            try testing.expect(row.below_quality_floor);
            try testing.expectApproxEqAbs(@as(f64, 0.6), row.raw_rate, 1e-9);
        }
    }

    try testing.expectEqual(@as(?usize, 0), result.recommended);
}

test "rank: a cohort with no evidence recommends nothing and says why" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);
    _ = try conn.execParams("insert into projects (slug, name, root_path) values ('p','p','/p')", &.{});

    var result = try rank(&conn, a, .{
        .project_id = 1,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
    }, .{});
    defer result.deinit(a);

    try testing.expectEqual(@as(usize, 0), result.rows.len);
    try testing.expect(result.recommended == null);
    try testing.expectEqualStrings(
        "no cohort-eligible declared-experiment samples",
        result.no_recommendation_reason.?,
    );
}

test "rank: a neighbouring cohort's evidence never leaks into this one" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    const ok: Sample = .{ .state = "completed", .success = true, .attempts = 1 };
    const rows = try repeat(a, 8, ok);
    defer a.free(rows);
    try seedCohort(&conn, a, &.{.{ .name = "cand-A", .order = 0, .samples = rows }});

    const base: Cohort = .{
        .project_id = 1,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
    };

    // The seeded cohort ranks the candidate.
    var here = try rank(&conn, a, base, .{});
    defer here.deinit(a);
    try testing.expectEqual(@as(usize, 1), here.rows.len);
    try testing.expectEqual(@as(u64, 8), here.rows[0].samples);

    // Every neighbouring cohort differs in exactly ONE dimension. Each must
    // see zero samples: eight successes at `feature`/`standard` say nothing
    // about `architectural` work, a different tier, a different role, or a
    // run under a different validation policy. Pooling any of them would let
    // evidence launder across the boundary it was collected in.
    var vendor_other = base;
    vendor_other.vendor = "vendor-y";
    var role_other = base;
    role_other.role = "reviewer";
    var tier_other = base;
    tier_other.tier = .large;
    var work_other = base;
    work_other.work_type = .architectural;
    var complexity_other = base;
    complexity_other.complexity = .high_risk;
    var validation_other = base;
    validation_other.validation_policy_version = "val-v2";
    var routing_other = base;
    routing_other.routing_policy_version = "route-v2";
    var project_other = base;
    project_other.project_id = 999;

    for ([_]Cohort{
        vendor_other,  role_other,       tier_other,
        work_other,    complexity_other, validation_other,
        routing_other, project_other,
    }) |neighbour| {
        var r = try rank(&conn, a, neighbour, .{});
        defer r.deinit(a);
        try testing.expectEqual(@as(usize, 0), r.rows.len);
        try testing.expect(r.recommended == null);
        try testing.expectEqualStrings(
            "no cohort-eligible declared-experiment samples",
            r.no_recommendation_reason.?,
        );
    }
}

test "rank: a candidate-mismatch sample is retained for audit but never counted" {
    const a = testing.allocator;
    var conn = try db.sqlite.Db.openMemory();
    defer conn.close();
    try db.migrate.applyAll(&conn, a);

    const ok: Sample = .{ .state = "completed", .success = true, .attempts = 1 };
    const rows = try repeat(a, 6, ok);
    defer a.free(rows);
    try seedCohortWith(
        &conn,
        a,
        &.{.{ .name = "cand-A", .order = 0, .samples = rows }},
        &.{"lwi-mismatch"},
    );

    // Terminal samples are immutable, and the identity trigger admits only
    // declared-experiment dispatches — so an observational run never produces
    // a sample row at all. The exclusion that CAN appear here is a candidate
    // mismatch: the host answered with a different model than was requested,
    // which says nothing about the requested candidate's quality.
    const exp = try conn.intQuery("select id from routing_experiments limit 1");
    const cand = try conn.intQuery("select id from routing_candidates limit 1");

    const dispatch = try conn.execParams(
        \\insert into routing_dispatch_snapshots (
        \\  dispatch_key, logical_work_item_id, project_id, validation_policy_version,
        \\  vendor, role, tier, work_type, complexity, routing_policy_version,
        \\  profile_rule_version, packet_digest, policy_digest, capability_digest,
        \\  requested_candidate_id, actual_vendor, actual_candidate_id,
        \\  assignment_class, experiment_id, operator_decision, reviewer_disposition,
        \\  terminal_state, confirmed_at
        \\) values (
        \\  'dk-mismatch','lwi-mismatch',1,'val-v1','vendor-x','coder','medium','feature',
        \\  'standard','route-v1','pr','pk','po','ca',?,'vendor-x','something-else',
        \\  'declared_experiment',?,'confirmed','approved','candidate_mismatch',
        \\  '2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .int = cand }, .{ .int = exp } });

    _ = try conn.execParams(
        \\insert into routing_dispatch_events (
        \\  dispatch_id, event_id, sequence, event_kind, attempt_number,
        \\  terminal_state, payload_json, occurred_at
        \\) values (?, 'ev-mismatch', 999, 'outcome', 1, 'candidate_mismatch', '{}', '2026-01-01T00:00:00Z')
    , &.{.{ .int = dispatch }});

    _ = try conn.execParams(
        \\insert into routing_terminal_samples (
        \\  experiment_id, logical_work_item_id, role, initial_packet_digest,
        \\  candidate_id, project_id, validation_policy_version, routing_policy_version,
        \\  vendor, tier, work_type, complexity, terminal_event_id, terminal_state,
        \\  quality_success, cohort_eligible, exclusion_reason, finalized_at
        \\) values (
        \\  ?, 'lwi-mismatch', 'coder', 'pk', ?, 1, 'val-v1', 'route-v1', 'vendor-x',
        \\  'medium', 'feature', 'standard', 'ev-mismatch', 'candidate_mismatch',
        \\  0, 0, 'candidate_mismatch', '2026-01-01T00:00:00Z'
        \\)
    , &.{ .{ .int = exp }, .{ .int = cand } });

    var result = try rank(&conn, a, .{
        .project_id = 1,
        .validation_policy_version = "val-v1",
        .routing_policy_version = "route-v1",
        .vendor = "vendor-x",
        .role = "coder",
        .tier = .medium,
        .work_type = .feature,
        .complexity = .standard,
    }, .{});
    defer result.deinit(a);

    // Seven samples on record, six counted. Counting the seventh would blame
    // the requested candidate for a substitution it did not make.
    try testing.expectEqual(@as(u64, 6), result.rows[0].samples);
    try testing.expectEqual(@as(u64, 6), result.rows[0].successes);
    try testing.expectEqual(
        @as(i64, 7),
        try conn.intQuery("select count(*) from routing_terminal_samples"),
    );
}

// --- optional comparable metrics --------------------------------------------

test "optional metrics only participate when BOTH candidates have them" {
    var a = mkRow("a", 20, 19, 0);
    var b = mkRow("b", 20, 19, 1);
    // Identical evidence, so latency would decide — but only if measured.
    try testing.expectEqual(a.wilson_lower, b.wilson_lower);

    a.mean_latency_ms = 100;
    b.mean_latency_ms = null;
    // b is unmeasured. Treating null as 0 would promote the candidate we know
    // LEAST about; treating it as infinity would bury a candidate merely for
    // not being instrumented. Neither is supported by evidence, so the metric
    // sits out and configured order decides.
    try testing.expect(lessThan({}, a, b));

    b.mean_latency_ms = 50;
    try testing.expect(lessThan({}, b, a));
}

test "cost breaks a tie only after latency, and only when comparable" {
    var a = mkRow("a", 20, 19, 0);
    var b = mkRow("b", 20, 19, 1);
    a.mean_latency_ms = 100;
    b.mean_latency_ms = 100;
    a.mean_cost_micros = 900;
    b.mean_cost_micros = 100;
    try testing.expect(lessThan({}, b, a));

    // With cost unmeasured on one side, configured order decides again.
    b.mean_cost_micros = null;
    try testing.expect(lessThan({}, a, b));
}

test "an unmeasured candidate is never ranked as instant and free" {
    // The concrete failure a zero default would cause: a candidate with no
    // measurements outranking a measured, genuinely fast one.
    var measured = mkRow("measured", 20, 19, 1);
    measured.mean_latency_ms = 10;
    measured.mean_cost_micros = 10;
    const unmeasured = mkRow("unmeasured", 20, 19, 0);
    try testing.expect(unmeasured.mean_latency_ms == null);
    try testing.expect(unmeasured.mean_cost_micros == null);

    var rows = [_]Row{ unmeasured, measured };
    std.mem.sort(Row, &rows, {}, sortEligibleFirst);
    // Order falls through to configured order rather than being decided by
    // absent data — the unmeasured row does not WIN on a phantom zero.
    try testing.expectEqualStrings("unmeasured", rows[0].candidate);
    try testing.expect(rows[0].mean_latency_ms == null);
}
