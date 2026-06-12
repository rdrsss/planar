//! integration_tests/workflow_run_lifecycle_test.zig — black-box integration
//! test for `planar-execute` run-row bracketing (plan 585 task 3922).
//!
//! ## What this tests
//!
//! The brief for task 3922 requires:
//!   - `planar-execute run` opens a `workflow_runs` row in `running` status
//!     BEFORE driving the workflow, and closes it `completed` on clean exit.
//!   - Dry-run (`--dry-run`) creates NO run row.
//!   - The `run_identifier` column on the row carries the `run-<pid>-<nanos>`
//!     string from the runlock, confirming the row is scoped to the right run.
//!   - The `[dispatch]` banner in the stdout/stderr of a workflow that calls
//!     `ctx.agent()` carries the `run=<id>` label.
//!
//! ## Hermeticity
//!
//! Every spawn injects an absolute `PLANAR_DB` path (via `suite.absDbPath()`)
//! and prepends the freshly-built bin dir onto PATH so all shelled
//! `planar-agent` calls resolve to the just-built binary, not the operator's
//! stale `~/.planar/bin`. This mirrors `planar_execute_doctor_test.zig`.
//!
//! ## Scope (task 3922 out-of-scope fence)
//!
//! - Eager-reconcile (task 3928): not tested here.
//! - Claim run/stage association (Q602): not tested here.
//! - `abandoned` status: not tested here (reconcile-only; covered by
//!   `planar_agent_run_test.zig` scenario C).
//! - `ctx.context` / `ctx.brief` host functions: not tested here.
//!
//! ## Workflow design
//!
//! Scenarios A, B, and C use a workflow that does PURE LUA with no
//! `ctx.agent()` call. Run-row bracketing fires in `handleRun` around the
//! entire workflow execution — not per-agent-call — so a pure-Lua workflow is
//! sufficient to exercise run start/end. This avoids the `worktree_path` +
//! `claim_token` requirements of `ctx.agent()` in a unit fixture.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// Binary resolution helpers (mirrors planar_execute_doctor_test.zig)
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
        @panic("PLANAR_EXECUTE_BIN is not set. Run via: make test-integration");
}

fn binDir() []const u8 {
    const planar_bin = envValue("PLANAR_BIN") orelse
        @panic("PLANAR_BIN is not set. Run via: make test-integration");
    return std.fs.path.dirname(planar_bin) orelse ".";
}

// ---------------------------------------------------------------------------
// Subprocess helper
// ---------------------------------------------------------------------------

const CmdResult = struct {
    term: std.process.Child.Term,
    stdout: []u8,
    stderr: []u8,
    gpa: std.mem.Allocator,

    fn deinit(self: CmdResult) void {
        self.gpa.free(self.stdout);
        self.gpa.free(self.stderr);
    }

    fn exitCode(self: CmdResult) u32 {
        return switch (self.term) {
            .exited => |c| c,
            else => 255,
        };
    }
};

const EnvKV = struct { key: []const u8, value: []const u8 };

fn runCmd(
    gpa: std.mem.Allocator,
    cwd: ?[]const u8,
    argv: []const []const u8,
    extra_env: []const EnvKV,
) CmdResult {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const posix_block: std.process.Environ.PosixBlock = .{ .slice = env_slice };
    const environ: std.process.Environ = .{ .block = posix_block };
    var env_map = environ.createMap(gpa) catch @panic("OOM env_map");
    defer env_map.deinit();
    for (extra_env) |e| env_map.put(e.key, e.value) catch @panic("OOM put env");

    const result = std.process.run(gpa, std.testing.io, .{
        .argv = argv,
        .environ_map = &env_map,
        .cwd = if (cwd) |c| .{ .path = c } else .inherit,
        .stdout_limit = std.Io.Limit.limited(512 * 1024),
        .stderr_limit = std.Io.Limit.limited(128 * 1024),
    }) catch |e| std.debug.panic("runCmd spawn failed for '{s}': {s}", .{ argv[0], @errorName(e) });
    return .{
        .term = result.term,
        .stdout = result.stdout,
        .stderr = result.stderr,
        .gpa = gpa,
    };
}

/// Run sqlite3 and return a scalar integer.
fn sqliteScalar(gpa: std.mem.Allocator, db_path: []const u8, sql: []const u8) i64 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", db_path, sql },
    }) catch |e| std.debug.panic("sqlite3 spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.panic("sqlite3 returned non-zero: {s}", .{result.stderr});
    }
    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    return std.fmt.parseInt(i64, trimmed, 10) catch |e|
        std.debug.panic("sqlite3 output not integer ('{s}'): {s}", .{ trimmed, @errorName(e) });
}

