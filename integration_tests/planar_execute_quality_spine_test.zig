//! integration_tests/planar_execute_quality_spine_test.zig —
//!   quality-spine workflow template smoke tests (plan 492 M10 task 3205).
//!
//! Black-box integration: exercises the canonical `workflows/quality-spine.lua`
//! template (and the parallel-fanout.lua template) against a seeded fixture DB
//! under `--mock-worker` so the full agent() pipeline (claim → brief → spawn
//! → wait → terminal decision → result table) runs end-to-end against the
//! FakeSpawner without any real `claude -p` spawn.
//!
//! Tests:
//!   1. quality-spine.lua PARSES + exits 0 under --dry-run.
//!   2. quality-spine.lua runs under --mock-worker against a seeded plan:
//!      - exits 0
//!      - per-task coder→reviewer log lines appear on stdout
//!   3. 3206 interaction: quality-spine has meta.reviewer = true → PASSES the
//!      bright-line guard when plan has a migration-touching task.
//!   4. parallel-fanout.lua parses + exits 0 under --dry-run.
//!   5. parallel-fanout.lua runs under --mock-worker: exits 0, fan-out log line.
//!
//! ## Hermeticity
//!
//! Same shape as planar_execute_refusal_guard_test:
//!   - Fixture DB seeded via the real `planar` CLI (harness Suite injects
//!     PLANAR_DB = temp fixture DB).
//!   - planar-execute run with PLANAR_DB pointed at the fixture and PATH
//!     prepended with the freshly-built bin dir.
//!   - PLANAR_EXECUTE_LIVE_AGENT is intentionally NOT set.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers
// ---------------------------------------------------------------------------

fn envValue(key: []const u8) ?[]const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key) and s.len > key.len and s[key.len] == '=') {
            return s[key.len + 1 ..];
        }
    }
    return null;
}

fn resolveExecuteBin() []const u8 {
    return envValue("PLANAR_EXECUTE_BIN") orelse
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: zig build test-integration");
}

fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: zig build test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// RunResult + runner
// ---------------------------------------------------------------------------

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: RunResult) u32 {
        return switch (self.term) {
            .exited => |code| code,
            else => 255,
        };
    }
};

/// runExecuteWithFixture spawns `planar-execute <args...>` with:
///   PLANAR_DB = suite's fixture DB
///   PATH = <bin_dir>:<original PATH>
/// PLANAR_EXECUTE_LIVE_AGENT is intentionally NOT set.
fn runExecuteWithFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveExecuteBin());
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", suite.absDbPath());

    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    _ = env_map.swapRemove("PLANAR_EXECUTE_LIVE_AGENT");

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .environ_map = &env_map,
    });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

// ---------------------------------------------------------------------------
// Workflow file helpers
// ---------------------------------------------------------------------------

/// workflowsDir returns the absolute path to the repo-root workflows/ directory.
/// Derived from PLANAR_BIN's directory (../../workflows from bin/).
fn workflowsDir(gpa: std.mem.Allocator) ![]u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN not set");
    // PLANAR_BIN = <repo>/zig-out/bin/planar → parent → parent → workflows
    const bin_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const zig_out_dir = std.fs.path.dirname(bin_dir) orelse ".";
    const repo_root = std.fs.path.dirname(zig_out_dir) orelse ".";
    return std.fs.path.join(gpa, &.{ repo_root, "workflows" });
}

// ---------------------------------------------------------------------------
// Fixture seeding helpers
// ---------------------------------------------------------------------------

const PlanJSON = struct { id: i64 };
const TaskJSON = struct { id: i64 };

fn addTask(suite: *harness.Suite, arena: std.mem.Allocator, pid: []const u8, title: []const u8) i64 {
    const t = suite.mustRunJSON(TaskJSON, arena, &.{ "task", "add", "--plan", pid, "--json", title });
    return t.id;
}

fn touchPath(suite: *harness.Suite, repo_slug: []const u8, task_id: i64, path: []const u8) void {
    const gpa = suite.allocator;
    const tid = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch unreachable;
    defer gpa.free(tid);
    const out = suite.mustRun(&.{ "task", "touches", "add", tid, repo_slug, "--path", path });
    gpa.free(out);
}

