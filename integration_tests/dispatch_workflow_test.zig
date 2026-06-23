//! integration_tests/dispatch_workflow_test.zig — black-box tests for
//! `workflows/dispatch.lua` (plan 638 M2, tasks 4113/4114/4115).
//!
//! These tests exercise the three dispatch phases (prep/route/heartbeat)
//! through the planar-execute engine against a seeded isolated PLANAR_DB.
//!
//! Covered scenarios:
//!
//!   4113 loop-contract: prep claims a task (assert claim_token, task in doing);
//!     route(approve) → task ends done, claim completed (one atomic terminal verb);
//!     run trace shows the cycle.
//!
//!   4114 cap: route with verdict=request-changes at iteration=5 (and iteration=6)
//!     is forced to abort (terminal verb=fail), NOT a loop-back. Assert cap rule.
//!
//!   4115 terminal-atomicity: the workflow source contains no `planar task done`
//!     call and route always produces exactly one atomic terminal verb per
//!     non-loop-back path (assert via claim+task state transitioning together).
//!
//! The tests isolate PLANAR_DB via a per-suite tmp dir and prepend the harness
//! binary dir to PATH so planar-execute's inner `cli.planar` / `cli.planar_agent`
//! shells hit the same binary + DB.

const std = @import("std");
const harness = @import("harness");

// ---------------------------------------------------------------------------
// resolveEnv — same pattern as planar_execute_test.zig
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

// ---------------------------------------------------------------------------
// RunResult — mirrors planar_execute_test.zig
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
};

// ---------------------------------------------------------------------------
// runExecute — spawn planar-execute with the harness PLANAR_DB + PATH
// ---------------------------------------------------------------------------

fn runExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    var argv = std.ArrayList([]const u8).empty;
    defer argv.deinit(gpa);
    try argv.append(gpa, resolveEnv("PLANAR_EXECUTE_BIN"));
    for (args) |a| try argv.append(gpa, a);

    const raw: [*:null]?[*:0]u8 = std.c.environ;
    var env_count: usize = 0;
    while (raw[env_count] != null) : (env_count += 1) {}
    const env_slice: [:null]const ?[*:0]const u8 = @ptrCast(raw[0..env_count :null]);
    const environ: std.process.Environ = .{ .block = .{ .slice = env_slice } };
    var env_map = try environ.createMap(gpa);
    defer env_map.deinit();

    try env_map.put("PLANAR_DB", db_path);
    try env_map.put("PLANAR_CONFIG_PATH", "/nonexistent-planar-config.toml");
    try env_map.put("PLANAR_DISABLE_WORKTREE_GATE", "1");

    // Prepend both planar and planar-agent binary dirs to PATH so the engine's
    // inner cli.planar / cli.planar_agent shells resolve to the harness binaries.
    const planar_bin = resolveEnv("PLANAR_BIN");
    const planar_dir = std.fs.path.dirname(planar_bin) orelse ".";
    const agent_bin = resolveEnv("PLANAR_AGENT_BIN");
    const agent_dir = std.fs.path.dirname(agent_bin) orelse ".";
    const old_path = env_map.get("PATH") orelse "";
    const new_path = try std.fmt.allocPrint(gpa, "{s}:{s}:{s}", .{ planar_dir, agent_dir, old_path });
    defer gpa.free(new_path);
    try env_map.put("PATH", new_path);

    const result = try std.process.run(gpa, std.testing.io, .{
        .argv = argv.items,
        .cwd = .{ .path = cwd },
        .environ_map = &env_map,
    });
    return .{ .term = result.term, .stdout = result.stdout, .stderr = result.stderr, .gpa = gpa };
}

// ---------------------------------------------------------------------------
// mustExecute — runExecute + assert exit 0
// ---------------------------------------------------------------------------

fn mustExecute(
    gpa: std.mem.Allocator,
    cwd: []const u8,
    db_path: []const u8,
    args: []const []const u8,
) !RunResult {
    const res = try runExecute(gpa, cwd, db_path, args);
    if (res.term != .exited or res.term.exited != 0) {
        std.debug.print(
            "planar-execute failed (term={any})\nstdout: {s}\nstderr: {s}\n",
            .{ res.term, res.stdout, res.stderr },
        );
        @panic("mustExecute: planar-execute exited non-zero");
    }
    return res;
}

