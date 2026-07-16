//! integration_tests/bench_lifecycle_test.zig — black-box integration tests
//! for the `planar bench *` verb group (M1.3 of plan 635).
//!
//! Tests walk the full measurement-rig lifecycle via the compiled binary and
//! assert on JSON output from `bench show --json`. Exit code 0 is not
//! sufficient — the JSON shape and its contents are verified.
//!
//! Covered scenarios:
//!
//!   - Full lifecycle: start → event → touch (declared) → harvest (actual)
//!     → finish → show --json; asserts status, events, and touches.
//!   - Invalid --kind on `bench touch` exits non-zero (enum guard).
//!   - Invalid --status on `bench finish` exits non-zero (enum guard).
//!   - Missing run_uid on `bench event` exits non-zero (not found).
//!   - `bench show` on a missing uid exits non-zero (not found).
//!
//! Git-fixture note: the harvest subtest creates a throwaway git repo under
//! the suite tmp dir so `harvest` has a real worktree to diff. The fixture
//! uses `git -C <dir>` for all git commands — no cd, no global config leak.

const std = @import("std");
const harness = @import("harness");

// -------------------------------------------------------------------------
// JSON shapes for bench show output.
// -------------------------------------------------------------------------

const ShowRun = struct {
    id: i64 = 0,
    run_uid: []const u8 = "",
    plan_id: i64 = 0,
    arm: []const u8 = "",
    base_sha: []const u8 = "",
    config_hash: []const u8 = "",
    // config_json is embedded as a raw JSON value (object/null), not a
    // JSON-encoded string, because the engine stores and emits it verbatim.
    config_json: ?std.json.Value = null,
    corpus_repo: ?[]const u8 = null,
    status: []const u8 = "",
    started_at: []const u8 = "",
    ended_at: ?[]const u8 = null,
    events: []const ShowEvent = &.{},
    touches: []const ShowTouch = &.{},
};

const ShowEvent = struct {
    id: i64 = 0,
    seq: i64 = 0,
    kind: []const u8 = "",
    payload: ?std.json.Value = null,
    created_at: []const u8 = "",
};

const ShowTouch = struct {
    id: i64 = 0,
    task_id: i64 = 0,
    path: []const u8 = "",
    kind: []const u8 = "",
    created_at: []const u8 = "",
};

const PlanId = struct {
    id: i64,
    scope_kind: []const u8,
    scope_id: ?i64 = null,
};

// -------------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------------

/// Seed a project + plan; return the plan id as an owned string.
fn seedPlan(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    const root = suite.registerProject("bench-test");
    const p = suite.mustRunInDir(root, &.{ "plan", "create", "--json", "--scope", "global", "Bench test plan" });
    defer suite.allocator.free(p);
    const plan = std.json.parseFromSlice(PlanId, arena, p, .{
        .allocate = .alloc_always,
        .ignore_unknown_fields = true,
    }) catch @panic("parseFromSlice PlanId failed");
    std.testing.expectEqualStrings("global", plan.value.scope_kind) catch
        @panic("bench lifecycle seed plan must be globally owned");
    std.testing.expect(plan.value.scope_id == null) catch
        @panic("globally owned bench lifecycle seed plan must not have a scope id");
    return std.fmt.allocPrint(arena, "{d}", .{plan.value.id}) catch @panic("OOM");
}

