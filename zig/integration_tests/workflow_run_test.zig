//! integration_tests/workflow_run_test.zig — `planar workflow run` verb
//! integration tests (plan 638, task 4346).
//!
//! Coverage:
//!
//!   1. Shipped workflow resolved by name, executed via planar-execute, and
//!      the flow.result JSON is forwarded to stdout (exit 0).
//!   2. Sandbox workflow resolved via --local flag; exit 0 + JSON forwarded.
//!   3. Unknown name → non-zero exit + error message (not-found path).
//!   4. Workflow that calls flow.fail → non-zero exit code propagated.
//!
//! The tests set PLANAR_WORKFLOWS_DIR / PLANAR_HOME to temporary directories
//! containing seeded .lua files, and PLANAR_EXECUTE_BIN to the built binary.
//! The harness's std.process.run captures planar's stdout, which includes the
//! forwarded planar-execute output because planar-execute inherits planar's
//! stdout pipe.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

/// writeWorkflow writes `body` to a file named `filename` under `dir`.
/// Returns the absolute path (allocator-owned).
fn writeWorkflow(gpa: std.mem.Allocator, dir: []const u8, filename: []const u8, body: []const u8) ![]const u8 {
    const path = try std.fs.path.join(gpa, &.{ dir, filename });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = body });
    return path;
}

// ---------------------------------------------------------------------------
// Test 1: shipped workflow resolved by name, forwarded JSON, exit 0
// ---------------------------------------------------------------------------

test "workflow run resolves a shipped workflow by name and forwards flow.result JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed a shipped workflow directory.
    const shipped_dir = suite.freshSystemTmpDir();

    // A simple deterministic workflow: no cli.planar shell needed.
    const wf =
        \\--[[ @meta
        \\name: run-smoke
        \\description: Smoke test for workflow run.
        \\phases: setup
        \\seam: planar workflow run
        \\--]]
        \\function setup()
        \\  flow.result({ ok = true, msg = "run-smoke-ok" })
        \\end
    ;
    const p = try writeWorkflow(gpa, shipped_dir, "run_smoke.lua", wf);
    defer gpa.free(p);

    const fake_home = suite.freshSystemTmpDir();
    const execute_bin = resolveEnv("PLANAR_EXECUTE_BIN");
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";

    // Prepend the planar binary dir to PATH so planar-execute can shell planar.
    const old_path_raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (old_path_raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(old_path_raw[0..env_count :null]);
    const base_environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try base_environ.createMap(gpa);
    defer env_map.deinit();
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    env_map.put("PATH", new_path) catch @panic("OOM");

    // Use execWith to pass extra env so PATH is in the env_map.
    // We build the extra_env list with all the overrides.
    const out = suite.mustRunWith(
        &.{ "workflow", "run", "run-smoke", "--phase", "setup" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = shipped_dir },
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_EXECUTE_BIN", .value = execute_bin },
            .{ .key = "PLANAR_CONFIG_PATH", .value = "/nonexistent-wfrun-test.toml" },
            .{ .key = "PATH", .value = new_path },
        },
    );
    defer gpa.free(out);

    // The forwarded JSON contains the flow.result fields.
    const Result = struct { ok: bool, msg: []const u8 };
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const parsed = try std.json.parseFromSlice(
        Result,
        arena.allocator(),
        std.mem.trim(u8, out, " \t\r\n"),
        .{ .ignore_unknown_fields = true },
    );
    try std.testing.expect(parsed.value.ok);
    try std.testing.expectEqualStrings("run-smoke-ok", parsed.value.msg);
}

// ---------------------------------------------------------------------------
// Test 2: sandbox workflow resolved via --local, forwarded JSON, exit 0
// ---------------------------------------------------------------------------

