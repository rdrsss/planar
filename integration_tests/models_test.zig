//! integration_tests/models_test.zig — `planar models` (plan 540/543)
//!
//! Black-box coverage of the provider/model discovery verb:
//!   - `models list` text + JSON shape (probes claude/codex, curated catalog,
//!     default role→tier→model routing).
//!   - `models refresh` writes the catalog cache under PLANAR_HOME/models/.
//!
//! Asserts STRUCTURE, never installed-state: whether claude/codex are present
//! is machine-dependent, but the vendors, their curated catalogs, and the
//! default routing are invariant.

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// planar models evals (plan 898/904, tech-spec 520 D8) — routing evals
// scorecard. Seeds real dispatch history through planar / planar-agent (not
// raw SQL) so the aggregator exercises the same code paths an orchestrator
// dispatch would produce.
// =========================================================================

fn resolveAgentBin() []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, "PLANAR_AGENT_BIN=")) {
            return s["PLANAR_AGENT_BIN=".len..];
        }
    }
    @panic(
        \\PLANAR_AGENT_BIN is not set.
        \\Run integration tests via: make test-integration (which sets it).
    );
}

fn runAgent(suite: *const harness.Suite, args: []const []const u8) harness.Suite.RunResult {
    const gpa = suite.allocator;
    var argv_list: std.ArrayList([]const u8) = .empty;
    defer argv_list.deinit(gpa);
    argv_list.append(gpa, resolveAgentBin()) catch @panic("OOM");
    for (args) |a| argv_list.append(gpa, a) catch @panic("OOM");

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM creating env map");
    defer env_map.deinit();
    env_map.put("PLANAR_DB", suite.db_path) catch @panic("OOM injecting PLANAR_DB");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv_list.items,
        .environ_map = &env_map,
    }) catch |e| std.debug.panic("runAgent spawn failed: {s}", .{@errorName(e)});
    return .{ .stdout = result.stdout, .stderr = result.stderr, .term = result.term };
}

fn mustRunAgent(suite: *const harness.Suite, args: []const []const u8) []u8 {
    const gpa = suite.allocator;
    const res = runAgent(suite, args);
    defer gpa.free(res.stderr);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("planar-agent failed (term={any}):\n{s}\nstderr: {s}\n", .{ res.term, res.stdout, res.stderr });
        @panic("planar-agent must-run failed");
    }
    return res.stdout;
}

/// `std.json.Stringify` renders a whole-number f64 without a decimal point
/// (e.g. `2.0` → `2`), so the parser reads it back as `.integer`, not
/// `.float`. Read either representation as f64.
fn jsonNumberAsF64(value: std.json.Value) f64 {
    return switch (value) {
        .integer => |i| @floatFromInt(i),
        .float => |f| f,
        else => @panic("expected a JSON number"),
    };
}

fn evalsExtractIntField(json: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, json, key) orelse return null;
    var i = idx + key.len;
    while (i < json.len and (json[i] == ' ' or json[i] == ':' or json[i] == '\t')) i += 1;
    var end = i;
    while (end < json.len and json[end] >= '0' and json[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, json[i..end], 10) catch null;
}

fn evalsExtractStringField(gpa: std.mem.Allocator, json: []const u8, prefix: []const u8) ![]u8 {
    const idx = std.mem.indexOf(u8, json, prefix) orelse return error.FieldNotFound;
    const i = idx + prefix.len;
    var end = i;
    while (end < json.len and json[end] != '"') end += 1;
    if (end == json.len) return error.UnterminatedString;
    return try gpa.dupe(u8, json[i..end]);
}

/// Seed a plan with one todo task; returns the plan id arg (caller frees).
fn evalsSeedPlan(suite: *const harness.Suite, slug: []const u8) []u8 {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = evalsExtractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    return std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");
}

fn evalsAddTask(suite: *const harness.Suite, plan_id_arg: []const u8, title: []const u8) i64 {
    const gpa = suite.allocator;
    const out = suite.mustRun(&.{ "task", "add", "--plan", plan_id_arg, "--json", title });
    defer gpa.free(out);
    return evalsExtractIntField(out, "\"id\"") orelse @panic("no task id");
}

/// Direct-claim a task, RECORDING the vendor and model actually used, and
/// return its claim token (caller frees). The scorecard reads the vendor back
/// off the claim (plan 950 removed the catalog it used to infer it from), so a
/// claim without `--vendor`/`--model` yields no vendor on the row.
fn evalsClaimTask(suite: *const harness.Suite, task_id: i64, vendor: []const u8, model: []const u8) []u8 {
    const gpa = suite.allocator;
    const ref = std.fmt.allocPrint(gpa, "task:{d}", .{task_id}) catch @panic("OOM");
    defer gpa.free(ref);
    const out = mustRunAgent(suite, &.{
        "claim",  "--entity", ref,   "--vendor",
        vendor,   "--model",  model, "--no-locality-probe",
        "--json",
    });
    defer gpa.free(out);
    return evalsExtractStringField(gpa, out, "\"claim_token\":\"") catch @panic("no claim_token");
}