/// Stand up a minimal git repo under `dir`, write `filename` with `content`,
/// commit it, then modify `filename` so the working tree is dirty.
/// Returns the initial HEAD sha (owned by arena) so the test can use a
/// range diff later if needed.
fn setupGitFixture(
    suite: *harness.Suite,
    arena: std.mem.Allocator,
    dir: []const u8,
    filename: []const u8,
) []const u8 {
    const gpa = suite.allocator;

    // Run git commands with -C <dir>; never cd — preserves test isolation.
    const run = struct {
        fn call(a: std.mem.Allocator, args: []const []const u8) void {
            const res = std.process.run(a, std.testing.io, .{ .argv = args }) catch
                @panic("git spawn failed");
            defer a.free(res.stdout);
            defer a.free(res.stderr);
            if (res.term != .exited or res.term.exited != 0) {
                std.debug.print("git failed (exit {any})\nstderr: {s}\n", .{ res.term, res.stderr });
                @panic("git command failed in setupGitFixture");
            }
        }
    };

    run.call(gpa, &.{ "git", "-C", dir, "init" });
    run.call(gpa, &.{ "git", "-C", dir, "checkout", "-b", "main" });
    run.call(gpa, &.{ "git", "-C", dir, "config", "user.email", "planar@example.com" });
    run.call(gpa, &.{ "git", "-C", dir, "config", "user.name", "Planar Test" });

    // Write and commit baseline file.
    const filepath = std.fs.path.join(gpa, &.{ dir, filename }) catch @panic("OOM");
    defer gpa.free(filepath);
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = filepath,
        .data = "v1\n",
    }) catch @panic("writeFile v1 failed");

    run.call(gpa, &.{ "git", "-C", dir, "add", "." });
    run.call(gpa, &.{ "git", "-C", dir, "commit", "-m", "base" });

    // Capture HEAD sha.
    const sha_res = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "git", "-C", dir, "rev-parse", "HEAD" },
    }) catch @panic("rev-parse spawn failed");
    defer gpa.free(sha_res.stderr);
    const raw_sha = std.mem.trim(u8, sha_res.stdout, " \t\r\n");
    const sha = arena.dupe(u8, raw_sha) catch @panic("OOM sha");
    gpa.free(sha_res.stdout);

    // Dirty the working tree so harvest has something to collect.
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = filepath,
        .data = "v2\n",
    }) catch @panic("writeFile v2 failed");
    run.call(gpa, &.{ "git", "-C", dir, "add", "." });

    return sha;
}

// -------------------------------------------------------------------------
// Test 1: Full lifecycle via CLI — start → event → touch → harvest → finish
// → show --json.
// -------------------------------------------------------------------------

test "bench: full lifecycle start → event → touch → harvest → finish → show --json" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);
    const run_uid = "01JBENCH0001LIFECYCLE0001";

    // Step 1: bench start.
    const started = suite.mustRun(&.{
        "bench",                "start",
        run_uid,                "--plan",
        plan_id,                "--arm",
        "strict",               "--base-sha",
        "deadbeef",             "--config-hash",
        "cfg-hash-1",           "--config-json",
        "{\"model\":\"test\"}", "--corpus-repo",
        "test/corpus",
    });
    defer gpa.free(started);
    try std.testing.expectEqualStrings(run_uid, std.mem.trim(u8, started, " \r\n"));

    // Step 2: bench event.
    const ev_out = suite.mustRun(&.{
        "bench",  "event",        run_uid,
        "--kind", "token_sample", "--seq",
        "1",      "--payload",    "{\"in\":100,\"out\":50}",
    });
    defer gpa.free(ev_out);

    // Step 3: bench touch (declared).
    const touch_out = suite.mustRun(&.{
        "bench",        "touch",  run_uid,
        "--task",       "42",     "--path",
        "src/main.zig", "--kind", "declared",
    });
    defer gpa.free(touch_out);

    // Step 4: bench harvest — set up a real git fixture so harvest has
    // something to collect.
    const git_dir = suite.freshSystemTmpDir();
    _ = setupGitFixture(&suite, arena, git_dir, "harvest_target.txt");

    const harvest_out = suite.mustRun(&.{
        "bench",  "harvest", run_uid,
        "--task", "42",      "--worktree",
        git_dir,
    });
    defer gpa.free(harvest_out);
    // harvest prints the count of actual-touch rows written.
    const harvest_n = std.fmt.parseInt(usize, std.mem.trim(u8, harvest_out, " \r\n"), 10) catch
        @panic("harvest output is not an integer");
    try std.testing.expect(harvest_n >= 1);

    // Step 5: bench finish.
    const finish_out = suite.mustRun(&.{
        "bench",    "finish",    run_uid,
        "--status", "completed",
    });
    defer gpa.free(finish_out);

    // Step 6: bench show --json; assert the full shape.
    const run = suite.mustRunJSON(ShowRun, arena, &.{ "bench", "show", run_uid, "--json" });

    try std.testing.expectEqualStrings(run_uid, run.run_uid);
    try std.testing.expectEqualStrings("strict", run.arm);
    try std.testing.expectEqualStrings("deadbeef", run.base_sha);
    try std.testing.expectEqualStrings("cfg-hash-1", run.config_hash);
    try std.testing.expectEqualStrings("completed", run.status);
    try std.testing.expect(run.ended_at != null);
    try std.testing.expect(run.corpus_repo != null);
    try std.testing.expectEqualStrings("test/corpus", run.corpus_repo.?);
    // config_json is embedded as a raw JSON object, not a string.
    try std.testing.expect(run.config_json != null);

    // Events: we inserted exactly one.
    try std.testing.expectEqual(@as(usize, 1), run.events.len);
    try std.testing.expectEqual(@as(i64, 1), run.events[0].seq);
    try std.testing.expectEqualStrings("token_sample", run.events[0].kind);
    // payload is not null.
    try std.testing.expect(run.events[0].payload != null);

    // Touches: one declared (bench touch), plus ≥1 actual (bench harvest).
    const declared_count = blk: {
        var n: usize = 0;
        for (run.touches) |t| {
            if (std.mem.eql(u8, t.kind, "declared")) n += 1;
        }
        break :blk n;
    };
    const actual_count = blk: {
        var n: usize = 0;
        for (run.touches) |t| {
            if (std.mem.eql(u8, t.kind, "actual")) n += 1;
        }
        break :blk n;
    };
    try std.testing.expectEqual(@as(usize, 1), declared_count);
    try std.testing.expect(actual_count >= 1);

    // The declared touch has the right path and task_id.
    for (run.touches) |t| {
        if (std.mem.eql(u8, t.kind, "declared")) {
            try std.testing.expectEqual(@as(i64, 42), t.task_id);
            try std.testing.expectEqualStrings("src/main.zig", t.path);
        }
    }
}