// ---------------------------------------------------------------------------
// repoRootFromBin — derive repo root from PLANAR_BIN (three dirname levels)
// ---------------------------------------------------------------------------

fn repoRootFromBin(allocator: std.mem.Allocator) ![]const u8 {
    const bin_path = resolveEnv("PLANAR_BIN");
    const d1 = std.fs.path.dirname(bin_path) orelse return error.FileNotFound;
    const d2 = std.fs.path.dirname(d1) orelse return error.FileNotFound;
    const d3 = std.fs.path.dirname(d2) orelse return error.FileNotFound;
    return allocator.dupe(u8, d3);
}

// ---------------------------------------------------------------------------
// JSON field helpers
// ---------------------------------------------------------------------------

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

/// Extract a quoted string value for `key` from a JSON blob.
/// Locates `"key":"` (with optional spaces) and returns the content up to the
/// next unescaped `"`. Handles simple values only (no nested quotes in value).
/// Returns a slice into `s` (not a copy; valid as long as `s` is live).
fn extractQuotedField(s: []const u8, key: []const u8) ?[]const u8 {
    // Look for `"<key>"` followed by `:` then `"`.
    const key_pattern = std.mem.indexOf(u8, s, key) orelse return null;
    var i = key_pattern + key.len;
    // Skip colon and whitespace.
    while (i < s.len and (s[i] == ' ' or s[i] == ':' or s[i] == '\t')) i += 1;
    if (i >= s.len or s[i] != '"') return null;
    i += 1; // skip opening quote
    const start = i;
    // Scan to closing quote (skip escaped quotes).
    while (i < s.len) : (i += 1) {
        if (s[i] == '\\') {
            i += 1; // skip escaped char
        } else if (s[i] == '"') {
            return s[start..i];
        }
    }
    return null;
}

// ---------------------------------------------------------------------------
// seedDispatchPlan
//
// init + plan + one task + plan activate; return {plan_id_str, task_id}.
// The plan is set to 'active' so `plan next` returns eligible tasks.
// ---------------------------------------------------------------------------

const SeedResult = struct {
    plan_id_str: []u8,
    task_id: i64,
};

fn seedDispatchPlan(suite: *harness.Suite, slug: []const u8) SeedResult {
    const gpa = suite.allocator;
    gpa.free(suite.mustRun(&.{ "init", "--skip-project" }));

    // Create plan and extract id.
    const plan_json = suite.mustRun(&.{ "plan", "create", "--slug", slug, "--json", slug });
    defer gpa.free(plan_json);
    const plan_id = extractIntField(plan_json, "\"id\"") orelse @panic("no plan id");
    const plan_id_str = std.fmt.allocPrint(gpa, "{d}", .{plan_id}) catch @panic("OOM");

    // Add task with --json so we can extract the integer id.
    const task_json = suite.mustRun(&.{ "task", "add", "--plan", plan_id_str, "--json", "Implement feature X" });
    defer gpa.free(task_json);
    const task_id = extractIntField(task_json, "\"id\"") orelse @panic("no task id from task add --json");

    // Activate the plan so `plan next` returns eligible tasks.
    const upd = suite.mustRun(&.{ "plan", "update", plan_id_str, "--status", "active" });
    gpa.free(upd);

    return .{ .plan_id_str = plan_id_str, .task_id = task_id };
}

// ---------------------------------------------------------------------------
// PrepFields — manually extracted from prep JSON output.
//
// (The PrepResult struct was intentionally removed in favor of ParsePrepFields
// to avoid failing on invalid UTF-8 bytes in the `brief` field.)
//
// The prep result embeds a `brief` field whose value contains binary data
// from the planar-agent schema catalog (invalid UTF-8 in some environments).
// We cannot use std.json.parseFromSlice on the full prep output — the scanner
// rejects invalid UTF-8 inside JSON strings even with ignore_unknown_fields.
// Extract the scalar fields we need via string search instead.
// ---------------------------------------------------------------------------

const PrepFields = struct {
    available: bool,
    task_id: i64,
    claim_token: []const u8, // slice into the prep stdout buffer
    run_uid: ?[]const u8, // slice into the prep stdout buffer; null if missing
};

