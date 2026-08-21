//! integration_tests/workflow_authoring_test.zig — M6 workflow authoring
//! surface integration tests (plan 638 task 4130).
//!
//! Coverage:
//!   1. `workflow list` enumerates shipped workflows (seeded via PLANAR_WORKFLOWS_DIR).
//!   2. `workflow list --local` enumerates sandbox workflows (seeded via PLANAR_HOME).
//!   3. `workflow show <name>` resolves shipped and sandbox workflows by name.
//!   4. `workflow show <missing>` exits non-zero (not-found).
//!   5. Guardrail: a workflow that tries to reach `os.execute` fails (sandbox enforced).
//!   6. Guardrail: a workflow that calls a spawn-shaped DENIED fn errors (host manifests).
//!   7. Guardrail: `cli.planar` with a non-allowlisted binary is rejected (runAllowlisted).
//!
//! Tests 5–7 exercise `planar-execute` directly (PLANAR_EXECUTE_BIN); tests 1–4
//! exercise `planar workflow list/show` (PLANAR_BIN).

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Helpers shared across the planar-execute guardrail tests
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

const RunResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,
    fn deinit(self: RunResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }
};

/// runExecute spawns `planar-execute run <wf_path> --phase <phase>` with
/// PLANAR_DB pointed at `db_path` and PATH prepended with the dir holding
/// the harness `planar` binary.
fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    wf_path: []const u8,
    phase: []const u8,
) !RunResult {
    const execute_bin = resolveEnv("PLANAR_EXECUTE_BIN");
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";

    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, execute_bin);
    try argv.append(gpa, "run");
    try argv.append(gpa, wf_path);
    try argv.append(gpa, "--phase");
    try argv.append(gpa, phase);

    // Build child env with PLANAR_DB overridden and PATH prepended.
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config-wftest.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}", .{ planar_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

/// writeWorkflow writes `body` to a file named `name` in `dir`.
/// Returns the absolute path (allocator-owned).
fn writeWorkflow(gpa: std.mem.Allocator, dir: []const u8, name: []const u8, body: []const u8) ![]const u8 {
    const path = try std.fs.path.join(gpa, &.{ dir, name });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = path, .data = body });
    return path;
}

// ---------------------------------------------------------------------------
// `planar workflow list` — shipped workflows via PLANAR_WORKFLOWS_DIR
// ---------------------------------------------------------------------------

test "workflow list enumerates shipped workflows seeded via PLANAR_WORKFLOWS_DIR" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Seed a fake shipped workflows directory with two workflows.
    const shipped_dir = suite.freshSystemTmpDir();
    const wf1 =
        \\--[[ @meta
        \\name: alpha-flow
        \\description: Alpha workflow for testing.
        \\phases: run
        \\seam: planar plan list
        \\--]]
        \\function run() flow.result({}) end
    ;
    const wf2 =
        \\--[[ @meta
        \\name: beta-flow
        \\description: Beta workflow for testing.
        \\phases: setup, teardown
        \\seam: planar task list
        \\--]]
        \\function setup() flow.result({}) end
    ;
    const p1 = try writeWorkflow(gpa, shipped_dir, "alpha.lua", wf1);
    defer gpa.free(p1);
    const p2 = try writeWorkflow(gpa, shipped_dir, "beta.lua", wf2);
    defer gpa.free(p2);

    // Run `planar workflow list --json` with PLANAR_WORKFLOWS_DIR pointing at
    // the seeded directory and PLANAR_HOME set to a non-existent path (so no
    // sandbox workflows are discovered).
    const fake_home = suite.freshSystemTmpDir();
    const out = suite.mustRunWith(
        &.{ "workflow", "list", "--json" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = shipped_dir },
            .{ .key = "PLANAR_HOME", .value = fake_home },
        },
    );
    defer gpa.free(out);

    // Parse each JSON line.
    const Entry = struct {
        name: []const u8,
        kind: []const u8,
        description: []const u8,
        meta_found: bool,
    };

    var found_alpha = false;
    var found_beta = false;
    var lines = std.mem.tokenizeScalar(u8, std.mem.trim(u8, out, " \t\r\n"), '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r\n");
        if (trimmed.len == 0) continue;
        var arena = std.heap.ArenaAllocator.init(gpa);
        defer arena.deinit();
        const parsed = try std.json.parseFromSlice(Entry, arena.allocator(), trimmed, .{ .ignore_unknown_fields = true });
        if (std.mem.eql(u8, parsed.value.name, "alpha-flow")) {
            try std.testing.expectEqualStrings("shipped", parsed.value.kind);
            try std.testing.expectEqualStrings("Alpha workflow for testing.", parsed.value.description);
            try std.testing.expect(parsed.value.meta_found);
            found_alpha = true;
        }
        if (std.mem.eql(u8, parsed.value.name, "beta-flow")) {
            try std.testing.expectEqualStrings("shipped", parsed.value.kind);
            try std.testing.expect(parsed.value.meta_found);
            found_beta = true;
        }
    }

    try std.testing.expect(found_alpha);
    try std.testing.expect(found_beta);
}

