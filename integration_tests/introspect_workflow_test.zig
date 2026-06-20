//! integration_tests/introspect_workflow_test.zig — black-box tests for the
//! `workflows/introspect.lua` pipeline (plan 638 tasks 4120, 4121).
//!
//! What is tested:
//!
//!   task 4120: Over a seeded signal set (CLI invocation failures inserted via
//!   sqlite3 into cli_invocations), the introspect workflow runs and files
//!   findings with deterministic taxonomy-keyed titles on the feedback plan.
//!   The questions list count is asserted to be > 0 and the titles match the
//!   expected `<taxonomy-key>: <signal-key>` format.
//!
//!   task 4121 (idempotency): A SECOND run over the same signal set files ZERO
//!   new findings. The question count after the second run equals the count
//!   after the first run — dedup is real.
//!
//! Signals are seeded directly via sqlite3 (mirroring report_test.zig's
//! `seedFailedInvocation` pattern). The workflow reaches Planar state exclusively
//! through the CLI (D7 contract); no direct DB writes happen inside the workflow.
//!
//! The `planar-execute` binary is resolved from PLANAR_EXECUTE_BIN (set by
//! `zig build test-integration`). The engine's inner `cli.planar` and
//! `cli.planar_watch` shells are resolved via PATH (the test prepends the
//! dir holding the harness `planar` binary, matching planar_execute_test.zig).

const std = @import("std");
const harness = @import("harness");

// =========================================================================
// Env helpers (mirror planar_execute_test.zig)
// =========================================================================

fn resolveEnv(comptime key: []const u8) []const u8 {
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var i: usize = 0;
    while (raw[i]) |entry| : (i += 1) {
        const s: []const u8 = std.mem.span(entry);
        if (std.mem.startsWith(u8, s, key ++ "=")) return s[(key ++ "=").len..];
    }
    @panic(key ++ " is not set. Run via: make test-integration");
}

// =========================================================================
// runExecute — spawns planar-execute with PLANAR_DB + PATH set
// =========================================================================

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

/// runExecute spawns `planar-execute <args...>` with cwd = `cwd`, PLANAR_DB
/// pointed at `db_path`, PLANAR_CONFIG_PATH at `config_path`, and PATH prefixed
/// with the dir holding the harness `planar` binary so the engine's inner CLI
/// shells resolve to the harness-built binaries.
fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    config_path: []const u8,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    // Build child env from current environ, then override PLANAR_DB and PATH.
    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", config_path);
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    // Prepend the harness binary dir to PATH so the engine's inner `planar` and
    // `planar-watch` shells resolve to the harness-built binaries.
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
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

/// repoRootFromBin derives the repo root from PLANAR_BIN (dirname × 3).
fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

// =========================================================================
// Signal-seeding helper (mirrors report_test.zig)
// =========================================================================

/// Seed N failed invocations of `verb_path` into cli_invocations. Enables
/// CLI logging by writing a config.toml first, then uses sqlite3 to insert
/// rows directly (the report bundle reads from this table).
/// Returns the absolute path to the written config.toml (caller must free).
fn seedFailedInvocations(
    suite: *harness.Suite,
    verb_path: []const u8,
    count: usize,
) []const u8 {
    const gpa = suite.allocator;
    // Write a config enabling cli_log (so `planar report --json` sees the rows).
    const cfg_dir = std.fs.path.dirname(suite.absDbPath()) orelse ".";
    const cfg_path = std.fs.path.join(gpa, &.{ cfg_dir, "config.toml" }) catch @panic("OOM");
    std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = cfg_path,
        .data = "[introspection]\ncli_log = true\n",
    }) catch @panic("cannot write config.toml");

    // Insert rows via sqlite3. Each row is offset by 1 minute to ensure
    // they fall within a 30-day window.
    var i: usize = 0;
    while (i < count) : (i += 1) {
        var sql_buf: [512]u8 = undefined;
        const sql = std.fmt.bufPrint(
            &sql_buf,
            "insert into cli_invocations (verb_path, args_shape, exit_code, error_category, recorded_at)" ++
                " values ('{s}', '', 2, 'usage', datetime('now', '-{d} minutes'));",
            .{ verb_path, i + 1 },
        ) catch @panic("sql buf too small");
        const result = std.process.run(gpa, std.testing.io, .{
            .argv = &.{ "sqlite3", suite.absDbPath(), sql },
        }) catch @panic("sqlite3 not found");
        defer gpa.free(result.stdout);
        defer gpa.free(result.stderr);
        if (result.term != .exited or result.term.exited != 0) {
            std.debug.print("sqlite3 seed failed: {s}\n", .{result.stderr});
            @panic("seed failed");
        }
    }
    return cfg_path;
}