/// Emit one dispatch-shape note (`agents/orchestrator.md` step 8a) naming a
/// single task's {tier, candidate, work_type} in `model_choice`.
fn evalsCaptureDispatchNote(suite: *const harness.Suite, task_id: i64, tier: []const u8, candidate: []const u8, work_type: []const u8) void {
    const gpa = suite.allocator;
    const body = std.fmt.allocPrint(
        gpa,
        "dispatch_shape: strict\nmodel_choice: {{\"{d}\":{{\"tier\":\"{s}\",\"candidate\":\"{s}\",\"work_type\":\"{s}\"}}}}",
        .{ task_id, tier, candidate, work_type },
    ) catch @panic("OOM");
    defer gpa.free(body);
    gpa.free(suite.mustRun(&.{ "capture", "note", body }));
}

test "planar models evals --json: two candidates for the same work type rank by approval rate and iteration count (plan 898/904 D8)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id_arg = evalsSeedPlan(&suite, "evals-rank");
    defer gpa.free(plan_id_arg);

    // Task A: claude-opus-4-8, one dispatch cycle, approved.
    const task_a = evalsAddTask(&suite, plan_id_arg, "evals task A");
    evalsCaptureDispatchNote(&suite, task_a, "large", "claude-opus-4-8", "schema");
    const claim_a = evalsClaimTask(&suite, task_a, "claude", "claude-opus-4-8");
    defer gpa.free(claim_a);
    gpa.free(mustRunAgent(&suite, &.{ "complete", "--claim", claim_a, "--json" }));

    // Task B: claude-haiku-4-5, two dispatch cycles (request-changes loop-back
    // simulated by a second note against the same task), then aborted.
    const task_b = evalsAddTask(&suite, plan_id_arg, "evals task B");
    evalsCaptureDispatchNote(&suite, task_b, "large", "claude-haiku-4-5", "schema");
    evalsCaptureDispatchNote(&suite, task_b, "large", "claude-haiku-4-5", "schema");
    const claim_b = evalsClaimTask(&suite, task_b, "claude", "claude-haiku-4-5");
    defer gpa.free(claim_b);
    gpa.free(mustRunAgent(&suite, &.{ "fail", "--claim", claim_b, "--reason", "test abort", "--json" }));

    const stdout = suite.mustRun(&.{ "models", "evals", "--json" });
    defer gpa.free(stdout);

    const parsed = std.json.parseFromSlice(std.json.Value, arena, std.mem.trim(u8, stdout, " \n"), .{
        .allocate = .alloc_always,
    }) catch |e| {
        std.debug.print("\nmodels evals --json parse failed: {s}\n{s}\n", .{ @errorName(e), stdout });
        return error.TestUnexpectedResult;
    };
    const obj = parsed.value.object;

    const scorecard = obj.get("scorecard").?.array;
    var opus_rank: ?i64 = null;
    var haiku_rank: ?i64 = null;
    var haiku_dispatch_count: ?i64 = null;
    var haiku_avg_iterations: ?f64 = null;
    for (scorecard.items) |row| {
        const ro = row.object;
        if (!std.mem.eql(u8, ro.get("work_type").?.string, "schema")) continue;
        if (std.mem.eql(u8, ro.get("candidate").?.string, "claude-opus-4-8")) {
            opus_rank = ro.get("rank").?.integer;
            try std.testing.expectEqualStrings("claude", ro.get("vendor").?.string);
            try std.testing.expect(!ro.get("insufficient_data").?.bool);
        }
        if (std.mem.eql(u8, ro.get("candidate").?.string, "claude-haiku-4-5")) {
            haiku_rank = ro.get("rank").?.integer;
            haiku_dispatch_count = ro.get("dispatch_count").?.integer;
            haiku_avg_iterations = jsonNumberAsF64(ro.get("avg_iterations").?);
        }
    }
    try std.testing.expect(opus_rank != null and haiku_rank != null);
    try std.testing.expectEqual(@as(i64, 1), opus_rank.?);
    try std.testing.expectEqual(@as(i64, 2), haiku_rank.?);
    // One task ("evals task B") went through two dispatch cycles before
    // aborting: dispatch_count (distinct tasks) is 1; avg_iterations is 2.
    try std.testing.expectEqual(@as(i64, 1), haiku_dispatch_count.?);
    try std.testing.expectEqual(@as(f64, 2.0), haiku_avg_iterations.?);

    const recs = obj.get("recommendations").?.array;
    var saw_opus_rec = false;
    for (recs.items) |rec| {
        const ro = rec.object;
        if (std.mem.eql(u8, ro.get("work_type").?.string, "schema")) {
            saw_opus_rec = true;
            try std.testing.expectEqualStrings("claude-opus-4-8", ro.get("candidate").?.string);
        }
    }
    try std.testing.expect(saw_opus_rec);

    const signals = obj.get("signals_sourced").?.object;
    try std.testing.expect(signals.get("reviewer_disposition").?.bool);
    try std.testing.expect(signals.get("iteration_count").?.bool);
    try std.testing.expect(!signals.get("quality_gate_pass_fail").?.bool);
    try std.testing.expect(signals.get("test_coder_expansion").?.bool);
}