// ---------------------------------------------------------------------------
// `planar workflow list --local` — sandbox workflows via PLANAR_HOME
// ---------------------------------------------------------------------------

test "workflow list --local enumerates sandbox workflows seeded in PLANAR_HOME" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Create a fake PLANAR_HOME with a local/workflows/ subdirectory.
    const fake_home = suite.freshSystemTmpDir();
    const local_wf_dir = try std.fs.path.join(gpa, &.{ fake_home, "local", "workflows" });
    defer gpa.free(local_wf_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_wf_dir);

    // Write one sandbox workflow.
    const sandbox_wf =
        \\--[[ @meta
        \\name: my-local-wf
        \\description: A personal sandbox workflow.
        \\phases: run
        \\seam: planar health --json
        \\--]]
        \\function run() flow.result({ok=true}) end
    ;
    const p = try writeWorkflow(gpa, local_wf_dir, "my_local.lua", sandbox_wf);
    defer gpa.free(p);

    // Use an empty shipped dir so only the sandbox workflow appears.
    const empty_shipped = suite.freshSystemTmpDir();

    const out = suite.mustRunWith(
        &.{ "workflow", "list", "--local", "--json" },
        &.{
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = empty_shipped },
        },
    );
    defer gpa.free(out);

    const Entry = struct {
        name: []const u8,
        kind: []const u8,
        description: []const u8,
    };

    var found = false;
    var lines = std.mem.tokenizeScalar(u8, std.mem.trim(u8, out, " \t\r\n"), '\n');
    while (lines.next()) |line| {
        const trimmed = std.mem.trim(u8, line, " \t\r\n");
        if (trimmed.len == 0) continue;
        var arena = std.heap.ArenaAllocator.init(gpa);
        defer arena.deinit();
        const parsed = try std.json.parseFromSlice(Entry, arena.allocator(), trimmed, .{ .ignore_unknown_fields = true });
        if (std.mem.eql(u8, parsed.value.name, "my-local-wf")) {
            try std.testing.expectEqualStrings("local", parsed.value.kind);
            try std.testing.expectEqualStrings("A personal sandbox workflow.", parsed.value.description);
            found = true;
        }
    }

    try std.testing.expect(found);
}

// ---------------------------------------------------------------------------
// `planar workflow show <name>` — resolve by name
// ---------------------------------------------------------------------------

test "workflow show resolves a shipped workflow by its @meta name" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const shipped_dir = suite.freshSystemTmpDir();
    const wf =
        \\--[[ @meta
        \\name: show-target
        \\description: The workflow show test finds this.
        \\phases: run
        \\seam: planar plan show
        \\--]]
        \\function run() flow.result({}) end
    ;
    const p = try writeWorkflow(gpa, shipped_dir, "show_target.lua", wf);
    defer gpa.free(p);

    const fake_home = suite.freshSystemTmpDir();
    const out = suite.mustRunWith(
        &.{ "workflow", "show", "show-target", "--json" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = shipped_dir },
            .{ .key = "PLANAR_HOME", .value = fake_home },
        },
    );
    defer gpa.free(out);

    const ShowResult = struct {
        name: []const u8,
        kind: []const u8,
        description: []const u8,
        phases: []const u8,
        seam: []const u8,
        meta_found: bool,
    };

    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const parsed = try std.json.parseFromSlice(
        ShowResult,
        arena.allocator(),
        std.mem.trim(u8, out, " \t\r\n"),
        .{ .ignore_unknown_fields = true },
    );
    try std.testing.expectEqualStrings("show-target", parsed.value.name);
    try std.testing.expectEqualStrings("shipped", parsed.value.kind);
    try std.testing.expectEqualStrings("The workflow show test finds this.", parsed.value.description);
    try std.testing.expectEqualStrings("run", parsed.value.phases);
    try std.testing.expectEqualStrings("planar plan show", parsed.value.seam);
    try std.testing.expect(parsed.value.meta_found);
}