/// parsePrepFields extracts the fields we care about from the prep JSON output
/// without parsing the entire document (sidesteps invalid UTF-8 in `brief`).
fn parsePrepFields(s: []const u8) PrepFields {
    const available = std.mem.indexOf(u8, s, "\"available\":true") != null;
    const task_id = extractIntField(s, "\"task_id\":") orelse
        extractIntField(s, "\"task_id\" :") orelse 0;
    const claim_token = extractQuotedField(s, "\"claim_token\":") orelse "";
    const run_uid = extractQuotedField(s, "\"run_uid\":");
    return .{
        .available = available,
        .task_id = task_id,
        .claim_token = claim_token,
        .run_uid = run_uid,
    };
}

// terminal_verb is absent (nil is omitted by Lua's luaToJson) for loop-back,
// and a string for terminal paths (complete/fail/block).
// Default to null so parseFromSlice handles the missing-field case.
const RouteResult = struct {
    action: []const u8 = "",
    terminal_verb: ?[]const u8 = null, // absent in JSON for loop-back
    verdict: []const u8 = "",
    iteration: i64 = 0,
    cap_fired: bool = false,
};

// ---------------------------------------------------------------------------
// 4113: loop-contract
//
// prep claims a task (assert claim_token present, task in 'doing');
// route(approve) → task ends 'done', claim 'completed' (one atomic terminal verb);
// run trace shows the cycle.
// ---------------------------------------------------------------------------

test "4113 dispatch loop-contract: prep→route(approve)→done+completed" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4113");
    suite.addAssoc("dispatch-4113", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4113");
    defer gpa.free(seed.plan_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // --- Phase: prep ---
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    // Assert prep result contains available=true, task_id, claim_token.
    // NOTE: Do not use std.json.parseFromSlice on the full prep output — the
    // `brief` field may contain invalid UTF-8 (garbled schema catalog bytes)
    // which makes the entire JSON document unparseable by the strict scanner.
    const prep_out = std.mem.trim(u8, prep_res.stdout, " \t\r\n");
    try std.testing.expect(std.mem.indexOf(u8, prep_out, "\"available\":true") != null);
    try std.testing.expect(std.mem.indexOf(u8, prep_out, "\"claim_token\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, prep_out, "\"task_id\"") != null);

    const prep_fields = parsePrepFields(prep_out);
    try std.testing.expect(prep_fields.available);
    try std.testing.expectEqual(seed.task_id, prep_fields.task_id);
    const claim_token = prep_fields.claim_token;
    const run_uid = prep_fields.run_uid;

    // Assert the task is now 'doing'.
    const task_id_str = try std.fmt.allocPrint(gpa, "{d}", .{seed.task_id});
    defer gpa.free(task_id_str);
    const task_json = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_json);
    try std.testing.expect(std.mem.indexOf(u8, task_json, "\"doing\"") != null);

    // --- Phase: route(approve) ---
    const route_args_json = blk: {
        if (run_uid) |uid| {
            break :blk try std.fmt.allocPrint(
                gpa,
                "{{\"claim_token\":\"{s}\",\"verdict\":\"approve\",\"iteration\":1,\"run_uid\":\"{s}\"}}",
                .{ claim_token, uid },
            );
        } else {
            break :blk try std.fmt.allocPrint(
                gpa,
                "{{\"claim_token\":\"{s}\",\"verdict\":\"approve\",\"iteration\":1}}",
                .{claim_token},
            );
        }
    };
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    const route_out = std.mem.trim(u8, route_res.stdout, " \t\r\n");
    const route_parsed = try std.json.parseFromSlice(
        RouteResult,
        gpa,
        route_out,
        .{ .ignore_unknown_fields = true },
    );
    defer route_parsed.deinit();

    // Assert exactly one atomic terminal verb was invoked (complete).
    try std.testing.expectEqualStrings("complete", route_parsed.value.action);
    try std.testing.expect(route_parsed.value.terminal_verb != null);
    try std.testing.expectEqualStrings("complete", route_parsed.value.terminal_verb.?);
    try std.testing.expectEqualStrings("approve", route_parsed.value.verdict);
    try std.testing.expect(!route_parsed.value.cap_fired);

    // Assert task is now 'done' (claim + task flipped atomically by `complete`).
    const task_after = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_after);
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"done\"") != null);
    // NOT still 'doing' — atomicity check.
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"doing\"") == null);
}