test "workflow run --local resolves a sandbox workflow and forwards flow.result JSON" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const fake_home = suite.freshSystemTmpDir();
    const local_wf_dir = try std.fs.path.join(gpa, &.{ fake_home, "local", "workflows" });
    defer gpa.free(local_wf_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_wf_dir);

    const wf =
        \\--[[ @meta
        \\name: local-smoke
        \\description: Sandbox workflow smoke test.
        \\phases: run
        \\seam: planar workflow run --local
        \\--]]
        \\function run()
        \\  flow.result({ is_local = true, label = "sandbox-smoke" })
        \\end
    ;
    const p = try writeWorkflow(gpa, local_wf_dir, "local_smoke.lua", wf);
    defer gpa.free(p);

    const empty_shipped = suite.freshSystemTmpDir();
    const execute_bin = resolveEnv("PLANAR_EXECUTE_BIN");
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const base_environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try base_environ.createMap(gpa);
    defer env_map.deinit();
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);

    const out = suite.mustRunWith(
        &.{ "workflow", "run", "local-smoke", "--phase", "run", "--local" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = empty_shipped },
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_EXECUTE_BIN", .value = execute_bin },
            .{ .key = "PLANAR_CONFIG_PATH", .value = "/nonexistent-wfrun-test.toml" },
            .{ .key = "PATH", .value = new_path },
        },
    );
    defer gpa.free(out);

    const Result = struct { is_local: bool, label: []const u8 };
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const parsed = try std.json.parseFromSlice(
        Result,
        arena.allocator(),
        std.mem.trim(u8, out, " \t\r\n"),
        .{ .ignore_unknown_fields = true },
    );
    try std.testing.expect(parsed.value.is_local);
    try std.testing.expectEqualStrings("sandbox-smoke", parsed.value.label);
}

// ---------------------------------------------------------------------------
// Test 3: unknown workflow name → non-zero exit + "not found" error message
// ---------------------------------------------------------------------------

test "workflow run exits non-zero when workflow name is not found" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const empty_shipped = suite.freshSystemTmpDir();
    const fake_home = suite.freshSystemTmpDir();
    const execute_bin = resolveEnv("PLANAR_EXECUTE_BIN");

    const stderr = suite.expectFailureWith(
        &.{ "workflow", "run", "no-such-workflow", "--phase", "go" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = empty_shipped },
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_EXECUTE_BIN", .value = execute_bin },
            .{ .key = "PLANAR_CONFIG_PATH", .value = "/nonexistent-wfrun-test.toml" },
        },
    );
    defer gpa.free(stderr);

    // The error message must name the missing workflow.
    try std.testing.expect(std.mem.indexOf(u8, stderr, "no-such-workflow") != null);
}

// ---------------------------------------------------------------------------
// Test 4: workflow that calls flow.fail → non-zero exit code propagated
// ---------------------------------------------------------------------------

test "workflow run propagates non-zero exit code from a flow.fail workflow" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const shipped_dir = suite.freshSystemTmpDir();

    // A workflow that always fails via flow.fail.
    const wf =
        \\--[[ @meta
        \\name: always-fail
        \\description: Always fails with flow.fail.
        \\phases: run
        \\seam: planar workflow run (negative test)
        \\--]]
        \\function run()
        \\  flow.fail("deliberate failure for run-propagation test")
        \\end
    ;
    const p = try writeWorkflow(gpa, shipped_dir, "always_fail.lua", wf);
    defer gpa.free(p);

    const fake_home = suite.freshSystemTmpDir();
    const execute_bin = resolveEnv("PLANAR_EXECUTE_BIN");
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const base_environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try base_environ.createMap(gpa);
    defer env_map.deinit();
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);

    const res = suite.execWith(
        &.{ "workflow", "run", "always-fail", "--phase", "run" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = shipped_dir },
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_EXECUTE_BIN", .value = execute_bin },
            .{ .key = "PLANAR_CONFIG_PATH", .value = "/nonexistent-wfrun-test.toml" },
            .{ .key = "PATH", .value = new_path },
        },
    );
    defer gpa.free(res.stdout);
    defer gpa.free(res.stderr);

    // planar-execute exits non-zero on flow.fail; planar workflow run propagates it.
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
}