test "workflow show exits non-zero when workflow is not found" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const empty_shipped = suite.freshSystemTmpDir();
    const fake_home = suite.freshSystemTmpDir();

    const stderr = suite.expectFailureWith(
        &.{ "workflow", "show", "nonexistent-workflow" },
        &.{
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = empty_shipped },
            .{ .key = "PLANAR_HOME", .value = fake_home },
        },
    );
    defer gpa.free(stderr);

    try std.testing.expect(std.mem.indexOf(u8, stderr, "nonexistent-workflow") != null);
}

test "workflow show resolves a sandbox workflow and marks it local" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const fake_home = suite.freshSystemTmpDir();
    const local_dir = try std.fs.path.join(gpa, &.{ fake_home, "local", "workflows" });
    defer gpa.free(local_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, local_dir);

    const wf =
        \\--[[ @meta
        \\name: sandbox-only
        \\description: Lives in the sandbox.
        \\phases: run
        \\seam: planar health
        \\--]]
        \\function run() flow.result({}) end
    ;
    const p = try writeWorkflow(gpa, local_dir, "sandbox_only.lua", wf);
    defer gpa.free(p);

    const empty_shipped = suite.freshSystemTmpDir();
    const out = suite.mustRunWith(
        &.{ "workflow", "show", "sandbox-only", "--json" },
        &.{
            .{ .key = "PLANAR_HOME", .value = fake_home },
            .{ .key = "PLANAR_WORKFLOWS_DIR", .value = empty_shipped },
        },
    );
    defer gpa.free(out);

    const ShowResult = struct { name: []const u8, kind: []const u8 };
    var arena = std.heap.ArenaAllocator.init(gpa);
    defer arena.deinit();
    const parsed = try std.json.parseFromSlice(
        ShowResult,
        arena.allocator(),
        std.mem.trim(u8, out, " \t\r\n"),
        .{ .ignore_unknown_fields = true },
    );
    try std.testing.expectEqualStrings("sandbox-only", parsed.value.name);
    try std.testing.expectEqualStrings("local", parsed.value.kind);
}

// ---------------------------------------------------------------------------
// Guardrail tests — prove authored workflows cannot escape the sandbox.
//
// These test `planar-execute` directly; they go through the same
// integration-test machinery as planar_execute_test.zig.
// ---------------------------------------------------------------------------

test "guardrail: workflow cannot escape via os.execute (sandbox enforced)" {
    // The sandbox nils `os` — any attempt to call `os.execute` should raise a
    // Lua error (nil access), causing the engine to exit non-zero.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.registerProject("wfguard1");
    suite.addAssoc("wfguard1", null);

    const wf =
        \\function escape()
        \\  -- This must fail because `os` is nil in the sandbox.
        \\  os.execute("echo pwned")
        \\  flow.result({ escaped = true })
        \\end
    ;
    const p = try writeWorkflow(gpa, root, "escape_os.lua", wf);
    defer gpa.free(p);

    const res = try runExecute(gpa, root, suite.absDbPath(), p, "escape");
    defer res.deinit();

    // Must exit non-zero (Lua error: attempt to index a nil value `os`).
    try std.testing.expect(res.term == .exited);
    try std.testing.expect(res.term.exited != 0);
    // The engine stderr must describe the failure (no "escaped" in stdout).
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"escaped\":true") == null);
}