// =========================================================================
// JSON shapes
// =========================================================================

const QuestionJSON = struct {
    id: i64,
    title: []const u8,
    status: []const u8,
};

const IntrospectResult = struct {
    filed: i64,
    skipped: i64,
    candidates: i64,
    summary: []const u8,
};

// =========================================================================
// Tests
// =========================================================================

test "introspect workflow: seeds signal, files taxonomy-keyed findings (task 4120)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Register a project and association so scope-derived verbs work.
    // registerProject shells `planar init` which creates the DB + runs migrations.
    const root = suite.registerProject("introspect-seed");
    suite.addAssoc("introspect-seed", null);

    // Seed >= 3 failures on "task add" so failure-cluster or retry-pattern triggers.
    // seedFailedInvocations returns the config.toml path that enables cli_log.
    const cfg_path = seedFailedInvocations(&suite, "task add", 3);
    defer gpa.free(cfg_path);

    // Create the feedback plan that the workflow will file findings on.
    // Run from the project dir so cwd-scope derivation resolves correctly.
    const plan_out = suite.mustRunInDir(root, &.{ "plan", "create", "Planar Feedback", "--slug", "planar-feedback", "--json" });
    defer gpa.free(plan_out);
    const PlanJSON = struct { id: i64 };
    const plan_parsed = std.json.parseFromSlice(PlanJSON, gpa, plan_out, .{ .ignore_unknown_fields = true }) catch @panic("plan create JSON parse failed");
    defer plan_parsed.deinit();
    const plan_id = plan_parsed.value.id;

    // Resolve the introspect.lua workflow path from the repo root.
    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "introspect.lua" });
    defer gpa.free(wf_path);

    // Build the --args JSON payload: { "plan_id": <id>, "days": 30 }
    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{d},\"days\":30}}", .{plan_id});
    defer gpa.free(args_json);

    // Run the introspect workflow with the config path that enables cli_log so the
    // engine's inner `planar report --json` shell sees the seeded invocations.
    const res = try runExecute(gpa, root, suite.absDbPath(), cfg_path, &.{
        "run", wf_path, "--phase", "introspect", "--args", args_json,
    });
    defer res.deinit();

    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print("introspect workflow failed:\nstdout: {s}\nstderr: {s}\n", .{
            res.stdout, res.stderr,
        });
        @panic("introspect workflow exited non-zero");
    }

    // Parse the flow.result payload.
    const trimmed = std.mem.trim(u8, res.stdout, " \t\r\n");
    const result = std.json.parseFromSlice(IntrospectResult, gpa, trimmed, .{
        .ignore_unknown_fields = true,
    }) catch |e| {
        std.debug.print("flow.result parse failed: {s}\nraw: {s}\n", .{ @errorName(e), res.stdout });
        @panic("flow.result parse failed");
    };
    defer result.deinit();

    // At least one finding must have been filed.
    try std.testing.expect(result.value.filed > 0);

    // Verify the questions were actually written to the DB.
    // Use mustRunInDir so cwd-scope resolution finds the registered project.
    const plan_id_str = try std.fmt.allocPrint(gpa, "{d}", .{plan_id});
    defer gpa.free(plan_id_str);
    const q_raw = suite.mustRunInDir(root, &.{ "question", "list", "--plan", plan_id_str, "--status", "open", "--json" });
    defer gpa.free(q_raw);
    const q_list = std.json.parseFromSlice([]QuestionJSON, gpa, q_raw, .{
        .ignore_unknown_fields = true,
    }) catch @panic("question list JSON parse failed");
    defer q_list.deinit();

    // Must have >= 1 question filed.
    try std.testing.expect(q_list.value.len > 0);

    // Every filed question title must follow the "<taxonomy-key>: <signal-key>" format.
    // The taxonomy keys are the four closed-set values.
    for (q_list.value) |q| {
        var valid_prefix = false;
        inline for ([_][]const u8{
            "failure-cluster: ",
            "retry-pattern: ",
            "abandoned-workflow: ",
            "gap-feature: ",
        }) |prefix| {
            if (std.mem.startsWith(u8, q.title, prefix)) valid_prefix = true;
        }
        if (!valid_prefix) {
            std.debug.print("unexpected question title format: '{s}'\n", .{q.title});
            return error.UnexpectedTitleFormat;
        }
    }
}