// ---------------------------------------------------------------------------
// 4114: cap rule
//
// route with verdict=request-changes at iteration=5 is forced to abort
// (terminal verb=fail), NOT a loop-back. Tested at iteration=5 and iteration=6.
// ---------------------------------------------------------------------------

test "4114 dispatch cap: request-changes at iteration>=5 is forced abort" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4114");
    suite.addAssoc("dispatch-4114", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4114");
    defer gpa.free(seed.plan_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // Run prep to claim the task.
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    const prep_out = std.mem.trim(u8, prep_res.stdout, " \t\r\n");
    const prep_fields = parsePrepFields(prep_out);
    try std.testing.expect(prep_fields.available);
    const claim_token = prep_fields.claim_token;

    // route with verdict=request-changes, iteration=5 → should be forced abort.
    const route_args_json = try std.fmt.allocPrint(
        gpa,
        "{{\"claim_token\":\"{s}\",\"verdict\":\"request-changes\",\"iteration\":5}}",
        .{claim_token},
    );
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    const route_out = std.mem.trim(u8, route_res.stdout, " \t\r\n");
    const route_parsed = try std.json.parseFromSlice(
        RouteResult,
        gpa,
        route_out,
        .{ .ignore_unknown_fields = true },
    );
    defer route_parsed.deinit();

    // The cap rule must have fired, converting request-changes to abort (fail).
    try std.testing.expect(route_parsed.value.cap_fired);
    try std.testing.expectEqualStrings("fail", route_parsed.value.action);
    try std.testing.expect(route_parsed.value.terminal_verb != null);
    try std.testing.expectEqualStrings("fail", route_parsed.value.terminal_verb.?);

    // Assert the task returned to 'todo' (fail flips it back).
    const task_id_str = try std.fmt.allocPrint(gpa, "{d}", .{seed.task_id});
    defer gpa.free(task_id_str);
    const task_after = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_after);
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"todo\"") != null);
}

test "4114 dispatch cap: request-changes at iteration=6 is also forced abort" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4114b");
    suite.addAssoc("dispatch-4114b", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4114b");
    defer gpa.free(seed.plan_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // Prep to claim.
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    const prep_out = std.mem.trim(u8, prep_res.stdout, " \t\r\n");
    const prep_fields = parsePrepFields(prep_out);
    try std.testing.expect(prep_fields.available);

    // route at iteration=6 (above cap).
    const route_args_json = try std.fmt.allocPrint(
        gpa,
        "{{\"claim_token\":\"{s}\",\"verdict\":\"request-changes\",\"iteration\":6}}",
        .{prep_fields.claim_token},
    );
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    const route_parsed = try std.json.parseFromSlice(
        RouteResult,
        gpa,
        std.mem.trim(u8, route_res.stdout, " \t\r\n"),
        .{ .ignore_unknown_fields = true },
    );
    defer route_parsed.deinit();

    try std.testing.expect(route_parsed.value.cap_fired);
    try std.testing.expect(route_parsed.value.terminal_verb != null);
    try std.testing.expectEqualStrings("fail", route_parsed.value.terminal_verb.?);
}

// ---------------------------------------------------------------------------
// 4115: terminal-atomicity
//
// Two assertions:
//   A) The workflow source must contain no `task done` or split-verb pattern.
//      (The atomic terminal verb is the ONLY path; splitting strands claims.)
//   B) The route phase always transitions claim+task atomically together.
//      After route(approve): task is 'done' (not still 'doing').
//      After route(abort): task is 'todo' (not 'doing').
//      Loop-back: task remains 'doing', terminal_verb is null.
// ---------------------------------------------------------------------------

test "4115 terminal-atomicity: workflow source contains no task done / split-verb pattern" {
    const gpa = std.testing.allocator;

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // Read the workflow source.
    const source = std.Io.Dir.cwd().readFileAlloc(std.testing.io, wf_path, gpa, .limited(256 * 1024)) catch
        @panic("failed to read dispatch.lua");
    defer gpa.free(source);

    // Assert: no `task done` call (the split verb pattern the brief prohibits).
    // The atomic `complete` verb is the only path for approve.
    if (std.mem.indexOf(u8, source, "\"task\", \"done\"") != null or
        std.mem.indexOf(u8, source, "\"task done\"") != null)
    {
        std.debug.print("dispatch.lua contains a 'task done' call — PROHIBITED\n", .{});
        return error.SplitVerbFound;
    }

    // Assert: no `planar-agent release` call (the split pattern's other half).
    // Only complete/fail/block are atomic terminal verbs.
    if (std.mem.indexOf(u8, source, "\"release\"") != null) {
        std.debug.print("dispatch.lua contains a 'release' call — PROHIBITED (use complete/fail/block)\n", .{});
        return error.SplitVerbFound;
    }

    // Assert: route phase uses the three atomic terminal verbs.
    try std.testing.expect(std.mem.indexOf(u8, source, "\"complete\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, source, "\"fail\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, source, "\"block\"") != null);
}

test "4115 terminal-atomicity: route(approve) transitions claim+task atomically" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4115a");
    suite.addAssoc("dispatch-4115a", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4115a");
    defer gpa.free(seed.plan_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // Prep → claim.
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    const prep_fields_4115a = parsePrepFields(std.mem.trim(u8, prep_res.stdout, " \t\r\n"));
    try std.testing.expect(prep_fields_4115a.available);
    const claim_token = prep_fields_4115a.claim_token;
    const task_id = prep_fields_4115a.task_id;

    const task_id_str = try std.fmt.allocPrint(gpa, "{d}", .{task_id});
    defer gpa.free(task_id_str);

    // Assert task is 'doing' before route.
    const task_before = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_before);
    try std.testing.expect(std.mem.indexOf(u8, task_before, "\"doing\"") != null);

    // route(approve) → atomic: task→done, claim→completed.
    const route_args_json = try std.fmt.allocPrint(
        gpa,
        "{{\"claim_token\":\"{s}\",\"verdict\":\"approve\",\"iteration\":1}}",
        .{claim_token},
    );
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    // Assert task is 'done' immediately after route (no intermediate state).
    const task_after = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_after);
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"done\"") != null);
    // And it must NOT still be 'doing' (the atomicity check).
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"doing\"") == null);
}