// -------------------------------------------------------------------------
// Test 2: Text output (no --json) — ensure it doesn't crash.
// -------------------------------------------------------------------------

test "bench: show without --json emits human-readable text" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);
    const run_uid = "01JBENCH0002TEXT0000000001";

    const s = suite.mustRun(&.{
        "bench",       "start",
        run_uid,       "--plan",
        plan_id,       "--arm",
        "eligibility", "--base-sha",
        "abc123",      "--config-hash",
        "h2",
    });
    defer gpa.free(s);

    const text = suite.mustRun(&.{ "bench", "show", run_uid });
    defer gpa.free(text);
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, run_uid));
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, "eligibility"));
    try std.testing.expect(std.mem.containsAtLeast(u8, text, 1, "running"));
}

// -------------------------------------------------------------------------
// Test 3: Invalid --kind on bench touch exits non-zero (enum guard).
// -------------------------------------------------------------------------

test "bench: invalid touch --kind exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);
    const run_uid = "01JBENCH0003INVALIDKIND001";

    const s = suite.mustRun(&.{
        "bench",  "start",
        run_uid,  "--plan",
        plan_id,  "--arm",
        "strict", "--base-sha",
        "abc",    "--config-hash",
        "h",
    });
    defer gpa.free(s);

    const stderr = suite.expectFailure(&.{
        "bench",   "touch",  run_uid,
        "--task",  "1",      "--path",
        "foo.zig", "--kind", "INVALID",
    });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "INVALID"));
}

// -------------------------------------------------------------------------
// Test 4: Invalid --status on bench finish exits non-zero (enum guard).
// -------------------------------------------------------------------------

test "bench: invalid finish --status exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const plan_id = seedPlan(&suite, arena);
    const run_uid = "01JBENCH0004INVALIDSTATUS1";

    const s = suite.mustRun(&.{
        "bench",  "start",
        run_uid,  "--plan",
        plan_id,  "--arm",
        "strict", "--base-sha",
        "abc",    "--config-hash",
        "h",
    });
    defer gpa.free(s);

    const stderr = suite.expectFailure(&.{
        "bench",    "finish", run_uid,
        "--status", "done",
    });
    defer gpa.free(stderr);
    try std.testing.expect(std.mem.containsAtLeast(u8, stderr, 1, "done"));
}

// -------------------------------------------------------------------------
// Test 5: bench show on a missing uid exits non-zero.
// -------------------------------------------------------------------------

test "bench: show on missing uid exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject("bench-missing");

    const stderr = suite.expectFailure(&.{ "bench", "show", "no-such-run-uid-xxxx" });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}

// -------------------------------------------------------------------------
// Test 6: bench event on a missing uid exits non-zero.
// -------------------------------------------------------------------------

test "bench: event on missing uid exits non-zero" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    _ = suite.registerProject("bench-event-missing");

    const stderr = suite.expectFailure(&.{
        "bench",  "event",        "no-such-run-uid-yyyy",
        "--kind", "token_sample", "--seq",
        "1",
    });
    defer gpa.free(stderr);
    try std.testing.expect(stderr.len > 0);
}