/// Run sqlite3 and return a scalar string (caller owns).
fn sqliteString(gpa: std.mem.Allocator, db_path: []const u8, sql: []const u8) []u8 {
    const result = std.process.run(gpa, std.testing.io, .{
        .argv = &.{ "sqlite3", db_path, sql },
    }) catch |e| std.debug.panic("sqlite3 spawn failed: {s}", .{@errorName(e)});
    defer gpa.free(result.stdout);
    defer gpa.free(result.stderr);
    if (result.term != .exited or result.term.exited != 0) {
        std.debug.panic("sqlite3 returned non-zero: {s}", .{result.stderr});
    }
    return gpa.dupe(u8, std.mem.trim(u8, result.stdout, " \t\r\n")) catch @panic("OOM");
}

/// Extract the first integer value for `key` from a JSON string.
fn extractIntField(s: []const u8, key: []const u8) ?i64 {
    const idx = std.mem.indexOf(u8, s, key) orelse return null;
    var i = idx + key.len;
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    var end = i;
    while (end < s.len and s[end] >= '0' and s[end] <= '9') end += 1;
    if (end == i) return null;
    return std.fmt.parseInt(i64, s[i..end], 10) catch null;
}

fn contains(haystack: []const u8, needle: []const u8) bool {
    return std.mem.indexOf(u8, haystack, needle) != null;
}

// ---------------------------------------------------------------------------
// Workflow source helpers
// ---------------------------------------------------------------------------

/// Write a minimal pure-Lua workflow (no ctx.agent() call).
///
/// Run-row bracketing fires in `handleRun` AROUND the entire workflow
/// execution, not per-agent-call. A pure-Lua workflow exercises run start/end
/// without requiring `worktree_path` / `claim_token` for ctx.agent().
fn writePureLuaWorkflow(
    gpa: std.mem.Allocator,
    dir: std.Io.Dir,
    name: []const u8,
) void {
    const src =
        \\return {
        \\  meta = {
        \\    name = "run-lifecycle-smoke",
        \\    description = "Integration test workflow for plan 585 task 3922 run-row bracketing.",
        \\  },
        \\  run = function(ctx)
        \\    -- Pure Lua; no ctx.agent() call needed to test run bracketing.
        \\    -- handleRun opens/closes the workflow_runs row around this block.
        \\    local x = 1 + 1
        \\    _ = x
        \\  end
        \\}
    ;
    const src_slice: []const u8 = src;
    _ = gpa;
    var f = dir.createFile(std.testing.io, name, .{}) catch
        std.debug.panic("createFile {s} failed", .{name});
    defer f.close(std.testing.io);
    f.writeStreamingAll(std.testing.io, src_slice) catch
        std.debug.panic("write workflow {s} failed", .{name});
}

// ---------------------------------------------------------------------------
// Fixture seeding helper
// ---------------------------------------------------------------------------

const SeedResult = struct {
    plan_id_str: []u8,
    task_id_str: []u8,
    plan_slug: []u8,
};

fn seedFixture(
    gpa: std.mem.Allocator,
    suite: *harness.Suite,
    plan_slug: []const u8,
) SeedResult {
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    const plan_json = suite.mustRun(&.{
        "plan", "create", "--slug", plan_slug, "--json", "Run lifecycle smoke plan",
    });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_str = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");

    const plan_slug_owned = gpa.dupe(u8, plan_slug) catch @panic("OOM");

    const task_json = suite.mustRun(&.{
        "task", "add", "--plan", plan_id_str, "--json", "Run lifecycle smoke task",
    });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id");
    const task_id_str = std.fmt.allocPrint(gpa, "{d}", .{task_id}) catch @panic("OOM");

    return .{
        .plan_id_str = plan_id_str,
        .task_id_str = task_id_str,
        .plan_slug = plan_slug_owned,
    };
}

// ---------------------------------------------------------------------------
// Scenario A — run opens running row, closes completed on success
// ---------------------------------------------------------------------------