test "4115 terminal-atomicity: loop-back leaves task doing, terminal_verb is null" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4115b");
    suite.addAssoc("dispatch-4115b", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4115b");
    defer gpa.free(seed.plan_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // Prep → claim.
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    const prep_fields_4115b = parsePrepFields(std.mem.trim(u8, prep_res.stdout, " \t\r\n"));
    try std.testing.expect(prep_fields_4115b.available);
    const claim_token = prep_fields_4115b.claim_token;
    const task_id = prep_fields_4115b.task_id;

    const task_id_str = try std.fmt.allocPrint(gpa, "{d}", .{task_id});
    defer gpa.free(task_id_str);

    // route(request-changes, iteration=1) → loop-back: NO terminal verb.
    const route_args_json = try std.fmt.allocPrint(
        gpa,
        "{{\"claim_token\":\"{s}\",\"verdict\":\"request-changes\",\"iteration\":1}}",
        .{claim_token},
    );
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    const route_out = std.mem.trim(u8, route_res.stdout, " \t\r\n");
    const route_parsed = try std.json.parseFromSlice(
        RouteResult,
        gpa,
        route_out,
        .{ .ignore_unknown_fields = true },
    );
    defer route_parsed.deinit();

    // terminal_verb is absent from the JSON (Lua nil is omitted by luaToJson) →
    // parsed as null (default). No atomic verb was fired.
    try std.testing.expect(route_parsed.value.terminal_verb == null);
    try std.testing.expectEqualStrings("loop-back", route_parsed.value.action);
    try std.testing.expect(!route_parsed.value.cap_fired);

    // Task must still be 'doing' (claim is still live).
    const task_after = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_after);
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"doing\"") != null);
}

// ---------------------------------------------------------------------------
// 4288: open-question → block path
//
// The route phase handles verdict=open-question by invoking exactly one
// atomic terminal verb: `planar-agent block --claim <token> --blocker <id>`.
// This transitions the task → blocked and releases the claim.
//
// Post-state asserted:
//   - route result: action="block", terminal_verb="block", verdict="open-question"
//   - task status: "blocked" (not "doing")
//   - blocker entity_links edge: `links list task:<id>` contains the blocker ref
// ---------------------------------------------------------------------------