fn registerRepoSlug(suite: *harness.Suite, arena: std.mem.Allocator) []const u8 {
    _ = suite.registerProject("qs-repo");
    const assoc_slug = "qs-org";
    const cr = suite.mustRun(&.{ "assoc", "create", assoc_slug, "--kind", "org" });
    suite.allocator.free(cr);
    const root = suite.tmpAbsPath();
    const ad = suite.mustRun(&.{ "assoc", "add", assoc_slug, root });
    suite.allocator.free(ad);

    const Member = struct { id: i64, slug: []const u8, name: []const u8 };
    const members = suite.mustRunJSON([]Member, arena, &.{ "assoc", "members", assoc_slug, "--json" });
    for (members) |m| {
        if (std.mem.eql(u8, m.name, "qs-repo")) return m.slug;
    }
    std.debug.panic("registered repo slug not found in assoc members", .{});
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

test "quality-spine.lua: --dry-run parses the template and exits 0 (task 3205)" {
    // Confirms the Lua module structure is valid (meta.name, description,
    // phases, run function) and that the --dry-run path never enters run().
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const wf_dir = try workflowsDir(gpa);
    defer gpa.free(wf_dir);
    const wf_path = try std.fs.path.join(gpa, &.{ wf_dir, "quality-spine.lua" });
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nquality-spine dry-run stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // Dry-run must print the workflow name.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "quality-spine") != null);
    // Phase titles must appear.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "Plan") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "Cycle") != null);
}

test "quality-spine.lua: --mock-worker run against a seeded plan exits 0 + coder/reviewer log lines (task 3205)" {
    // THE DELIVERABLE: the canonical workflow template runs under --mock-worker
    // against a real seeded plan. Asserts:
    //   - exit 0
    //   - the "eligible: N" log line appears (ctx.eligible was called)
    //   - per-task coder log line appears ("coder: status=")
    //   - per-task reviewer log line appears ("reviewer: status=")
    //   - the "quality-spine cycle complete" log appears
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    // Seed a plan with two disjoint-touch tasks (both eligible, no overlap).
    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "qs-mock-plan", "--json", "QS_MOCK_PLAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    const t_a = addTask(&suite, arena, pid, "alpha-task");
    touchPath(&suite, repo, t_a, "src/engine/alpha.zig");
    const t_b = addTask(&suite, arena, pid, "beta-task");
    touchPath(&suite, repo, t_b, "src/engine/beta.zig");

    const wf_dir = try workflowsDir(gpa);
    defer gpa.free(wf_dir);
    const wf_path = try std.fs.path.join(gpa, &.{ wf_dir, "quality-spine.lua" });
    defer gpa.free(wf_path);

    // Pass plan_id as ctx.args[1] (the workflow reads it via ctx.args[1]).
    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path, pid });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nquality-spine mock stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // ctx.eligible was called — "eligible: N" appears in output.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "eligible: 2") != null or
        std.mem.indexOf(u8, res.stdout, "eligible:") != null);

    // Per-task coder dispatch log line.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "coder: status=") != null);

    // Per-task reviewer dispatch log line.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "reviewer: status=") != null);

    // Workflow completion log line.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "quality-spine cycle complete") != null);

    // MOCK MODE banner was printed (the FakeSpawner ran).
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}

test "quality-spine.lua: meta.reviewer=true PASSES 3206 guard on migration-touching plan (task 3205 + 3206)" {
    // 3206 interaction: seed a plan with a migration-touching task (normally
    // causes a bright-line refusal when meta.reviewer is absent/false).
    // quality-spine.lua declares meta.reviewer = true, so the guard PASSES.
    // run() enters normally and the workflow exits 0.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "qs-guard-plan", "--json", "QS_GUARD_PLAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // A migration-touching task — the guard fires on this if meta.reviewer is false.
    const t_mig = addTask(&suite, arena, pid, "add-migration");
    touchPath(&suite, repo, t_mig, "migrations/00099_widget.up.sql");

    const wf_dir = try workflowsDir(gpa);
    defer gpa.free(wf_dir);
    const wf_path = try std.fs.path.join(gpa, &.{ wf_dir, "quality-spine.lua" });
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path, pid });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nqs-guard stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    // Must exit 0 — meta.reviewer = true satisfies the guard.
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    // No refusal message.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "REFUSING TO RUN") == null);
    // Workflow entered run() and printed eligible output (guard was bypassed by meta.reviewer=true).
    // The migration task is "serialized" (not eligible), so "no eligible tasks" is expected.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "eligible:") != null);
}

test "parallel-fanout.lua: --dry-run parses the template and exits 0 (task 3205)" {
    // Confirms the parallel-fanout template has valid module structure.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const wf_dir = try workflowsDir(gpa);
    defer gpa.free(wf_dir);
    const wf_path = try std.fs.path.join(gpa, &.{ wf_dir, "parallel-fanout.lua" });
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--dry-run", wf_path });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nparallel-fanout dry-run stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "parallel-fanout") != null);
}

test "parallel-fanout.lua: --mock-worker run against a seeded plan exits 0 + fan-out log lines (task 3205)" {
    // Confirms the parallel-fanout template exercises ctx.parallel() under the
    // mock driver: seeds a plan with 2 disjoint tasks (fan_out_available = true),
    // runs the template, and asserts on the log output.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    var arena_state = std.heap.ArenaAllocator.init(gpa);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const repo = registerRepoSlug(&suite, arena);
    const plan = suite.mustRunJSON(PlanJSON, arena, &.{
        "plan", "create", "--slug", "pf-mock-plan", "--json", "PF_MOCK_PLAN",
    });
    const pid = std.fmt.allocPrint(arena, "{d}", .{plan.id}) catch unreachable;

    // Two disjoint tasks — fan_out_available = true.
    const t_a = addTask(&suite, arena, pid, "alpha");
    touchPath(&suite, repo, t_a, "src/engine/alpha2.zig");
    const t_b = addTask(&suite, arena, pid, "beta");
    touchPath(&suite, repo, t_b, "src/engine/beta2.zig");

    const wf_dir = try workflowsDir(gpa);
    defer gpa.free(wf_dir);
    const wf_path = try std.fs.path.join(gpa, &.{ wf_dir, "parallel-fanout.lua" });
    defer gpa.free(wf_path);

    const res = try runExecuteWithFixture(gpa, &suite, &.{ "run", "--mock-worker", "--plan", pid, wf_path, pid });
    defer res.deinit();

    if (res.exitCode() != 0) {
        std.debug.print("\nparallel-fanout mock stdout: {s}\nstderr: {s}\n", .{ res.stdout, res.stderr });
    }
    try std.testing.expectEqual(@as(u32, 0), res.exitCode());

    // fan-out log line.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "fanning out") != null);

    // Per-branch outcome log lines (the thunks ran).
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "branch 1 outcome:") != null);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "branch 2 outcome:") != null);

    // Completion line.
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "parallel-fanout complete") != null);

    // MOCK MODE banner.
    try std.testing.expect(std.mem.indexOf(u8, res.stderr, "MOCK MODE") != null);
}