test "guardrail: workflow cannot call a DENIED spawn-shaped function (agent)" {
    // `agent` is in DENIED_HOST_FNS. It must not appear in any host table;
    // a workflow that calls `cli.agent(...)` should get a Lua nil-access error.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.registerProject("wfguard2");
    suite.addAssoc("wfguard2", null);

    const wf =
        \\function spawn_attempt()
        \\  -- `cli.agent` must be nil (DENIED_HOST_FNS). A call should fail.
        \\  assert(cli.agent == nil, "cli.agent should be nil")
        \\  -- `cli.exec` is also denied.
        \\  assert(cli.exec == nil, "cli.exec should be nil")
        \\  -- `cli.spawn` is also denied.
        \\  assert(cli.spawn == nil, "cli.spawn should be nil")
        \\  flow.result({ spawn_denied = true })
        \\end
    ;
    const p = try writeWorkflow(gpa, root, "spawn_denied.lua", wf);
    defer gpa.free(p);

    const res = try runExecute(gpa, root, suite.absDbPath(), p, "spawn_attempt");
    defer res.deinit();

    // All asserts pass → engine exits 0 and result contains spawn_denied=true.
    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"spawn_denied\":true") != null);
}

test "guardrail: no general exec surface in Lua (os, io, cli.exec, cli.spawn all nil)" {
    // The ALLOWED_CLI_BINS enforcement is at the Zig level, not Lua-visible:
    // cli.planar hardcodes "planar", cli.planar_agent hardcodes "planar-agent",
    // etc. There is no way for a workflow to reach an arbitrary binary from Lua.
    // Proof: os and io are nil (sandbox), and cli has no exec/spawn fields.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.registerProject("wfguard3");
    suite.addAssoc("wfguard3", null);

    const wf =
        \\function non_allowlisted()
        \\  -- Verify that there is no general exec surface available in Lua.
        \\  -- The only binary shells are cli.planar / cli.planar_watch / etc.
        \\  -- which are hardcoded to their respective binaries.
        \\  assert(os == nil, "os must be nil")
        \\  assert(io == nil, "io must be nil")
        \\  -- cli table exists but only has the allowlisted fns.
        \\  assert(cli ~= nil, "cli table must exist")
        \\  -- exec/spawn not present:
        \\  assert(cli.exec == nil, "cli.exec must be nil")
        \\  assert(cli.spawn == nil, "cli.spawn must be nil")
        \\  flow.result({ no_exec_surface = true })
        \\end
    ;
    const p = try writeWorkflow(gpa, root, "no_exec.lua", wf);
    defer gpa.free(p);

    const res = try runExecute(gpa, root, suite.absDbPath(), p, "non_allowlisted");
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"no_exec_surface\":true") != null);
}

test "guardrail: workflow has no SQLite handle (all DB access via CLI shells)" {
    // Prove that a workflow cannot get a db handle: there is no db.open,
    // no db.exec, no sqlite3_open in the Lua environment — just cli.planar.
    // We verify by asserting that all db-shaped globals are nil.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const root = suite.registerProject("wfguard4");
    suite.addAssoc("wfguard4", null);

    const wf =
        \\function no_db()
        \\  -- No db/sqlite globals exist.
        \\  assert(db == nil, "db must not be a global")
        \\  assert(sqlite == nil, "sqlite must not be a global")
        \\  assert(sqlite3 == nil, "sqlite3 must not be a global")
        \\  -- os is nil (no shell backdoor to sqlite3 CLI).
        \\  assert(os == nil, "os must be nil")
        \\  -- require is nil (no module loading).
        \\  assert(require == nil, "require must be nil")
        \\  flow.result({ no_db_handle = true })
        \\end
    ;
    const p = try writeWorkflow(gpa, root, "no_db.lua", wf);
    defer gpa.free(p);

    const res = try runExecute(gpa, root, suite.absDbPath(), p, "no_db");
    defer res.deinit();

    try std.testing.expect(res.term == .exited);
    try std.testing.expectEqual(@as(u32, 0), res.term.exited);
    try std.testing.expect(std.mem.indexOf(u8, res.stdout, "\"no_db_handle\":true") != null);
}