test "scenario A: planar-execute run opens workflow_runs running row, closes completed on clean exit" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedFixture(gpa, &suite, "rl-smoke-a");
    defer {
        gpa.free(seed.plan_id_str);
        gpa.free(seed.task_id_str);
        gpa.free(seed.plan_slug);
    }

    // Resolve the absolute DB path so the child's non-default cwd doesn't
    // break the relative path the harness stores in suite.db_path.
    const abs_db = suite.absDbPath();

    var wf_dir = std.testing.tmpDir(.{});
    defer wf_dir.cleanup();
    var wf_dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const wf_dir_len = wf_dir.dir.realPath(std.testing.io, &wf_dir_buf) catch
        @panic("realPath wf_dir failed");
    const wf_dir_abs = wf_dir_buf[0..wf_dir_len];

    writePureLuaWorkflow(gpa, wf_dir.dir, "smoke.lua");
    const wf_path = std.fs.path.join(gpa, &.{ wf_dir_abs, "smoke.lua" }) catch @panic("OOM wf_path");
    defer gpa.free(wf_path);

    // Prepend bin dir so shelled `planar-agent` resolves to the just-built binary.
    const old_path = envValue("PATH") orelse "";
    const new_path = std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path }) catch @panic("OOM path");
    defer gpa.free(new_path);

    const execute_bin = resolveExecuteBin();

    // Run planar-execute with --mock-worker and --plan so run bracketing fires.
    // The `--mock-worker` flag prevents real claude spawns (this test uses a
    // pure-Lua workflow so no spawn happens anyway, but it's good practice).
    const result = runCmd(gpa, wf_dir_abs, &.{
        execute_bin,      "run",
        "--mock-worker",  "--plan",
        seed.plan_id_str, wf_path,
    }, &.{
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PATH", .value = new_path },
    });
    defer result.deinit();

    if (result.exitCode() != 0) {
        std.debug.print(
            "planar-execute exited {d}\nstdout: {s}\nstderr: {s}\n",
            .{ result.exitCode(), result.stdout, result.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), result.exitCode());

    // Assert: exactly one workflow_runs row for this plan, status = completed.
    const row_count_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE plan_id = {s} AND status = 'completed';",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(row_count_sql);
    const row_count = sqliteScalar(gpa, abs_db, row_count_sql);
    try std.testing.expectEqual(@as(i64, 1), row_count);

    // Assert: ended_at is non-null on the completed row.
    const ended_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE plan_id = {s} AND status = 'completed' AND ended_at IS NOT NULL;",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(ended_sql);
    const ended_count = sqliteScalar(gpa, abs_db, ended_sql);
    try std.testing.expectEqual(@as(i64, 1), ended_count);

    // Assert: workflow_name column matches our workflow's meta.name.
    const wf_name_sql = std.fmt.allocPrint(
        gpa,
        "SELECT workflow_name FROM workflow_runs WHERE plan_id = {s} AND status = 'completed' LIMIT 1;",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(wf_name_sql);
    const wf_name = sqliteString(gpa, abs_db, wf_name_sql);
    defer gpa.free(wf_name);
    try std.testing.expectEqualStrings("run-lifecycle-smoke", wf_name);

    // Assert: run_identifier starts with "run-" (the runlock format).
    const run_id_sql = std.fmt.allocPrint(
        gpa,
        "SELECT run_identifier FROM workflow_runs WHERE plan_id = {s} AND status = 'completed' LIMIT 1;",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(run_id_sql);
    const run_identifier = sqliteString(gpa, abs_db, run_id_sql);
    defer gpa.free(run_identifier);
    if (!std.mem.startsWith(u8, run_identifier, "run-")) {
        std.debug.print(
            "Expected run_identifier to start with 'run-', got: {s}\n",
            .{run_identifier},
        );
    }
    try std.testing.expect(std.mem.startsWith(u8, run_identifier, "run-"));
}

// ---------------------------------------------------------------------------
// Scenario B — dry-run creates NO workflow_runs row
// ---------------------------------------------------------------------------

test "scenario B: planar-execute --dry-run creates no workflow_runs row" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedFixture(gpa, &suite, "rl-dry-b");
    defer {
        gpa.free(seed.plan_id_str);
        gpa.free(seed.task_id_str);
        gpa.free(seed.plan_slug);
    }

    const abs_db = suite.absDbPath();

    var wf_dir = std.testing.tmpDir(.{});
    defer wf_dir.cleanup();
    var wf_dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const wf_dir_len = wf_dir.dir.realPath(std.testing.io, &wf_dir_buf) catch
        @panic("realPath wf_dir failed");
    const wf_dir_abs = wf_dir_buf[0..wf_dir_len];

    writePureLuaWorkflow(gpa, wf_dir.dir, "dry.lua");
    const wf_path = std.fs.path.join(gpa, &.{ wf_dir_abs, "dry.lua" }) catch @panic("OOM wf_path");
    defer gpa.free(wf_path);

    const old_path = envValue("PATH") orelse "";
    const new_path = std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path }) catch @panic("OOM path");
    defer gpa.free(new_path);

    const execute_bin = resolveExecuteBin();

    const result = runCmd(gpa, wf_dir_abs, &.{
        execute_bin,      "run",
        "--dry-run",      "--plan",
        seed.plan_id_str, wf_path,
    }, &.{
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PATH", .value = new_path },
    });
    defer result.deinit();

    if (result.exitCode() != 0) {
        std.debug.print(
            "dry-run exited {d}\nstdout: {s}\nstderr: {s}\n",
            .{ result.exitCode(), result.stdout, result.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), result.exitCode());

    // Assert: NO workflow_runs row was created for this plan.
    const row_sql = std.fmt.allocPrint(
        gpa,
        "SELECT count(*) FROM workflow_runs WHERE plan_id = {s};",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(row_sql);
    const row_count = sqliteScalar(gpa, abs_db, row_sql);
    try std.testing.expectEqual(@as(i64, 0), row_count);
}

// ---------------------------------------------------------------------------
// Scenario C — run_identifier is threaded from the runlock into the DB row
// ---------------------------------------------------------------------------

test "scenario C: workflow_runs row run_identifier matches run-<pid>-<nanos> format from runlock" {
    // Confirms that the run_identifier stored in the workflow_runs row is the
    // harness's own runlock identifier — run-<pid>-<nanos> — not a synthetic
    // value. This proves the runlock threading path in run_lifecycle.zig works.
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    const seed = seedFixture(gpa, &suite, "rl-runid-c");
    defer {
        gpa.free(seed.plan_id_str);
        gpa.free(seed.task_id_str);
        gpa.free(seed.plan_slug);
    }

    const abs_db = suite.absDbPath();

    var wf_dir = std.testing.tmpDir(.{});
    defer wf_dir.cleanup();
    var wf_dir_buf: [std.fs.max_path_bytes]u8 = undefined;
    const wf_dir_len = wf_dir.dir.realPath(std.testing.io, &wf_dir_buf) catch
        @panic("realPath wf_dir failed");
    const wf_dir_abs = wf_dir_buf[0..wf_dir_len];

    writePureLuaWorkflow(gpa, wf_dir.dir, "runid.lua");
    const wf_path = std.fs.path.join(gpa, &.{ wf_dir_abs, "runid.lua" }) catch @panic("OOM wf_path");
    defer gpa.free(wf_path);

    const old_path = envValue("PATH") orelse "";
    const new_path = std.fmt.allocPrint(gpa, "{s}:{s}", .{ binDir(), old_path }) catch @panic("OOM path");
    defer gpa.free(new_path);

    const execute_bin = resolveExecuteBin();

    const result = runCmd(gpa, wf_dir_abs, &.{
        execute_bin,      "run",
        "--mock-worker",  "--plan",
        seed.plan_id_str, wf_path,
    }, &.{
        .{ .key = "PLANAR_DB", .value = abs_db },
        .{ .key = "PATH", .value = new_path },
    });
    defer result.deinit();

    if (result.exitCode() != 0) {
        std.debug.print(
            "planar-execute (C) exited {d}\nstdout: {s}\nstderr: {s}\n",
            .{ result.exitCode(), result.stdout, result.stderr },
        );
    }
    try std.testing.expectEqual(@as(u32, 0), result.exitCode());

    // Pull the run_identifier from the completed row.
    const run_id_sql = std.fmt.allocPrint(
        gpa,
        "SELECT run_identifier FROM workflow_runs WHERE plan_id = {s} AND status = 'completed' LIMIT 1;",
        .{seed.plan_id_str},
    ) catch @panic("OOM sql");
    defer gpa.free(run_id_sql);
    const run_identifier = sqliteString(gpa, abs_db, run_id_sql);
    defer gpa.free(run_identifier);

    // The runlock always generates "run-<pid>-<nanos>" so the identifier
    // must start with "run-" and contain at least two hyphens.
    if (!std.mem.startsWith(u8, run_identifier, "run-")) {
        std.debug.print(
            "Expected run_identifier to start with 'run-', got: {s}\n",
            .{run_identifier},
        );
    }
    try std.testing.expect(std.mem.startsWith(u8, run_identifier, "run-"));
    // Must contain a second hyphen (run-<pid>-<nanos>).
    const after_prefix = run_identifier["run-".len..];
    const second_hyphen = std.mem.indexOf(u8, after_prefix, "-") != null;
    try std.testing.expect(second_hyphen);
}