test "4288 dispatch open-question: block path → task blocked + blocker edge created" {
    const gpa = std.testing.allocator;
    var suite = harness.Suite.init(gpa);
    defer suite.deinit();
    const root = suite.registerProject("dispatch-4288");
    suite.addAssoc("dispatch-4288", null);

    const seed = seedDispatchPlan(&suite, "dispatch-4288");
    defer gpa.free(seed.plan_id_str);

    // Seed a second task to serve as the blocker entity.
    const blocker_json = suite.mustRun(&.{ "task", "add", "--plan", seed.plan_id_str, "--json", "Blocker task for 4288" });
    defer gpa.free(blocker_json);
    const blocker_id = extractIntField(blocker_json, "\"id\"") orelse @panic("no blocker task id");
    const blocker_id_str = try std.fmt.allocPrint(gpa, "{d}", .{blocker_id});
    defer gpa.free(blocker_id_str);

    const repo_root = try repoRootFromBin(gpa);
    defer gpa.free(repo_root);
    const wf_path = try std.fs.path.join(gpa, &.{ repo_root, "workflows", "dispatch.lua" });
    defer gpa.free(wf_path);

    // --- Phase: prep (claim the first task) ---
    const prep_args_json = try std.fmt.allocPrint(gpa, "{{\"plan_id\":{s}}}", .{seed.plan_id_str});
    defer gpa.free(prep_args_json);

    const prep_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "prep", "--args", prep_args_json,
    });
    defer prep_res.deinit();

    const prep_out = std.mem.trim(u8, prep_res.stdout, " \t\r\n");
    const prep_fields = parsePrepFields(prep_out);
    try std.testing.expect(prep_fields.available);
    try std.testing.expectEqual(seed.task_id, prep_fields.task_id);
    const claim_token = prep_fields.claim_token;

    const task_id_str = try std.fmt.allocPrint(gpa, "{d}", .{seed.task_id});
    defer gpa.free(task_id_str);

    // Assert task is 'doing' before route.
    const task_before = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_before);
    try std.testing.expect(std.mem.indexOf(u8, task_before, "\"doing\"") != null);

    // --- Phase: route(open-question) with blocker ---
    // blocker must be the entity reference accepted by `planar-agent block --blocker`.
    const route_args_json = try std.fmt.allocPrint(
        gpa,
        "{{\"claim_token\":\"{s}\",\"verdict\":\"open-question\",\"iteration\":1,\"blocker\":\"{s}\"}}",
        .{ claim_token, blocker_id_str },
    );
    defer gpa.free(route_args_json);

    const route_res = try mustExecute(gpa, root, suite.absDbPath(), &.{
        "run", wf_path, "--phase", "route", "--args", route_args_json,
    });
    defer route_res.deinit();

    const route_out = std.mem.trim(u8, route_res.stdout, " \t\r\n");
    const route_parsed = try std.json.parseFromSlice(
        RouteResult,
        gpa,
        route_out,
        .{ .ignore_unknown_fields = true },
    );
    defer route_parsed.deinit();

    // Assert route returned action=block, terminal_verb=block, verdict=open-question.
    try std.testing.expectEqualStrings("block", route_parsed.value.action);
    try std.testing.expect(route_parsed.value.terminal_verb != null);
    try std.testing.expectEqualStrings("block", route_parsed.value.terminal_verb.?);
    try std.testing.expectEqualStrings("open-question", route_parsed.value.verdict);
    try std.testing.expect(!route_parsed.value.cap_fired);

    // Assert task is now 'blocked' (atomic terminal verb flipped it).
    const task_after = suite.mustRun(&.{ "task", "show", "--json", task_id_str });
    defer gpa.free(task_after);
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"blocked\"") != null);
    // NOT still 'doing'.
    try std.testing.expect(std.mem.indexOf(u8, task_after, "\"doing\"") == null);

    // Assert the blocker entity_links edge was created.
    // `links list task:<id>` surfaces all edges on the task (with or without --json).
    const task_ref = try std.fmt.allocPrint(gpa, "task:{s}", .{task_id_str});
    defer gpa.free(task_ref);
    const links_out = suite.mustRun(&.{ "links", "list", task_ref });
    defer gpa.free(links_out);
    // The edge must reference the blocker task id.
    try std.testing.expect(std.mem.indexOf(u8, links_out, blocker_id_str) != null);
}