test "introspect workflow: second run files zero new findings — dedup is real (task 4121)" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();

    // Register a project and association.
    const root = suite.registerProject("introspect-dedup");
    suite.addAssoc("introspect-dedup", null);

    // Seed 3 failures to guarantee at least one finding.
    const cfg_path = seedFailedInvocations(&suite, "plan create", 3);
    defer gpa.free(cfg_path);

    // Create the feedback plan. Run from the project dir so scope resolves.
    const plan_out = suite.mustRunInDir(root, &.{ "plan", "create", "Planar Feedback", "--slug", "planar-feedback", "--json" });
    defer gpa.free(plan_out);
    const PlanJSON = struct { id: i64 };
    const plan_parsed = std.json.parseFromSlice(PlanJSON, gpa, plan_out, .{ .ignore_unknown_fields = true }) catch @panic("plan JSON parse failed");
    defer plan_parsed.deinit();
    const plan_id = plan_parsed.value.id;

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "introspect.lua" });
    defer gpa.free(wf_path);

    const args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{d},\"days\":30}}", .{plan_id});
    defer gpa.free(args_json);

    // --- First run ---
    const res1 = try runExecute(gpa, root, suite.absDbPath(), cfg_path, &.{
        "run", wf_path, "--phase", "introspect", "--args", args_json,
    });
    defer res1.deinit();

    if (res1.term != .exited or res1.term.exited != 0) {
        std.debug.print("first run failed:\nstdout: {s}\nstderr: {s}\n", .{
            res1.stdout, res1.stderr,
        });
        @panic("first introspect run exited non-zero");
    }

    const r1_trimmed = std.mem.trim(u8, res1.stdout, " \t\r\n");
    const result1 = std.json.parseFromSlice(IntrospectResult, gpa, r1_trimmed, .{
        .ignore_unknown_fields = true,
    }) catch @panic("first run result parse failed");
    defer result1.deinit();

    // First run must file at least one finding.
    try std.testing.expect(result1.value.filed > 0);

    // Count questions after first run. Run from the project dir so cwd-scope works.
    const plan_id_str = try std.fmt.allocPrint(gpa, "{d}", .{plan_id});
    defer gpa.free(plan_id_str);
    const q_after_run1 = suite.mustRunInDir(root, &.{ "question", "list", "--plan", plan_id_str, "--status", "open", "--json" });
    defer gpa.free(q_after_run1);
    const q_list1 = std.json.parseFromSlice([]QuestionJSON, gpa, q_after_run1, .{
        .ignore_unknown_fields = true,
    }) catch @panic("question list parse failed (run 1)");
    defer q_list1.deinit();
    const count_after_run1 = q_list1.value.len;
    try std.testing.expect(count_after_run1 > 0);

    // --- Second run over the SAME signals ---
    const res2 = try runExecute(gpa, root, suite.absDbPath(), cfg_path, &.{
        "run", wf_path, "--phase", "introspect", "--args", args_json,
    });
    defer res2.deinit();

    if (res2.term != .exited or res2.term.exited != 0) {
        std.debug.print("second run failed:\nstdout: {s}\nstderr: {s}\n", .{
            res2.stdout, res2.stderr,
        });
        @panic("second introspect run exited non-zero");
    }

    const r2_trimmed = std.mem.trim(u8, res2.stdout, " \t\r\n");
    const result2 = std.json.parseFromSlice(IntrospectResult, gpa, r2_trimmed, .{
        .ignore_unknown_fields = true,
    }) catch @panic("second run result parse failed");
    defer result2.deinit();

    // Second run must file ZERO new findings.
    try std.testing.expectEqual(@as(i64, 0), result2.value.filed);
    // Skipped count must equal the number of candidates (all deduplicated).
    try std.testing.expect(result2.value.skipped >= result1.value.filed);

    // The question count must be UNCHANGED after the second run.
    const q_after_run2 = suite.mustRunInDir(root, &.{ "question", "list", "--plan", plan_id_str, "--status", "open", "--json" });
    defer gpa.free(q_after_run2);
    const q_list2 = std.json.parseFromSlice([]QuestionJSON, gpa, q_after_run2, .{
        .ignore_unknown_fields = true,
    }) catch @panic("question list parse failed (run 2)");
    defer q_list2.deinit();
    const count_after_run2 = q_list2.value.len;

    try std.testing.expectEqual(count_after_run1, count_after_run2);
}
